#include "core/UpdateCheck.h"

namespace aligna
{
const char* const latestReleaseApiUrl = "https://api.github.com/repos/erikhector/Aligna/releases/latest";
const char* const releasesPageUrl = "https://github.com/erikhector/Aligna/releases";
const char* const windowsInstallerName = "Aligna-windows-setup.exe";

namespace
{
juce::String withoutV(const juce::String& v)
{
    const auto t = v.trim();
    return t.startsWithIgnoreCase("v") ? t.substring(1) : t;
}

bool isDottedNumber(const juce::String& v)
{
    if (v.isEmpty() || v.length() > 32 || ! v.containsOnly("0123456789.") || v.startsWithChar('.') || v.endsWithChar('.')
        || v.contains(".."))
        return false;
    for (auto& part : juce::StringArray::fromTokens(v, ".", {}))
        if (part.length() > 9) // each part fits an int
            return false;
    return true;
}
} // namespace

int compareVersions(const juce::String& a, const juce::String& b)
{
    const auto pa = juce::StringArray::fromTokens(withoutV(a), ".", {});
    const auto pb = juce::StringArray::fromTokens(withoutV(b), ".", {});
    for (int i = 0; i < std::max(pa.size(), pb.size()); ++i)
    {
        const auto x = pa[i].getLargeIntValue(), y = pb[i].getLargeIntValue(); // out of range: empty -> 0
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

std::optional<ReleaseInfo> parseLatestRelease(const juce::String& json)
{
    const auto root = juce::JSON::parse(json);
    if (! root.isObject() || (bool) root["draft"] || (bool) root["prerelease"])
        return {};
    ReleaseInfo info;
    info.version = withoutV(root["tag_name"].toString());
    if (! isDottedNumber(info.version))
        return {};

    const juce::String pages = juce::String(releasesPageUrl) + "/";
    info.pageUrl = root["html_url"].toString();
    if (! info.pageUrl.startsWith(pages + "tag/"))
        info.pageUrl = releasesPageUrl;

    if (auto* assets = root["assets"].getArray())
        for (auto& a : *assets)
        {
            if (a["name"].toString() != windowsInstallerName)
                continue;
            const auto url = a["browser_download_url"].toString();
            const auto size = (juce::int64) a["size"];
            if (! url.startsWith(pages + "download/") || size <= 0)
                break;
            info.installerUrl = url;
            info.installerSize = size;
            const auto digest = a["digest"].toString();
            const auto hex = digest.fromFirstOccurrenceOf("sha256:", false, false).toLowerCase();
            if (digest.startsWith("sha256:") && hex.length() == 64 && hex.containsOnly("0123456789abcdef"))
                info.installerSha256 = hex;
            break;
        }
    return info;
}
} // namespace aligna
