#pragma once
///@file

#include "nix/util/url.hh"
#include "nix/store/binary-cache-store.hh"
#include "nix/store/filetransfer.hh"
#include "nix/util/sync.hh"

#include <nlohmann/json_fwd.hpp>

namespace nix {

struct OCIBinaryCacheStoreConfig : std::enable_shared_from_this<OCIBinaryCacheStoreConfig>,
                                   virtual Store::Config,
                                   BinaryCacheStoreConfig
{
    OCIBinaryCacheStoreConfig(const Params & params)
        : StoreConfig(params, FilePathType::Unix)
        , BinaryCacheStoreConfig(params)
    {
    }

    OCIBinaryCacheStoreConfig(ParsedURL cacheUri, const Store::Config::Params & params);

    /**
     * The parsed OCI registry URL.
     *
     * For example, `oci://ghcr.io/user/nix-cache` is parsed into:
     * - registry: `ghcr.io`
     * - repository: `user/nix-cache`
     */
    ParsedURL cacheUri;

    /**
     * The OCI registry hostname (e.g. `ghcr.io`).
     */
    std::string registryHost;

    /**
     * The OCI repository path (e.g. `user/nix-cache`).
     */
    std::string repository;

    static const std::string name()
    {
        return "OCI Binary Cache Store";
    }

    static StringSet uriSchemes()
    {
        return {"oci"};
    }

    static std::optional<ExperimentalFeature> experimentalFeature()
    {
        return ExperimentalFeature::OCIBinaryCacheStore;
    }

    static std::string doc();

    ref<Store> openStore() const override;

    StoreReference getReference() const override;
};

class OCIBinaryCacheStore : public virtual BinaryCacheStore
{
public:

    using Config = OCIBinaryCacheStoreConfig;

    ref<Config> config;

    OCIBinaryCacheStore(ref<Config> config, ref<FileTransfer> fileTransfer = getFileTransfer());

    void init() override;

protected:

    ref<FileTransfer> fileTransfer;

    bool fileExists(const std::string & path) override;

    void upsertFile(
        const std::string & path, RestartableSource & source, const std::string & mimeType, uint64_t sizeHint) override;

    void getFile(const std::string & path, Sink & sink) override;

    void getFile(const std::string & path, Callback<std::optional<std::string>> callback) noexcept override;

    std::optional<TrustedFlag> isTrustedClient() override;

private:

    /**
     * Convert a binary cache file path to an OCI-compatible tag.
     *
     * OCI tags must match `[a-zA-Z0-9_.-]{1,128}`.
     * We replace `/` with `__` to ensure valid tags while preserving
     * reversibility.
     */
    std::string pathToTag(const std::string & path);

    /**
     * Get the base URL for OCI registry API v2 calls.
     *
     * Returns `https://<registry>/v2/<repository>`
     */
    std::string apiBase();

    /**
     * Fetch an OCI bearer token for the given scope.
     *
     * Implements the OCI token authentication flow:
     * 1. Make a request to the registry
     * 2. Parse the WWW-Authenticate header from the 401 response
     * 3. Request a token from the auth realm
     */
    std::string fetchToken(const std::string & scope);

    /**
     * Make an authenticated FileTransferRequest for the OCI registry API.
     */
    FileTransferRequest makeRequest(const std::string & url, const std::string & scope);

    /**
     * Upload a blob to the OCI registry and return its digest.
     *
     * Uses the OCI distribution upload flow:
     * 1. POST to initiate upload
     * 2. PUT to complete upload with digest
     */
    std::string uploadBlob(const std::string & data, const std::string & mediaType);

    /**
     * Create and push an OCI manifest tagged with the given tag.
     *
     * The manifest wraps a single blob (the file content) as an OCI
     * image layer.
     */
    void pushManifest(const std::string & tag, const std::string & blobDigest, uint64_t blobSize);

    /**
     * Resolve an OCI manifest tag and return the blob digest of the
     * first layer.
     */
    std::string resolveManifest(const std::string & tag);

    struct TokenCache
    {
        std::map<std::string, std::string> tokens;
    };

    Sync<TokenCache> tokenCache;
};

} // namespace nix
