#pragma once
#include <juce_core/juce_core.h>
#include <optional>

namespace aligna
{
// Where releases are published. The update check reads the first, the user is sent to the second.
extern const char* const latestReleaseApiUrl;
extern const char* const releasesPageUrl;
extern const char* const windowsInstallerName; // the release asset the Windows update downloads

// <0, 0 or >0 as a is older than, equal to or newer than b. Dotted numbers; a leading 'v' is ignored and
// missing parts count as 0 ("1.2" == "v1.2.0").
int compareVersions(const juce::String& a, const juce::String& b);

struct ReleaseInfo
{
    juce::String version;         // without the leading 'v'
    juce::String pageUrl;         // the release's web page
    juce::String installerUrl;    // empty if the release has no Windows installer
    juce::int64 installerSize = 0;
    juce::String installerSha256; // lower-case hex; empty if the server gave none
};

// Reads GitHub's "latest release" JSON. Nothing if it is not a release with a dotted-number tag. Links that do
// not point into this project's releases are dropped (pageUrl falls back to releasesPageUrl), so a response
// cannot send the user or the download anywhere else.
std::optional<ReleaseInfo> parseLatestRelease(const juce::String& json);
} // namespace aligna
