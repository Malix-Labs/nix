#include "nix/store/oci-binary-cache-store.hh"
#include "nix/store/filetransfer.hh"
#include "nix/store/globals.hh"
#include "nix/store/nar-info-disk-cache.hh"
#include "nix/util/callback.hh"
#include "nix/util/hash.hh"
#include "nix/store/store-registration.hh"

#include <nlohmann/json.hpp>

namespace nix {

MakeError(UploadToOCI, Error);

OCIBinaryCacheStoreConfig::OCIBinaryCacheStoreConfig(ParsedURL _cacheUri, const Params & params)
    : StoreConfig(params, FilePathType::Unix)
    , BinaryCacheStoreConfig(params)
    , cacheUri(std::move(_cacheUri))
{
    if (!cacheUri.authority || cacheUri.authority->host.empty())
        throw UsageError("`oci` Store requires a non-empty registry host in Store URL");

    registryHost = cacheUri.authority->host;
    if (cacheUri.authority->port)
        registryHost += ":" + std::to_string(*cacheUri.authority->port);

    // Build the repository path from the URL path segments
    std::string repo;
    for (auto & segment : cacheUri.path) {
        if (segment.empty())
            continue;
        if (!repo.empty())
            repo += "/";
        repo += segment;
    }
    if (repo.empty())
        throw UsageError("`oci` Store requires a non-empty repository path in Store URL (e.g. oci://registry/user/repo)");
    repository = repo;
}

std::string OCIBinaryCacheStoreConfig::doc()
{
    return
#include "oci-binary-cache-store.md"
        ;
}

StoreReference OCIBinaryCacheStoreConfig::getReference() const
{
    return {
        .variant =
            StoreReference::Specified{
                .scheme = cacheUri.scheme,
                .authority = cacheUri.renderAuthorityAndPath(),
            },
        .params = getQueryParams(),
    };
}

OCIBinaryCacheStore::OCIBinaryCacheStore(ref<Config> config, ref<FileTransfer> fileTransfer)
    : Store{*config}
    , BinaryCacheStore{*config}
    , config{config}
    , fileTransfer{fileTransfer}
{
    diskCache = NarInfoDiskCache::get(settings.getNarInfoDiskCacheSettings(), {.useWAL = settings.useSQLiteWAL});
}

void OCIBinaryCacheStore::init()
{
    auto cacheKey = config->getReference().render(/*withParams=*/false);

    if (auto cacheInfo = diskCache->upToDateCacheExists(cacheKey)) {
        config->wantMassQuery.setDefault(cacheInfo->wantMassQuery);
        config->priority.setDefault(cacheInfo->priority);
    } else {
        try {
            BinaryCacheStore::init();
        } catch (UploadToOCI &) {
            throw Error("'%s' does not appear to be a binary cache", config->cacheUri.to_string());
        }
        diskCache->createCache(cacheKey, config->storeDir, config->wantMassQuery, config->priority);
    }
}

std::string OCIBinaryCacheStore::pathToTag(const std::string & path)
{
    // OCI tags must match [a-zA-Z0-9_.-]{1,128}
    // Replace '/' with '__' to keep tags reversible.
    // Replace any other invalid characters with '_'.
    std::string tag;
    tag.reserve(path.size());
    for (char c : path) {
        if (c == '/') {
            tag += "__";
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'
                   || c == '_') {
            tag += c;
        } else {
            tag += '_';
        }
    }

    // OCI tags have a max length of 128 characters. If we exceed it,
    // hash the original path to produce a deterministic short tag.
    if (tag.size() > 128) {
        auto hash = hashString(HashAlgorithm::SHA256, path);
        tag = "nix-" + hash.to_string(HashFormat::Base32, false);
        if (tag.size() > 128)
            tag = tag.substr(0, 128);
    }
    return tag;
}

std::string OCIBinaryCacheStore::apiBase()
{
    return "https://" + config->registryHost + "/v2/" + config->repository;
}

std::string OCIBinaryCacheStore::fetchToken(const std::string & scope)
{
    // First, check the cache.
    {
        auto cache = tokenCache.lock();
        auto it = cache->tokens.find(scope);
        if (it != cache->tokens.end())
            return it->second;
    }

    // Try an unauthenticated request to get the WWW-Authenticate challenge.
    std::string wwwAuth;
    try {
        FileTransferRequest pingReq(apiBase() + "/");
        pingReq.method = HttpMethod::Get;
        fileTransfer->download(pingReq);
        // If we get here, the registry doesn't require authentication.
        return "";
    } catch (FileTransferError & e) {
        // We need the WWW-Authenticate header from the 401 response.
        // The Nix FileTransfer API doesn't expose response headers directly,
        // so we parse the error message or use a fallback approach.
        //
        // For known registries, we use well-known token endpoints.
        // This is the standard approach used by Docker/OCI clients.
    }

    // Construct the token URL based on the registry.
    // The OCI distribution spec says registries should return a
    // WWW-Authenticate: Bearer realm="<url>",service="<service>",scope="<scope>"
    // header on 401, but since FileTransfer doesn't expose response headers,
    // we use well-known token endpoints for common registries.
    std::string tokenUrl;

    if (config->registryHost == "ghcr.io") {
        tokenUrl = "https://ghcr.io/token?scope=" + percentEncode(scope)
            + "&service=ghcr.io";
    } else if (config->registryHost == "registry-1.docker.io" || config->registryHost == "docker.io") {
        tokenUrl = "https://auth.docker.io/token?scope=" + percentEncode(scope)
            + "&service=registry.docker.io";
    } else {
        // Generic fallback: try the /v2/ endpoint's realm
        // Many registries support a token endpoint at /v2/token
        tokenUrl = "https://" + config->registryHost + "/v2/token?scope=" + percentEncode(scope)
            + "&service=" + percentEncode(config->registryHost);
    }

    try {
        FileTransferRequest tokenReq(tokenUrl);
        tokenReq.method = HttpMethod::Get;
        auto result = fileTransfer->download(tokenReq);

        auto json = nlohmann::json::parse(result.data);
        std::string token;
        if (json.contains("token"))
            token = json["token"].get<std::string>();
        else if (json.contains("access_token"))
            token = json["access_token"].get<std::string>();

        if (!token.empty()) {
            auto cache = tokenCache.lock();
            cache->tokens[scope] = token;
        }
        return token;
    } catch (std::exception & e) {
        debug("failed to fetch OCI token from '%s': %s", tokenUrl, e.what());
        return "";
    }
}

FileTransferRequest OCIBinaryCacheStore::makeRequest(const std::string & url, const std::string & scope)
{
    FileTransferRequest request(url);

    auto token = fetchToken(scope);
    if (!token.empty()) {
        request.headers.push_back({"Authorization", "Bearer " + token});
    }

    return request;
}

std::string OCIBinaryCacheStore::uploadBlob(const std::string & data, const std::string & mediaType)
{
    auto hash = hashString(HashAlgorithm::SHA256, data);
    std::string digest = "sha256:" + hash.to_string(HashFormat::Base16, false);

    auto scope = "repository:" + config->repository + ":pull,push";

    // Step 1: Check if blob already exists (by digest).
    try {
        auto req = makeRequest(apiBase() + "/blobs/" + digest, scope);
        req.method = HttpMethod::Head;
        fileTransfer->download(req);
        // Blob already exists, no need to upload.
        return digest;
    } catch (FileTransferError & e) {
        if (e.error != FileTransfer::NotFound && e.error != FileTransfer::Forbidden)
            throw;
    }

    // Step 2: Monolithic upload via POST with digest query parameter.
    // This is the simplest OCI upload flow: a single POST with the blob data
    // and the digest, which avoids needing to parse the Location header from
    // the 202 response of a two-step upload.
    auto uploadUrl = apiBase() + "/blobs/uploads/?digest=" + percentEncode(digest);

    auto uploadReq = makeRequest(uploadUrl, scope);
    uploadReq.method = HttpMethod::Post;
    uploadReq.mimeType = "application/octet-stream";
    StringSource source(data);
    uploadReq.data = {data.size(), source};
    fileTransfer->upload(uploadReq);

    return digest;
}

void OCIBinaryCacheStore::pushManifest(const std::string & tag, const std::string & blobDigest, uint64_t blobSize)
{
    auto scope = "repository:" + config->repository + ":pull,push";

    // We need a config blob for the manifest. Use an empty JSON object.
    std::string emptyConfig = "{}";
    auto configDigest = uploadBlob(emptyConfig, "application/vnd.oci.image.config.v1+json");

    // Build the OCI image manifest.
    nlohmann::json manifest = {
        {"schemaVersion", 2},
        {"mediaType", "application/vnd.oci.image.manifest.v1+json"},
        {"config",
         {
             {"mediaType", "application/vnd.oci.image.config.v1+json"},
             {"digest", configDigest},
             {"size", emptyConfig.size()},
         }},
        {"layers",
         {{
             {"mediaType", "application/octet-stream"},
             {"digest", blobDigest},
             {"size", blobSize},
         }}},
    };

    auto manifestData = manifest.dump();

    auto req = makeRequest(apiBase() + "/manifests/" + tag, scope);
    req.method = HttpMethod::Put;
    req.mimeType = "application/vnd.oci.image.manifest.v1+json";
    StringSource source(manifestData);
    req.data = {manifestData.size(), source};
    fileTransfer->upload(req);
}

std::string OCIBinaryCacheStore::resolveManifest(const std::string & tag)
{
    auto scope = "repository:" + config->repository + ":pull";

    auto req = makeRequest(apiBase() + "/manifests/" + tag, scope);
    req.method = HttpMethod::Get;
    req.headers.push_back({"Accept", "application/vnd.oci.image.manifest.v1+json"});
    auto result = fileTransfer->download(req);

    auto manifest = nlohmann::json::parse(result.data);

    auto & layers = manifest["layers"];
    if (layers.empty())
        throw Error("OCI manifest for tag '%s' has no layers", tag);

    return layers[0]["digest"].get<std::string>();
}

bool OCIBinaryCacheStore::fileExists(const std::string & path)
{
    auto tag = pathToTag(path);
    auto scope = "repository:" + config->repository + ":pull";

    try {
        auto req = makeRequest(apiBase() + "/manifests/" + tag, scope);
        req.method = HttpMethod::Head;
        req.headers.push_back({"Accept", "application/vnd.oci.image.manifest.v1+json"});
        fileTransfer->download(req);
        return true;
    } catch (FileTransferError & e) {
        if (e.error == FileTransfer::NotFound || e.error == FileTransfer::Forbidden)
            return false;
        throw;
    }
}

void OCIBinaryCacheStore::upsertFile(
    const std::string & path, RestartableSource & source, const std::string & mimeType, uint64_t sizeHint)
{
    try {
        // Read all data from the source.
        auto data = source.drain();

        auto tag = pathToTag(path);

        // Upload the content as an OCI blob.
        auto digest = uploadBlob(data, mimeType);

        // Create an OCI manifest wrapping the blob, tagged with the path.
        pushManifest(tag, digest, data.size());
    } catch (FileTransferError & e) {
        UploadToOCI err(e.message());
        err.addTrace({}, "while uploading to OCI binary cache at '%s'", config->cacheUri.to_string());
        throw err;
    }
}

void OCIBinaryCacheStore::getFile(const std::string & path, Sink & sink)
{
    auto tag = pathToTag(path);
    auto scope = "repository:" + config->repository + ":pull";

    try {
        // Step 1: Resolve the tag to get the blob digest.
        auto blobDigest = resolveManifest(tag);

        // Step 2: Download the blob.
        auto req = makeRequest(apiBase() + "/blobs/" + blobDigest, scope);
        req.method = HttpMethod::Get;
        fileTransfer->download(std::move(req), sink);
    } catch (FileTransferError & e) {
        if (e.error == FileTransfer::NotFound || e.error == FileTransfer::Forbidden)
            throw NoSuchBinaryCacheFile(
                "file '%s' does not exist in binary cache '%s'", path, config->getHumanReadableURI());
        throw;
    }
}

void OCIBinaryCacheStore::getFile(const std::string & path, Callback<std::optional<std::string>> callback) noexcept
{
    try {
        auto tag = pathToTag(path);
        auto scope = "repository:" + config->repository + ":pull";

        // Resolve manifest to find the blob digest.
        std::string blobDigest;
        try {
            blobDigest = resolveManifest(tag);
        } catch (FileTransferError & e) {
            if (e.error == FileTransfer::NotFound || e.error == FileTransfer::Forbidden)
                return callback({});
            throw;
        }

        // Download the blob.
        auto req = makeRequest(apiBase() + "/blobs/" + blobDigest, scope);
        req.method = HttpMethod::Get;

        fileTransfer->enqueueFileTransfer(req, {[callback{std::move(callback)}](std::future<FileTransferResult> result) mutable {
            try {
                callback(std::move(result.get().data));
            } catch (FileTransferError & e) {
                if (e.error == FileTransfer::NotFound || e.error == FileTransfer::Forbidden)
                    return callback({});
                callback.rethrow();
            } catch (...) {
                callback.rethrow();
            }
        }});
    } catch (...) {
        callback.rethrow();
    }
}

std::optional<TrustedFlag> OCIBinaryCacheStore::isTrustedClient()
{
    return std::nullopt;
}

ref<Store> OCIBinaryCacheStoreConfig::openStore() const
{
    auto store = make_ref<OCIBinaryCacheStore>(
        ref{std::const_pointer_cast<OCIBinaryCacheStore::Config>(shared_from_this())});
    store->init();
    return store;
}

static RegisterStoreImplementation<OCIBinaryCacheStore::Config> regOCIBinaryCacheStore;

} // namespace nix
