#include "UI.h"

#if HHS_LICENSED
// the stable app opens with a short Welcome listing what's in it (no key, no activation)
struct WelcomePanel : public juce::Component
{
    juce::Label title, sub;
    juce::TextEditor list;
    juce::ToggleButton dontShow { "Don't show this again" };
    juce::TextButton start { "Start making music" };
    std::function<void()> onDone;
    static juce::File offFile() { return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("HumHouse").getChildFile ("welcome-off.txt"); }
    WelcomePanel()
    {
        title.setText ("Welcome to HumHouse Studio", juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (24.0f, juce::Font::bold)));
        sub.setText ("Beats and vocals in one place - FL-style workflow, BandLab-fast.", juce::dontSendNotification);
        sub.setColour (juce::Label::textColourId, hhs::col::dim);
        list.setMultiLine (true); list.setReadOnly (true); list.setCaretVisible (false);
        list.setText (
            "BEATS\n"
            "  - Channel Rack with step sequencer, built-in kits, sampler and synth\n"
            "  - Piano Roll: scale highlight + LOCK, chord stamp, hi-hat rolls, glide / 808 slides\n"
            "  - Playlist: patterns + audio clips, FL keys (select, paint, slice, mute), snap, zoom\n"
            "\nVOCALS\n"
            "  - Record takes with punch-in, loop recording and instant playback of earlier takes\n"
            "  - AutoPitch (key / scale, speed, Hard Tune), Studio Vocal smoother, de-ess, Noise Gate\n"
            "  - Mono or stereo input per track, works with USB mics\n"
            "\nMIX\n"
            "  - Mixer with routing wires, sends, sidechain Duck, FX chains and presets\n"
            "  - Studio EQ, Compressor, Limiter, Reverb, Delay, Saturator and more - 0 latency\n"
            "  - Your VST3 / AU plugins, crash-safe hosting, automatic delay compensation\n"
            "  - Scope: oscilloscope, waveform and spectrum\n"
            "\nFINISH\n"
            "  - Export WAV / MP3 / M4A and stems, always from bar 1\n"
            "  - Templates, auto-backups, undo history\n"
            "  - OBS / Discord capture friendly windows");
        start.onClick = [this] {
            if (dontShow.getToggleState()) { offFile().getParentDirectory().createDirectory(); offFile().replaceWithText ("1"); }
            if (onDone) onDone();
        };
        for (auto* c : std::initializer_list<juce::Component*> { &title, &sub, &list, &dontShow, &start }) addAndMakeVisible (c);
        setSize (620, 520);
    }
    void paint (juce::Graphics& g) override { g.fillAll (hhs::col::bg); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (22);
        title.setBounds (r.removeFromTop (34)); sub.setBounds (r.removeFromTop (24)); r.removeFromTop (10);
        auto b = r.removeFromBottom (34); dontShow.setBounds (b.removeFromLeft (220)); start.setBounds (b.removeFromRight (190));
        r.removeFromBottom (12); list.setBounds (r);
    }
};

struct WelcomeWindow : public juce::DocumentWindow
{
    explicit WelcomeWindow (WelcomePanel* p) : juce::DocumentWindow ("Welcome to HumHouse Studio", hhs::col::bg, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (p, true);
        centreWithSize (getWidth(), getHeight());
        setAlwaysOnTop (true);
        setVisible (true);
    }
    std::function<void()> onClose;
    void closeButtonPressed() override { if (onClose) onClose(); }
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
        return "1.2.9";
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
       #if HHS_LICENSED
        if (! WelcomePanel::offFile().existsAsFile())
        {
            auto* p = new WelcomePanel();
            auto close = [this] { juce::MessageManager::callAsync ([this] { welcomeWindow = nullptr; }); };
            p->onDone = close;
            auto w = std::make_unique<WelcomeWindow> (p);
            w->onClose = close;
            welcomeWindow = std::move (w);
        }
       #endif
    }
    void shutdown() override
    {
        welcomeWindow = nullptr;
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
           #if ! HHS_LICENSED
            juce::MessageManager::callAsync ([] { hhs::showWhatsNewIfUpdated(); });
           #endif
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

private:
    hhs::HHLookAndFeel laf;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<juce::DocumentWindow> welcomeWindow;
};

START_JUCE_APPLICATION (HumHouseApp)
