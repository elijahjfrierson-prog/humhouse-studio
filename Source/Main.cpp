#include "PluginSandbox.h"
#include "UI.h"
#include "License.h"

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
        sub.setText ("Beats and vocals in one place - full studio workflow, phone-fast.", juce::dontSendNotification);
        sub.setColour (juce::Label::textColourId, hhs::col::dim);
        list.setMultiLine (true); list.setReadOnly (true); list.setCaretVisible (false);
        list.setText (
            "BEATS\n"
            "  - Channel Rack with step sequencer, built-in kits, sampler and synth\n"
            "  - Piano Roll: scale highlight + LOCK, chord stamp, hi-hat rolls, glide / 808 slides\n"
            "  - Playlist: patterns + audio clips, HumHouse keys (select, paint, slice, mute), snap, zoom\n"
            "\nVOCALS\n"
            "  - Record takes with punch-in, loop recording and instant playback of earlier takes\n"
            "  - AutoPitch (key / scale, speed, Hard Tune), Studio Vocal smoother, de-ess, Noise Gate\n"
            "  - Mono or stereo input per track, works with USB mics\n"
            "\nZENITH AUDIO ENGINE\n"
            "  - Lock-free and multicore: edits never pause playback\n"
            "  - Render-ahead playback, sample-exact automation, delay compensation\n"
            "  - Plug-ins run directly in the engine (fastest); optional crash protection in Options > Performance\n"
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


#if HHS_KEY_GATE
// regular build: no key, no app. The key is checked by our license server (3 computers per key).
struct ActivationPanel : public juce::Component, public juce::TextDragAndDropTarget, private juce::Timer
{
    juce::Label title, info, status, mid;
    juce::TextEditor key;
    juce::TextButton activate { "Activate" }, quitBtn { "Quit" }, copyId { "Copy Machine ID" }, pasteBtn { "PASTE KEY" };
    std::function<void (juce::String)> onActivate;
    explicit ActivationPanel (const juce::String& message)
    {
        title.setText ("Activate HumHouse Studio", juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (24.0f, juce::Font::bold)));
        info.setText ("Enter your license key (HHS-XXXXX-XXXXX-XXXXX-XXXXX). Each key works on up to 3 computers. "
                      "It checks in online once a day by itself.", juce::dontSendNotification);
        info.setColour (juce::Label::textColourId, hhs::col::dim);
        info.setJustificationType (juce::Justification::topLeft);
        mid.setText ("Machine ID: " + hhs::license::machineId(), juce::dontSendNotification);
        mid.setColour (juce::Label::textColourId, hhs::col::dim);
        status.setText (message, juce::dontSendNotification);
        key.setTextToShowWhenEmpty ("HHS-XXXXX-XXXXX-XXXXX-XXXXX", hhs::col::dim);
        key.setFont (juce::Font (juce::FontOptions (18.0f)));
        key.onReturnKey = [this] { activate.triggerClick(); };
        key.onTextChange = [this] { typed(); };   // Cmd/Ctrl+V or typing: a full key (or this Machine ID) submits by itself
        activate.onClick = [this] {
            if (isMachineId (key.getText())) key.setText ({}, false);   // Machine ID in the box = unlock this computer by its ID
            if (const auto k = hhs::license::findKey (key.getText()); k.isNotEmpty()) key.setText (k, false);
            activate.setEnabled (false);
            setStatus (key.getText().trim().isEmpty() ? "Checking this computer's Machine ID..." : "Checking your key...");
            if (onActivate) onActivate (key.getText());
        };
        quitBtn.onClick = [] { juce::JUCEApplication::getInstance()->quit(); };
        pasteBtn.setTooltip ("Paste the license key you copied (or press Cmd/Ctrl+V in the box, or drag the highlighted key onto this window)");
        pasteBtn.onClick = [this] {
            const auto k = keyIn (juce::SystemClipboard::getTextFromClipboard());
            if (k.isEmpty()) { setStatus ("No license key on the clipboard. Highlight your HHS-... key, press Cmd/Ctrl+C, then PASTE KEY."); return; }
            key.setText (k, false); setStatus ("Key filled in - click Activate.");
        };
        for (auto* c : std::initializer_list<juce::Component*> { &title, &info, &key, &status, &mid, &activate, &quitBtn, &pasteBtn }) addAndMakeVisible (c);
        setSize (560, 300);
        pasteFromClipboard();   // a key copied from the purchase email / page fills itself in
        startTimer (800);       // ...and so does one copied while this window is open
    }
    static juce::String keyIn (const juce::String& text)
    {
        return hhs::license::findKey (text);   // the exact HHS-XXXXX-XXXXX-XXXXX-XXXXX form the server checks
    }
    static bool isMachineId (const juce::String& text)
    {
        const auto strip = [] (const juce::String& t) { return t.toUpperCase().retainCharacters ("0123456789ABCDEF"); };
        return text.trim().isNotEmpty() && strip (text) == strip (hhs::license::machineId()) && text.trim().length() <= 24;
    }
    void copyAndUnlock()
    {
        const auto id = hhs::license::machineId();
        juce::SystemClipboard::copyTextToClipboard (id);
        lastClip = id;
        copyId.setButtonText ("Copied!");
        mid.setText ("Machine ID copied: " + id, juce::dontSendNotification);
        key.setText (id, true);   // -> typed(): unlocks an already-activated computer
    }
    void typed()
    {
        const auto text = key.getText();
        if (isMachineId (text))
        {
            if (activate.isEnabled()) { setStatus ("Machine ID pasted - unlocking this computer..."); activate.triggerClick(); }
            return;
        }
        const auto k = keyIn (text);
        if (k.isEmpty()) return;
        if (k != text) key.setText (k, false);
        setStatus ("Key filled in - click Activate.");
    }
    bool pasteFromClipboard()
    {
        const auto clip = juce::SystemClipboard::getTextFromClipboard();
        if (clip == lastClip && key.getText().isNotEmpty()) return key.getText() == keyIn (clip) && keyIn (clip).isNotEmpty();
        lastClip = clip;
        const auto k = keyIn (clip);
        if (k.isEmpty()) return false;
        if (k != key.getText()) key.setText (k, false);
        setStatus ("Key filled in - click Activate.");   // a copied key fills itself in; you only click Activate
        return true;
    }
    void timerCallback() override   // a copied key always lands in the box by itself (no remembered clipboard, no waiting)
    {
        const auto k = keyIn (juce::SystemClipboard::getTextFromClipboard());
        if (k.isNotEmpty() && k != key.getText()) { key.setText (k, false); setStatus ("Key filled in - click Activate."); }
    }   // the key box has focus on open, so don't wait for it to lose it
    void mouseUp (const juce::MouseEvent& e) override
    {
        juce::ignoreUnused (e);
    }
    juce::String lastClip;
    // drag the highlighted key from the email / web page onto this window
    bool isInterestedInTextDrag (const juce::String&) override { return true; }
    void textDropped (const juce::String& text, int, int) override
    {
        const auto k = keyIn (text);
        if (k.isEmpty()) { setStatus ("That isn't a HumHouse license key (HHS-XXXXX-XXXXX-XXXXX-XXXXX)."); return; }
        key.setText (k, false); setStatus ("Key filled in - click Activate.");
    }
    void visibilityChanged() override { if (isShowing()) juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<ActivationPanel> (this)] { if (sp) sp->key.grabKeyboardFocus(); }); }
    void setStatus (const juce::String& s) { status.setText (s, juce::dontSendNotification); }
    void failed (const juce::String& s) { setStatus (s); activate.setEnabled (true); }
    // a saved key, a copied key, or (neither) this computer's Machine ID: check by itself, nothing to type
    void autoCheck (const juce::String& saved)
    {
        const auto clip = juce::SystemClipboard::getTextFromClipboard();
        const auto fresh = keyIn (clip);
        lastClip = clip;
        if (fresh.isNotEmpty() && fresh != keyIn (saved)) key.setText (fresh, false);   // a key copied just now wins over an old saved one
        else if (saved.isNotEmpty()) key.setText (saved, false);
        activate.triggerClick();
    }
    void paint (juce::Graphics& g) override { g.fillAll (hhs::col::bg); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (22);
        title.setBounds (r.removeFromTop (34)); info.setBounds (r.removeFromTop (54)); r.removeFromTop (8);
        auto kr = r.removeFromTop (34); pasteBtn.setBounds (kr.removeFromRight (110)); kr.removeFromRight (6); key.setBounds (kr); r.removeFromTop (8);
        status.setBounds (r.removeFromTop (40));
        auto b = r.removeFromBottom (32);
        quitBtn.setBounds (b.removeFromRight (90)); b.removeFromRight (8); activate.setBounds (b.removeFromRight (130));
        mid.setBounds (r.removeFromBottom (24));
    }
};

struct ActivationWindow : public juce::DocumentWindow
{
    explicit ActivationWindow (ActivationPanel* p) : juce::DocumentWindow ("Activate HumHouse Studio", hhs::col::bg, juce::DocumentWindow::closeButton)
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
    juce::ScopedLowPowerModeDisabler noAppNap;   // macOS: stay full speed while OBS is in front
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override
    {
       #ifdef JUCE_APPLICATION_VERSION_STRING
        return JUCE_APPLICATION_VERSION_STRING;
       #else
        return "1.3.2";
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
       #if ! JUCE_IOS
        if (hhs::isPluginHostChild (commandLine))   // crash-protected plug-in helper: no window, runs until the host closes it
        {
            if (! hhs::runPluginHostChild (commandLine)) quit();
            return;
        }
       #endif
        juce::LookAndFeel::setDefaultLookAndFeel (&laf);
       #if JUCE_IOS   // phone / tablet: scale so the full top bar (~1600 px wide) fits the screen
        if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            const auto a = d->totalArea;
            const float shortSide = (float) juce::jmin (a.getWidth(), a.getHeight());
            juce::Desktop::getInstance().setGlobalScaleFactor (juce::jmin (1.0f, shortSide / 640.0f));   // the top bar wraps into rows, so portrait works too
        }
        juce::Desktop::getInstance().setOrientationsEnabled (juce::Desktop::allOrientations);
       #endif
       #if HHS_KEY_GATE
        launchLine = commandLine;
        // no key screen: the app opens, and behind it the server gives this computer its license
        // (a saved bought key is used if there is one). Only a revoke from the monitor locks it.
        renewInBackground (true);
       #endif
        startMain (commandLine);
    }
    void startMain (const juce::String& commandLine)
    {
        window = std::make_unique<MainWindow>();
        openFromCommandLine (commandLine);
        for (auto& f : pendingOpen) openFromCommandLine (f);
        pendingOpen.clear();
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
    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (window == nullptr) { pendingOpen.add (commandLine); return; }   // macOS hands over a double-clicked file before the window exists
        openFromCommandLine (commandLine);
    }
    void openFromCommandLine (const juce::String& commandLine)
    {
        for (auto& arg : juce::StringArray::fromTokens (commandLine, true))
        {
            const juce::File f (arg.unquoted().trim());
            if (! juce::File::isAbsolutePath (arg.unquoted().trim()) || ! f.existsAsFile() || ! hhs::isHumHouseShareFile (f.getFullPathName())) continue;
            juce::Component::SafePointer<juce::Component> main (window->getContentComponent());
            juce::MessageManager::callAsync ([main, f] { if (main != nullptr) hhs::openFileInMain (main, f); });
        }
    }
    void shutdown() override
    {
       #if HHS_KEY_GATE
        *alive = false;
        licenseWatch.stopTimer();
        licensePool.removeAllJobs (true, 15000);
        activationWindow = nullptr;
       #endif
        welcomeWindow = nullptr;
        window = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }
    void memoryWarningReceived() override { if (hhs::lowMemoryMain) hhs::lowMemoryMain(); }
    void suspended() override { if (hhs::saveBackupMain) hhs::saveBackupMain(); }   // phone backgrounded: the OS may kill it next
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
           #if JUCE_IOS || JUCE_ANDROID
            setFullScreen (true);
           #else
            setResizable (true, true);
            setResizeLimits (1000, 640, 10000, 8000);
            centreWithSize (1440, 900);
           #endif
            setVisible (true);
            hhs::makeCaptureFriendly (*this);
           #if ! HHS_LICENSED
            juce::MessageManager::callAsync ([] { hhs::showWhatsNewIfUpdated(); });
           #endif
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

private:
   #if HHS_KEY_GATE
    struct Watch : public juce::Timer { std::function<void()> fn; void timerCallback() override { if (fn) fn(); } };
    juce::String launchLine;
    juce::ThreadPool licensePool { 1 };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    std::unique_ptr<juce::DocumentWindow> activationWindow;
    juce::Component::SafePointer<ActivationPanel> activationPanel;
    Watch licenseWatch;

    void showActivation (const juce::String& message)
    {
        auto* p = new ActivationPanel (message);
        juce::Component::SafePointer<ActivationPanel> sp (p);
        p->onActivate = [this, sp] (juce::String key) {
            licensePool.addJob ([this, sp, key, alive = alive] {
                const auto r = hhs::license::activateOnline (key, hhs::license::machineId());
                juce::MessageManager::callAsync ([this, sp, key, r, alive] {
                    if (! *alive || sp == nullptr) return;
                    if (! r.ok) { sp->failed (r.error); return; }
                    if (! hhs::license::saveActivation (key, r.pass)) { sp->failed ("Couldn't save the activation on this computer."); return; }
                    activationWindow = nullptr;
                    startMain (launchLine);
                    watchLicense();
                });
            });
        };
        activationWindow = std::make_unique<ActivationWindow> (p);
        activationPanel = sp;
    }
    void lockApp (const juce::String& why)
    {
        hhs::license::clearActivation();
        if (window != nullptr && hhs::saveBackupMain) hhs::saveBackupMain();
        welcomeWindow = nullptr;
        window = nullptr;
        showActivation (why);
    }
    // once a day online, re-check the key; a revoked key or a 4th computer locks right away
    void renewInBackground (bool force = false)
    {
        watchLicense();
        const auto p = hhs::license::storedPass();
        if (! force && p.ok && juce::Time::currentTimeMillis() / 1000 - p.issued < 23 * 3600) return;
        const auto key = hhs::license::keyFile().loadFileAsString().trim();
        licensePool.addJob ([this, key, alive = alive] {
            auto r = hhs::license::activateOnline (key, hhs::license::machineId());
            auto used = key;
            if (! r.ok && ! r.revoked && key.isNotEmpty())   // an old / mistyped saved key: this computer's own license instead
            {
                const auto d = hhs::license::activateOnline ({}, hhs::license::machineId());
                if (d.ok || d.revoked) { r = d; used = {}; }
                else r.lockNow = false;
            }
            juce::MessageManager::callAsync ([this, key = used, r, alive] {
                if (! *alive) return;
                if (r.ok) hhs::license::saveActivation (key, r.pass);
                else if (r.lockNow) lockApp (r.error);
            });
        });
    }
    void watchLicense()
    {
        licenseWatch.fn = [this] {
            if (window == nullptr) return;
            const auto p = hhs::license::storedPass();
            if (! p.ok || juce::Time::currentTimeMillis() / 1000 - p.issued >= 23 * 3600) renewInBackground();   // only the server's revoke locks
        };
        licenseWatch.startTimer (20 * 60 * 1000);
    }
   #endif
    hhs::HHLookAndFeel laf;
    std::unique_ptr<MainWindow> window;
    juce::StringArray pendingOpen;
    std::unique_ptr<juce::DocumentWindow> welcomeWindow;
};

START_JUCE_APPLICATION (HumHouseApp)
