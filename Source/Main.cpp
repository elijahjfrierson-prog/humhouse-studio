#include "UI.h"
#include "License.h"

#if HHS_LICENSED
struct LicensePanel : public juce::Component
{
    std::function<void()> onActivated;
    juce::Label title, info, status;
    juce::TextEditor machine, key;
    juce::TextButton copyBtn { "Copy Machine ID" }, activateBtn { "Activate" }, quitBtn { "Quit" };
    LicensePanel()
    {
        title.setText ("Activate HumHouse Studio", juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
        info.setText ("Send this Machine ID with your purchase email to get your license key (one key covers up to 2 computers), then paste the key below.",
                      juce::dontSendNotification);
        machine.setText (hhs::license::machineId()); machine.setReadOnly (true); machine.setJustification (juce::Justification::centred);
        key.setMultiLine (true, true); key.setReturnKeyStartsNewLine (false);
        key.setTextToShowWhenEmpty ("Paste your license key (HHS1....)", juce::Colours::grey);
        copyBtn.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (machine.getText()); status.setText ("Machine ID copied.", juce::dontSendNotification); };
        activateBtn.onClick = [this] { activate(); };
        quitBtn.onClick = [] { juce::JUCEApplication::getInstance()->quit(); };
        for (auto* c : std::initializer_list<juce::Component*> { &title, &info, &status, &machine, &key, &copyBtn, &activateBtn, &quitBtn })
            addAndMakeVisible (c);
        setSize (580, 400);
    }
    void activate()
    {
        const auto r = hhs::license::verify (key.getText());
        if (! r.ok) { status.setText (r.error, juce::dontSendNotification); return; }
        auto f = hhs::license::keyFile();
        f.getParentDirectory().createDirectory();
        if (! f.replaceWithText (key.getText().trim())) { status.setText ("Could not save the key to " + f.getFullPathName(), juce::dontSendNotification); return; }
        if (onActivated) onActivated();
    }
    void paint (juce::Graphics& g) override { g.fillAll (hhs::col::bg); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (20);
        title.setBounds (r.removeFromTop (34)); info.setBounds (r.removeFromTop (48)); r.removeFromTop (6);
        auto m = r.removeFromTop (30); copyBtn.setBounds (m.removeFromRight (150)); m.removeFromRight (8); machine.setBounds (m);
        r.removeFromTop (12);
        auto b = r.removeFromBottom (32); quitBtn.setBounds (b.removeFromLeft (100)); activateBtn.setBounds (b.removeFromRight (140));
        status.setBounds (r.removeFromBottom (44)); key.setBounds (r);
    }
};

struct LicenseWindow : public juce::DocumentWindow
{
    explicit LicenseWindow (LicensePanel* p) : juce::DocumentWindow ("HumHouse Studio - Activation", hhs::col::bg, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (p, true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->quit(); }
};
#endif

class HumHouseApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
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
       #if HHS_LICENSED
        if (! hhs::license::storedValid())
        {
            auto* p = new LicensePanel();
            p->onActivated = [this] { juce::MessageManager::callAsync ([this] { licenseWindow = nullptr; window = std::make_unique<MainWindow>(); }); };
            licenseWindow = std::make_unique<LicenseWindow> (p);
            return;
        }
       #endif
        window = std::make_unique<MainWindow>();
    }
    void shutdown() override
    {
        licenseWindow = nullptr;
        window = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }
    void systemRequestedQuit() override
    {
        if (window != nullptr) hhs::requestQuit (window->getContentComponent());
        else quit();
    }

    struct MainWindow : public juce::DocumentWindow
    {
        MainWindow() : juce::DocumentWindow (juce::String (JUCE_APPLICATION_NAME_STRING) + " " + juce::JUCEApplication::getInstance()->getApplicationVersion(), hhs::col::bg, juce::DocumentWindow::allButtons)
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
    std::unique_ptr<juce::DocumentWindow> licenseWindow;
};

START_JUCE_APPLICATION (HumHouseApp)
