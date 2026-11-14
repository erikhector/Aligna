#pragma once
#include "core/UpdateCheck.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace aligna
{
// Help > Check for Updates. Asks GitHub for the latest release when the user chooses to (never by itself),
// and on Windows can download that release's installer, start it and quit. Message thread only; the network
// work runs on a thread behind a progress window with Cancel.
class Updater
{
public:
    // Asks about unsaved changes, then calls its argument if the application may quit.
    using ConfirmQuit = std::function<void(std::function<void()> proceed)>;

    Updater(juce::Component& parent, ConfirmQuit);
    ~Updater();

    void check();                                  // no-op while a check or download is running
    void close();                                  // stops a running check or download
    bool isBusy() const { return task != nullptr; }

private:
    void checked(bool ok, const std::optional<ReleaseInfo>&);
    void offerPage(const juce::String& title, const juce::String& message, const juce::String& url);
    void download(const ReleaseInfo&);
    void downloaded(const juce::File& installer, const juce::String& error);
    void finishTask(std::function<void()> then); // drops the task outside its own callback, then runs `then`

    juce::Component& parent;
    ConfirmQuit confirmQuit;
    std::unique_ptr<juce::ThreadWithProgressWindow> task; // the check or the download; at most one
    JUCE_DECLARE_WEAK_REFERENCEABLE(Updater)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Updater)
};
} // namespace aligna
