#include "ui/Updater.h"
#include "core/Version.h"
#include <juce_cryptography/juce_cryptography.h>

namespace aligna
{
namespace
{
// Without libcurl JUCE has no HTTPS on Linux/BSD: there the menu item opens the releases page instead.
#if (JUCE_LINUX || JUCE_BSD) && ! JUCE_USE_CURL
constexpr bool canCheck = false;
#else
constexpr bool canCheck = true;
#endif
#if JUCE_WINDOWS
constexpr bool canInstall = true;
#else
constexpr bool canInstall = false;
#endif

constexpr int maxReleaseJsonBytes = 1 << 20;

std::unique_ptr<juce::InputStream> openUrl(const juce::String& url, juce::Thread& thread)
{
    int status = 0;
    auto in = juce::URL(url).createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                                   .withConnectionTimeoutMs(15000)
                                                   .withNumRedirectsToFollow(5)
                                                   .withStatusCode(&status)
                                                   .withExtraHeaders("User-Agent: Aligna\r\nAccept: */*")
                                                   .withProgressCallback([&thread](int, int) { return ! thread.threadShouldExit(); }));
    if (in == nullptr || status != 200)
        return {};
    return in;
}

class CheckTask : public juce::ThreadWithProgressWindow
{
public:
    CheckTask(std::function<void(bool cancelled, bool ok, std::optional<ReleaseInfo>)> done, juce::Component* around)
        : ThreadWithProgressWindow("Checking for updates...", false, true, 10000, {}, around), onDone(std::move(done))
    {
    }
    void run() override
    {
        setProgress(-1.0); // length unknown
        auto in = openUrl(latestReleaseApiUrl, *this);
        if (in == nullptr)
            return;
        juce::MemoryOutputStream text;
        char buffer[8192];
        while (! threadShouldExit() && ! in->isExhausted())
        {
            const auto n = in->read(buffer, (int) sizeof(buffer));
            if (n <= 0)
                break;
            if (text.getDataSize() + (size_t) n > (size_t) maxReleaseJsonBytes)
                return; // not a release description
            text.write(buffer, (size_t) n);
        }
        if (threadShouldExit())
            return;
        release = parseLatestRelease(text.toUTF8());
        ok = release.has_value();
    }
    void threadComplete(bool cancelled) override { onDone(cancelled, ok, release); }

private:
    std::function<void(bool, bool, std::optional<ReleaseInfo>)> onDone;
    std::optional<ReleaseInfo> release;
    bool ok = false;
};

// Downloads the installer to `target`. On any failure the partial file is deleted and `error` says why
// (empty error with no file: the user cancelled).
class DownloadTask : public juce::ThreadWithProgressWindow
{
public:
    DownloadTask(ReleaseInfo r, juce::File t, std::function<void(juce::File, juce::String)> done, juce::Component* around)
        : ThreadWithProgressWindow("Downloading Aligna " + r.version + "...", true, true, 10000, {}, around),
          info(std::move(r)), target(std::move(t)), onDone(std::move(done))
    {
    }
    void run() override
    {
        error = fetch();
        if (error.isNotEmpty() || threadShouldExit())
            target.deleteFile();
    }
    void threadComplete(bool cancelled) override
    {
        if (cancelled)
        {
            target.deleteFile();
            onDone({}, {});
        }
        else
            onDone(error.isEmpty() ? target : juce::File(), error);
    }

private:
    juce::String fetch()
    {
        const juce::String failed = "The download failed. Check your internet connection and try again.";
        target.getParentDirectory().createDirectory();
        target.deleteFile();
        auto in = openUrl(info.installerUrl, *this);
        if (in == nullptr)
            return failed;
        {
            juce::FileOutputStream out(target);
            if (out.failedToOpen())
                return "Could not write " + target.getFullPathName();
            juce::HeapBlock<char> buffer(1 << 16);
            juce::int64 total = 0;
            while (! threadShouldExit() && ! in->isExhausted())
            {
                const auto n = in->read(buffer, 1 << 16);
                if (n <= 0)
                    break;
                total += n;
                if (total > info.installerSize)
                    return "The download is not the file that was announced.";
                if (! out.write(buffer, (size_t) n))
                    return "Could not write " + target.getFullPathName();
                setProgress((double) total / (double) info.installerSize);
            }
            out.flush();
            if (threadShouldExit())
                return {};
            if (total != info.installerSize)
                return failed;
        }
        setStatusMessage("Checking the download...");
        if (info.installerSha256.isNotEmpty()
            && juce::SHA256(target).toHexString().toLowerCase() != info.installerSha256)
            return "The download is damaged or is not the file that was announced.";
        return {};
    }

    ReleaseInfo info;
    juce::File target;
    std::function<void(juce::File, juce::String)> onDone;
    juce::String error;
};

juce::MessageBoxOptions box(juce::Component& parent, const juce::String& title, const juce::String& message,
                            juce::MessageBoxIconType icon = juce::MessageBoxIconType::InfoIcon)
{
    return juce::MessageBoxOptions().withIconType(icon).withTitle(title).withMessage(message).withAssociatedComponent(&parent);
}
} // namespace

Updater::Updater(juce::Component& p, ConfirmQuit c) : parent(p), confirmQuit(std::move(c)) {}
Updater::~Updater()
{
    close();
    masterReference.clear();
}

void Updater::close() { task.reset(); } // stops and joins the thread

void Updater::finishTask(std::function<void()> then)
{
    // Deferred: the task must not be deleted from inside its own callback.
    juce::MessageManager::callAsync([weak = juce::WeakReference<Updater>(this), then = std::move(then)] {
        if (weak == nullptr || weak->task == nullptr) // close() got here first
            return;
        weak->task.reset();
        then();
    });
}

void Updater::check()
{
    if (task != nullptr)
        return;
    if (! canCheck)
    {
        offerPage("Check for Updates",
                  "You have Aligna " + juce::String(versionString()) + ". New versions are published on the releases page.",
                  releasesPageUrl);
        return;
    }
    task = std::make_unique<CheckTask>(
        [this](bool cancelled, bool ok, std::optional<ReleaseInfo> release) {
            finishTask([this, cancelled, ok, release] {
                if (! cancelled)
                    checked(ok, release);
            });
        },
        &parent);
    static_cast<CheckTask*>(task.get())->launchThread();
}

void Updater::checked(bool ok, const std::optional<ReleaseInfo>& release)
{
    const juce::String current = versionString();
    if (! ok || ! release)
    {
        offerPage("Check for Updates", "Could not check for updates. Check your internet connection, or look at the releases page.",
                  releasesPageUrl);
        return;
    }
    if (compareVersions(release->version, current) <= 0)
    {
        juce::AlertWindow::showAsync(box(parent, "Check for Updates", "Aligna " + current + " is the latest version.").withButton("OK"),
                                     nullptr);
        return;
    }
    const auto headline = "Aligna " + release->version + " is available. You have " + current + ".";
    if (! canInstall || release->installerUrl.isEmpty())
    {
        offerPage("Update available", headline, release->pageUrl);
        return;
    }
    juce::AlertWindow::showAsync(box(parent, "Update available",
                                     headline + "\n\nInstall downloads the update, closes Aligna and opens it again when the update is installed.",
                                     juce::MessageBoxIconType::QuestionIcon)
                                     .withButton("Install")
                                     .withButton("Release notes")
                                     .withButton("Later"),
                                 [weak = juce::WeakReference<Updater>(this), info = *release](int r) {
                                     if (weak == nullptr)
                                         return;
                                     if (r == 1)
                                         weak->download(info);
                                     else if (r == 2)
                                         juce::URL(info.pageUrl).launchInDefaultBrowser();
                                 });
}

void Updater::offerPage(const juce::String& title, const juce::String& message, const juce::String& url)
{
    juce::AlertWindow::showAsync(box(parent, title, message).withButton("Open releases page").withButton("Close"), [url](int r) {
        if (r == 1)
            juce::URL(url).launchInDefaultBrowser();
    });
}

void Updater::download(const ReleaseInfo& info)
{
    if (task != nullptr)
        return;
    const auto target = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("Aligna-update").getChildFile(windowsInstallerName);
    task = std::make_unique<DownloadTask>(
        info, target,
        [this](juce::File installer, juce::String error) { finishTask([this, installer, error] { downloaded(installer, error); }); },
        &parent);
    static_cast<DownloadTask*>(task.get())->launchThread();
}

void Updater::downloaded(const juce::File& installer, const juce::String& error)
{
    if (error.isNotEmpty())
    {
        juce::AlertWindow::showAsync(box(parent, "Update", error, juce::MessageBoxIconType::WarningIcon).withButton("OK"), nullptr);
        return;
    }
    if (installer == juce::File()) // cancelled
        return;
    // The save question comes last, so a cancelled or failed download leaves the session as it was.
    confirmQuit([weak = juce::WeakReference<Updater>(this), installer] {
        if (weak == nullptr)
            return;
        // The installer shows only its progress, replaces the program and starts it again (see Aligna.iss).
        if (installer.startAsProcess("/SILENT /relaunch=1"))
            juce::JUCEApplication::quit();
        else
            juce::AlertWindow::showAsync(box(weak->parent, "Update", "Could not start the installer:\n" + installer.getFullPathName(),
                                             juce::MessageBoxIconType::WarningIcon)
                                             .withButton("OK"),
                                         nullptr);
    });
}
} // namespace aligna
