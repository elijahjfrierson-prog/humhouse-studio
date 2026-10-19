#include "UI.h"

class HumHouseApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "HumHouse Studio"; }
    const juce::String getApplicationVersion() override
    {
       #ifdef JUCE_APPLICATION_VERSION_STRING
        return JUCE_APPLICATION_VERSION_STRING;
       #else
        return "1.2.5";
       #endif
    }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        if (hhs::isPluginScanChild (commandLine))
        {
            setApplicationReturnValue (hhs::runPluginScanChild (commandLine));
            quit();
            return;
        }
        juce::LookAndFeel::setDefaultLookAndFeel (&laf);
        window = std::make_unique<MainWindow>();
    }
    void shutdown() override
    {
        window = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }
    void systemRequestedQuit() override { quit(); }

    struct MainWindow : public juce::DocumentWindow
    {
        MainWindow() : juce::DocumentWindow ("HumHouse Studio " + juce::JUCEApplication::getInstance()->getApplicationVersion(), hhs::col::bg, juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (hhs::createMainComponent().release(), true);
            setResizable (true, true);
            setResizeLimits (1000, 640, 10000, 8000);
            centreWithSize (1440, 900);
            setVisible (true);
            hhs::makeCaptureFriendly (*this);
            juce::MessageManager::callAsync ([] { hhs::showWhatsNewIfUpdated(); });
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

private:
    hhs::HHLookAndFeel laf;
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (HumHouseApp)
