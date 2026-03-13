#include <gtest/gtest.h>

#include "nix/store/oci-binary-cache-store.hh"
#include "nix/util/url.hh"

namespace nix {

TEST(OCIBinaryCacheStore, constructConfig)
{
    OCIBinaryCacheStoreConfig config{parseURL("oci://ghcr.io/user/nix-cache"), {}};

    EXPECT_EQ(config.registryHost, "ghcr.io");
    EXPECT_EQ(config.repository, "user/nix-cache");
}

TEST(OCIBinaryCacheStore, constructConfigWithPort)
{
    OCIBinaryCacheStoreConfig config{parseURL("oci://localhost:5000/my/repo"), {}};

    EXPECT_EQ(config.registryHost, "localhost:5000");
    EXPECT_EQ(config.repository, "my/repo");
}

TEST(OCIBinaryCacheStore, constructConfigDeepPath)
{
    OCIBinaryCacheStoreConfig config{parseURL("oci://ghcr.io/org/team/nix-cache"), {}};

    EXPECT_EQ(config.registryHost, "ghcr.io");
    EXPECT_EQ(config.repository, "org/team/nix-cache");
}

TEST(OCIBinaryCacheStore, constructConfigNoHost)
{
    EXPECT_THROW(OCIBinaryCacheStoreConfig(parseURL("oci:///user/repo"), {}), UsageError);
}

TEST(OCIBinaryCacheStore, constructConfigNoPath)
{
    EXPECT_THROW(OCIBinaryCacheStoreConfig(parseURL("oci://ghcr.io"), {}), UsageError);
}

TEST(OCIBinaryCacheStore, constructConfigEmptyPath)
{
    EXPECT_THROW(OCIBinaryCacheStoreConfig(parseURL("oci://ghcr.io/"), {}), UsageError);
}

TEST(OCIBinaryCacheStore, storeDir_absolutePath)
{
    OCIBinaryCacheStoreConfig config{parseURL("oci://ghcr.io/user/nix-cache"), {{"store", "/my/store"}}};
    EXPECT_EQ(config.storeDir, "/my/store");
}

TEST(OCIBinaryCacheStore, storeDir_relativePath_rejected)
{
    EXPECT_THROW(
        OCIBinaryCacheStoreConfig(parseURL("oci://ghcr.io/user/nix-cache"), {{"store", "my/store"}}), UsageError);
}

TEST(OCIBinaryCacheStore, uriSchemes)
{
    auto schemes = OCIBinaryCacheStoreConfig::uriSchemes();
    EXPECT_TRUE(schemes.count("oci") > 0) << "OCI scheme should be supported";
    EXPECT_EQ(schemes.size(), 1) << "Only the 'oci' scheme should be registered";
}

TEST(OCIBinaryCacheStore, experimentalFeature)
{
    auto feature = OCIBinaryCacheStoreConfig::experimentalFeature();
    EXPECT_TRUE(feature.has_value());
    EXPECT_EQ(*feature, ExperimentalFeature::OCIBinaryCacheStore);
}

TEST(OCIBinaryCacheStore, name)
{
    EXPECT_EQ(OCIBinaryCacheStoreConfig::name(), "OCI Binary Cache Store");
}

TEST(OCIBinaryCacheStore, getReference)
{
    OCIBinaryCacheStoreConfig config{parseURL("oci://ghcr.io/user/nix-cache"), {}};
    auto ref = config.getReference();
    auto specified = std::get<StoreReference::Specified>(ref.variant);
    EXPECT_EQ(specified.scheme, "oci");
}

TEST(OCIBinaryCacheStore, getReferenceWithParams)
{
    StoreConfig::Params params{{"compression", "xz"}};
    OCIBinaryCacheStoreConfig config{parseURL("oci://ghcr.io/user/nix-cache"), params};
    auto ref = config.getReference();
    EXPECT_EQ(ref.params["compression"], "xz");
}

} // namespace nix
