#include <juce_gui_basics/juce_gui_basics.h>

#include "app/AppController.h"
#include "io/FileNaming.h"
#include "ui/MainComponent.h"

namespace aligna
{
class AlignaApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override
    {
        controller = std::make_unique<AppController>();
        mainWindow = std::make_unique<MainWindow>(getApplicationName(), *controller);
        // A project named on the command line (double-click on a .aligna file) opens once the window is up.
        const auto project = projectFileFromArgs(getCommandLineParameterArray(), juce::File::getCurrentWorkingDirectory());
        if (project != juce::File())
            mainWindow->openWhenReady(project);
    }
    void shutdown() override
    {
        if (mainWindow != nullptr)
            mainWindow->closeDialogs();
        mainWindow.reset(); // before the controller
        controller.reset();
    }
    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
            mainWindow->requestQuit(); // asks about unsaved changes first
        else
            quit();
    }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow(const juce::String& name, AppController& c)
            : DocumentWindow(name, juce::Colour(0xff1e1f22), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            main = new MainComponent(c);
            setContentOwned(main, true);
            setResizable(true, true);
            centreWithSize(1200, 800);
            setVisible(true);
        }

        void requestQuit() { main->requestQuit(); }
        void openWhenReady(const juce::File& f)
        {
            juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainComponent>(main), f] {
                if (safe != nullptr)
                    safe->openFile(f);
            });
        }
        void closeDialogs() { main->closeDialogs(); }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    private:
        MainComponent* main = nullptr; // owned as the content component
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

    std::unique_ptr<AppController> controller;
    std::unique_ptr<MainWindow> mainWindow;
};
}

START_JUCE_APPLICATION(aligna::AlignaApplication)
