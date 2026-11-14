#include "core/UpdateCheck.h"
#include <catch2/catch_test_macros.hpp>

using namespace aligna;

namespace
{
juce::String release(const juce::String& tag, const juce::String& assets, const juce::String& extra = {})
{
    return "{\"tag_name\":\"" + tag + "\",\"html_url\":\"https://github.com/erikhector/Aligna/releases/tag/" + tag
           + "\",\"draft\":false,\"prerelease\":false," + extra + "\"assets\":[" + assets + "]}";
}
juce::String asset(const juce::String& name, const juce::String& url, const juce::String& size, const juce::String& digest)
{
    return "{\"name\":\"" + name + "\",\"browser_download_url\":\"" + url + "\",\"size\":" + size
           + (digest.isEmpty() ? juce::String() : ",\"digest\":\"" + digest + "\"") + "}";
}
const juce::String base = "https://github.com/erikhector/Aligna/releases/download/v0.4.0/";
const juce::String sha = "2e0ab4449522a95527982af17a0b3ca8b6b68dbeeba06a2c9b5fb57f29f5639f";
} // namespace

TEST_CASE("compareVersions orders dotted numbers")
{
    CHECK(compareVersions("0.3.0", "0.3.0") == 0);
    CHECK(compareVersions("v0.3.0", "0.3") == 0);
    CHECK(compareVersions("0.3.0", "0.4.0") < 0);
    CHECK(compareVersions("0.10.0", "0.9.9") > 0);
    CHECK(compareVersions("1.0", "0.99.99") > 0);
    CHECK(compareVersions("0.3.1", "0.3") > 0);
    CHECK(compareVersions("V1.2.3", "v1.2.4") < 0);
}

TEST_CASE("parseLatestRelease reads version, page and installer")
{
    const auto r = parseLatestRelease(release(
        "v0.4.0", asset("Aligna-windows.zip", base + "Aligna-windows.zip", "100", {}) + ","
                      + asset("Aligna-windows-setup.exe", base + "Aligna-windows-setup.exe", "4000000", "sha256:" + sha)));
    REQUIRE(r.has_value());
    CHECK(r->version == "0.4.0");
    CHECK(r->pageUrl == "https://github.com/erikhector/Aligna/releases/tag/v0.4.0");
    CHECK(r->installerUrl == base + "Aligna-windows-setup.exe");
    CHECK(r->installerSize == 4000000);
    CHECK(r->installerSha256 == sha);
}

TEST_CASE("parseLatestRelease without an installer still reports the version")
{
    const auto r = parseLatestRelease(release("v0.3.0", asset("Aligna-windows.zip", base + "Aligna-windows.zip", "100", {})));
    REQUIRE(r.has_value());
    CHECK(r->version == "0.3.0");
    CHECK(r->installerUrl.isEmpty());
    CHECK(parseLatestRelease(release("v0.3.0", {}))->installerUrl.isEmpty());
}

TEST_CASE("parseLatestRelease refuses what is not a usable release")
{
    CHECK_FALSE(parseLatestRelease({}).has_value());
    CHECK_FALSE(parseLatestRelease("not json").has_value());
    CHECK_FALSE(parseLatestRelease("[1,2]").has_value());
    CHECK_FALSE(parseLatestRelease("{\"message\":\"API rate limit exceeded\"}").has_value());
    CHECK_FALSE(parseLatestRelease(release("nightly", {})).has_value());
    CHECK_FALSE(parseLatestRelease(release("v1..2", {})).has_value());
    CHECK_FALSE(parseLatestRelease(release("v0.4.0-beta", {})).has_value());
    CHECK_FALSE(parseLatestRelease(release("v0.4.0", {}, "\"prerelease\":true,")).has_value());
}

TEST_CASE("parseLatestRelease drops links that leave the project's releases")
{
    // The installer must come from this project's release downloads, with a size; a malformed digest is ignored.
    auto r = parseLatestRelease(release("v0.4.0", asset("Aligna-windows-setup.exe", "https://example.com/setup.exe", "5", {})));
    REQUIRE(r.has_value());
    CHECK(r->installerUrl.isEmpty());
    r = parseLatestRelease(release("v0.4.0", asset("Aligna-windows-setup.exe", base + "Aligna-windows-setup.exe", "0", {})));
    CHECK(r->installerUrl.isEmpty());
    r = parseLatestRelease(release("v0.4.0", asset("Aligna-windows-setup.exe", base + "Aligna-windows-setup.exe", "5", "md5:abc")));
    CHECK(r->installerUrl.isNotEmpty());
    CHECK(r->installerSha256.isEmpty());

    r = parseLatestRelease("{\"tag_name\":\"v0.4.0\",\"html_url\":\"https://example.com/x\",\"assets\":[]}");
    REQUIRE(r.has_value());
    CHECK(r->pageUrl == "https://github.com/erikhector/Aligna/releases");
}
