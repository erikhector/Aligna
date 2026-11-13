#include <juce_gui_basics/juce_gui_basics.h>

#include "app/AppController.h"
#include "ui/MainComponent.h"

namespace aligna
{
class AlignaApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override { controller = std::make_unique<AppController>(); mainWindow = std::make_unique<MainWindow>(getApplicationName(), *controller); }
    void shutdown() override { mainWindow.reset(); controller.reset(); }
    void systemRequestedQuit() override { quit(); }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow(const juce::String& name, AppController& c)
            : DocumentWindow(name, juce::Colour(0xff1e1f22), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(c), true);
            setResizable(true, true);
            centreWithSize(1200, 800);
            setVisible(true);
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

    std::unique_ptr<AppController> controller;
    std::unique_ptr<MainWindow> mainWindow;
};
}

START_JUCE_APPLICATION(aligna::AlignaApplication)
