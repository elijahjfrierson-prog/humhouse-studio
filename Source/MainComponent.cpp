#include "UI.h"

namespace hhs
{
// ------------------------------------------------------------------ look and feel
HHLookAndFeel::HHLookAndFeel() { refreshColours(); }

void HHLookAndFeel::refreshColours()
{
    setColourScheme ({ col::bg, col::panel2, col::panel, col::gridBar, col::text, col::accent,
                       juce::Colours::black, col::accent, col::text });
    setColour (juce::TextButton::buttonColourId, col::panel2);
    setColour (juce::TextButton::buttonOnColourId, col::accent);
    setColour (juce::TextButton::textColourOffId, col::text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    setColour (juce::ComboBox::backgroundColourId, col::panel2);
    setColour (juce::ComboBox::outlineColourId, col::gridBar);
    setColour (juce::Slider::rotarySliderFillColourId, col::accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, col::gridBar);
    setColour (juce::Slider::trackColourId, col::accent);
    setColour (juce::Slider::backgroundColourId, col::grid);
    setColour (juce::Slider::thumbColourId, col::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TooltipWindow::backgroundColourId, col::panel2);
    setColour (juce::TooltipWindow::textColourId, col::text);
    setColour (juce::TooltipWindow::outlineColourId, col::accent);
    setColour (juce::PopupMenu::backgroundColourId, col::panel);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, col::accent);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::black);
    setColour (juce::Label::textColourId, col::text);
    setColour (juce::ListBox::backgroundColourId, col::panel);
    setColour (juce::TextEditor::backgroundColourId, col::panel2);
    setColour (juce::TextEditor::outlineColourId, col::gridBar);
    setColour (juce::ResizableWindow::backgroundColourId, col::bg);
    setColour (juce::ScrollBar::thumbColourId, col::gridBar);
    setColour (juce::DirectoryContentsDisplayComponent::textColourId, col::text);
    setColour (juce::DirectoryContentsDisplayComponent::highlightColourId, col::accent.withAlpha (0.5f));
    setColour (juce::FileTreeComponent::backgroundColourId, col::panel);
}

void HHLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider& s)
{
    auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (2.0f);
    const float size = std::min (r.getWidth(), r.getHeight());
    auto c = r.withSizeKeepingCentre (size, size);
    const float ang = a0 + pos * (a1 - a0), rad = size * 0.5f - 2.0f;
    g.setColour (col::panel2.brighter (0.05f));
    g.fillEllipse (c.reduced (3.0f));
    juce::Path track, arc;
    track.addCentredArc (c.getCentreX(), c.getCentreY(), rad, rad, 0, a0, a1, true);
    g.setColour (col::bg);
    g.strokePath (track, juce::PathStrokeType (3.0f));
    arc.addCentredArc (c.getCentreX(), c.getCentreY(), rad, rad, 0, a0, ang, true);
    g.setColour (s.isEnabled() ? s.findColour (juce::Slider::rotarySliderFillColourId) : col::dim);
    g.strokePath (arc, juce::PathStrokeType (3.0f));
    g.setColour (col::text);
    g.drawLine (juce::Line<float> (c.getCentre().getPointOnCircumference (rad * 0.25f, ang),
                                   c.getCentre().getPointOnCircumference (rad - 3.0f, ang)), 2.0f);
}

void HHLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    auto base = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : b.findColour (juce::TextButton::buttonColourId);
    if (down) base = base.brighter (0.2f);
    else if (over) base = base.brighter (0.08f);
    g.setColour (base);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (col::gridBar.withAlpha (0.7f));
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
}

void HHLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                                      juce::Slider::SliderStyle style, juce::Slider& s)
{
    if (style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, minPos, maxPos, style, s);
        return;
    }
    const float cx = (float) x + (float) w * 0.5f;
    g.setColour (col::bg);
    g.fillRoundedRectangle (cx - 2.5f, (float) y, 5.0f, (float) h, 2.5f);
    g.setColour (col::accent.withAlpha (0.65f));
    g.fillRoundedRectangle (cx - 2.5f, pos, 5.0f, (float) (y + h) - pos, 2.5f);
    juce::Rectangle<float> cap (cx - 13.0f, pos - 7.0f, 26.0f, 14.0f);
    g.setColour (col::panel2.brighter (0.3f));
    g.fillRoundedRectangle (cap, 3.0f);
    g.setColour (col::text);
    g.drawLine (cap.getX() + 4, pos, cap.getRight() - 4, pos, 1.5f);
}

Knob::Knob (const juce::String& name, const juce::String& help, double mn, double mx, double def, const juce::String& suffix)
    : juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox)
{
    setName (name);
    setTooltip (name + " - " + help + "  (double-click resets)");
    setRange (mn, mx, 0.0);
    setValue (def, juce::dontSendNotification);
    setDoubleClickReturnValue (true, def);
    setTextValueSuffix (suffix);
    setPopupDisplayEnabled (true, true, nullptr, 1500);
    setMouseDragSensitivity (180);
}

void setupButton (juce::Button& b, const juce::String& text, const juce::String& tooltip, bool toggle)
{
    b.setButtonText (text);
    b.setTooltip (tooltip);
    b.setClickingTogglesState (toggle);
}

void askText (const juce::String& title, const juce::String& initial, std::function<void (juce::String)> done)
{
    auto* w = new juce::AlertWindow (title, {}, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("t", initial);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([w, done] (int r) {
        if (r == 1) done (w->getTextEditorContents ("t"));
    }), true);
}

// ------------------------------------------------------------------ studio
void Studio::commit (const juce::String& what)
{
    undo.push (p(), what);
    engine.sync();
    dirty = true;
    sendChangeMessage();
}

void Studio::undoStep (int delta)
{
    bool ok = false;
    {
        const juce::ScopedLock sl (engine.getLock());
        ok = delta < 0 ? undo.undo (p()) : undo.redo (p());
    }
    if (ok) { engine.sync(); dirty = true; sendChangeMessage(); }
}

void Studio::jumpHistory (int index)
{
    bool ok = false;
    { const juce::ScopedLock sl (engine.getLock()); ok = undo.jumpTo (index, p()); }
    if (ok) { engine.sync(); dirty = true; sendChangeMessage(); }
}

NoteList& Studio::notes (int channelId)
{
    auto& pr = p();
    if (pr.patterns.empty()) pr.patterns.push_back ({ "Pattern 1" });
    pr.currentPattern = juce::jlimit (0, (int) pr.patterns.size() - 1, pr.currentPattern);
    return pr.patterns[(size_t) pr.currentPattern].notes[channelId];
}

Channel* Studio::channel (int id)
{
    for (auto& c : p().channels) if (c.id == id) return &c;
    return nullptr;
}

// ------------------------------------------------------------------ main component
namespace
{
enum MenuIds
{
    mNew = 1, mOpen, mOpenBackup, mSave, mSaveAs, mExport, mExportTrack, mQuit, mSaveTemplate, mDefaultTemplate, mClearDefaultTemplate,
    mUndo = 20, mRedo, mBackupFolder,
    mBeat = 40, mRecord, mMix, mBrowser, mSamplesUsed,
    mAudio = 60, mPlugins, mAutoBackup, mTheme, mClearBlacklist,
    mShortcuts = 80, mAbout,
    mHistory = 1000
};

struct RenderJob : public juce::ThreadWithProgressWindow
{
    RenderJob (Engine& e, juce::File f, bool song, int t)
        : juce::ThreadWithProgressWindow ("Exporting " + f.getFileName() + " ...", true, true), engine (e), file (f), songMode (song), track (t) {}
    void run() override
    {
        ok = engine.renderToWav (file, songMode, [this] (float p) { setProgress (p); return ! threadShouldExit(); }, track);
    }
    void threadComplete (bool cancelled) override
    {
        if (! cancelled)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Export",
                                                    ok ? "Saved " + file.getFullPathName() : "Export failed.");
        juce::MessageManager::callAsync ([this] { delete this; });
    }
    Engine& engine; juce::File file; bool songMode; int track; bool ok = false;
};
}

class MainComponent : public juce::Component, public juce::MenuBarModel, private juce::Timer, private juce::ChangeListener
{
public:
    MainComponent()
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "HumHouse Studio";
        o.filenameSuffix = ".settings";
        o.folderName = "HumHouse Studio";
        o.osxLibrarySubFolder = "Application Support";
        props.setStorageParameters (o);
        studio.settings = props.getUserSettings();

        if (studio.settings->containsKey ("uiScale"))
            juce::Desktop::getInstance().setGlobalScaleFactor ((float) juce::jlimit (0.75, 2.0, studio.settings->getDoubleValue ("uiScale", 1.0)));
        if (studio.settings->containsKey ("themeColour"))
            applyTheme (juce::Colour::fromString (studio.settings->getValue ("themeColour")), (float) studio.settings->getDoubleValue ("themeTint", 0.5));
        auto audioXml = studio.settings->getXmlValue ("audio");
        engine.initialiseAudio (audioXml.get());
        if (auto pl = studio.settings->getXmlValue ("plugins")) engine.knownPlugins.recreateFromXml (*pl);
        engine.knownPlugins.addChangeListener (this);
        engine.onPluginDiscard = [] (juce::AudioProcessor* p) { closeEditorsForPlugin (p); };
        const auto crashed = engine.recoverFromPluginCrash();
        if (crashed.isNotEmpty())
            juce::MessageManager::callAsync ([crashed] {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Plugin disabled",
                    crashed + " crashed HumHouse Studio last time, so it was switched off to keep your session safe.\n\n"
                    "Your work is in File > Open backup. To try it again: Options > Re-enable crashed plugins.");
            });
        engine.recordingFolder = docsDir().getChildFile ("Recordings");
        engine.recordingFolder.createDirectory();
        juce::Component::SafePointer<MainComponent> sp (this);
        engine.onRecordingAdded = [sp] { juce::MessageManager::callAsync ([sp] { if (sp) sp->studio.commit ("Record take"); }); };

        engine.newProject (Project::createDefault());
        studio.undo.push (engine.project(), "New project");
        studio.selectedChannel = engine.project().channels.empty() ? -1 : engine.project().channels.front().id;
        studio.showView = [this] (int v) { setView (v); };
        studio.addChangeListener (this);

        rack = createChannelRack (studio);
        piano = createPianoRoll (studio);
        playlist = createPlaylist (studio);
        mixer = createMixer (studio);
        browser = createBrowser (studio);
        for (auto* c : { rack.get(), piano.get(), playlist.get(), mixer.get(), browser.get() }) addChildComponent (c);

        titleLabel.setText ("HUMHOUSE", juce::dontSendNotification);
        titleLabel.setFont (juce::FontOptions (19.0f, juce::Font::bold));
        titleLabel.setColour (juce::Label::textColourId, col::accent);

        int i = 0;
        for (auto* t : { &beatTab, &recordTab, &mixTab })
        {
            static const char* tips[] { "Beat: Channel Rack + Piano Roll (F6 / F7)", "Record: Playlist - arm tracks, record vocals, arrange (F5)",
                                        "Mix: Mixer - inserts, FX, click-to-route (F9)" };
            t->setTooltip (tips[i]);
            t->setRadioGroupId (100);
            t->setClickingTogglesState (true);
            const int v = i++;
            t->onClick = [this, v] { setView (v); };
            addAndMakeVisible (t);
        }
        setupButton (songBtn, "PAT", "Pattern / Song mode (L)", true);
        songBtn.onClick = [this] { studio.edit ("Pattern/Song", [this] (Project& p) { p.songMode = songBtn.getToggleState(); }); };
        setupButton (playBtn, juce::String::fromUTF8 ("\xe2\x96\xb6"), "Play / pause (Space)");
        playBtn.onClick = [this] { engine.togglePlay(); };
        setupButton (stopBtn, juce::String::fromUTF8 ("\xe2\x96\xa0"), "Stop and return to start");
        stopBtn.onClick = [this] { engine.stop(); };
        setupButton (recBtn, juce::String::fromUTF8 ("\xe2\x97\x8f"), "Record (R): records every armed track at once. Drag the playlist ruler to punch in/out");
        recBtn.setColour (juce::TextButton::buttonOnColourId, col::rec);
        recBtn.onClick = [this] { toggleRecord(); };
        setupButton (metroBtn, "METRO", "Metronome (Ctrl/Cmd+M)", true);
        metroBtn.onClick = [this] { engine.metronomeOn = metroBtn.getToggleState(); };
        setupButton (countBtn, "COUNT-IN", "1 bar count-in / pre-roll before recording", true);
        countBtn.setToggleState (engine.countInOn.load(), juce::dontSendNotification);
        countBtn.onClick = [this] { engine.countInOn = countBtn.getToggleState(); };
        setupButton (browserBtn, "PLUGIN HOUSE", "Show / hide Plugin House and the sample + kit browser (F8)", true);
        browserBtn.setToggleState (true, juce::dontSendNotification);
        browserBtn.onClick = [this] { showBrowser = browserBtn.getToggleState(); resized(); };
        setupButton (themeBtn, "COLOR", "Color wheel: change the overall color of the DAW");
        themeBtn.onClick = [this] { showThemeDialog (studio); };
        for (auto* b : { &songBtn, &playBtn, &stopBtn, &recBtn, &metroBtn, &countBtn, &browserBtn, &themeBtn }) addAndMakeVisible (b);

        bpm.setSliderStyle (juce::Slider::LinearBar);
        bpm.setRange (40.0, 300.0, 0.01);
        bpm.setTextValueSuffix (" BPM");
        bpm.setTooltip ("Tempo - drag up/down or double-click to type");
        bpm.setColour (juce::Slider::trackColourId, col::panel2);
        bpm.setValue (engine.project().bpm, juce::dontSendNotification);
        bpm.onValueChange = [this] { studio.live ([this] (Project& p) { p.bpm = bpm.getValue(); }); if (! bpm.isMouseButtonDown()) studio.commit ("Tempo"); };
        bpm.onDragEnd = [this] { studio.commit ("Tempo"); };
        addAndMakeVisible (bpm);

        for (auto* l : { &posLabel, &cpuLabel })
        {
            l->setFont (juce::FontOptions (15.0f, juce::Font::bold));
            l->setJustificationType (juce::Justification::centred);
            l->setColour (juce::Label::backgroundColourId, col::bg);
            addAndMakeVisible (l);
        }
        posLabel.setTooltip ("Song position  bar : beat : step");
        cpuLabel.setTooltip ("CPU load of the audio engine. Freeze heavy tracks in the Mixer to save CPU");
        hintBar.setColour (juce::Label::textColourId, col::accent2);
        hintBar.setColour (juce::Label::backgroundColourId, col::panel);
        hintBar.setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (hintBar);

#if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (this);
#else
        addAndMakeVisible (menuBar);
#endif
        setWantsKeyboardFocus (true);
        setSize (1440, 900);
        setView (0);
        syncControls();
        startTimerHz (30);
    }

    ~MainComponent() override
    {
        stopTimer();
#if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (nullptr);
#endif
        closeAllEditorWindows();
        engine.onPluginDiscard = nullptr;
        engine.knownPlugins.removeChangeListener (this);
        if (auto x = engine.deviceManager.createStateXml()) studio.settings->setValue ("audio", x.get());
        if (studio.dirty) { engine.storePluginStates(); writeBackup (engine.project()); }
        studio.settings->saveIfNeeded();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::bg);
        g.setColour (col::panel);
        g.fillRect (topBar);
        drawLogo (g, logoArea.toFloat(), true);
    }

    void resized() override
    {
        auto r = getLocalBounds();
#if ! JUCE_MAC
        menuBar.setBounds (r.removeFromTop (22));
#endif
        topBar = r.removeFromTop (48);
        auto t = topBar.reduced (8, 8);
        logoArea = t.removeFromLeft (150);
        for (auto* b : { &beatTab, &recordTab, &mixTab }) { b->setBounds (t.removeFromLeft (84)); t.removeFromLeft (4); }
        t.removeFromLeft (16);
        songBtn.setBounds (t.removeFromLeft (48)); t.removeFromLeft (4);
        for (auto* b : { &playBtn, &stopBtn, &recBtn }) { b->setBounds (t.removeFromLeft (40)); t.removeFromLeft (4); }
        t.removeFromLeft (8);
        bpm.setBounds (t.removeFromLeft (110)); t.removeFromLeft (6);
        posLabel.setBounds (t.removeFromLeft (110)); t.removeFromLeft (6);
        metroBtn.setBounds (t.removeFromLeft (62)); t.removeFromLeft (4);
        countBtn.setBounds (t.removeFromLeft (80)); t.removeFromLeft (8);
        browserBtn.setBounds (t.removeFromRight (120)); t.removeFromRight (8);
        themeBtn.setBounds (t.removeFromRight (64)); t.removeFromRight (6);
        cpuLabel.setBounds (t.removeFromRight (86));
        hintBar.setBounds (r.removeFromBottom (22));
        browser->setVisible (showBrowser);
        if (showBrowser) browser->setBounds (r.removeFromLeft (260));
        rack->setVisible (view == 0); piano->setVisible (view == 0);
        playlist->setVisible (view == 1); mixer->setVisible (view == 2);
        if (view == 0) { rack->setBounds (r.removeFromTop (juce::jmax (180, (int) (r.getHeight() * 0.42f)))); piano->setBounds (r); }
        else if (view == 1) playlist->setBounds (r);
        else mixer->setBounds (r);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        const int cmd = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier, alt = juce::ModifierKeys::altModifier;
        auto is = [&k] (int key, int mods) { return k == juce::KeyPress (key, mods, 0); };
        if (is (juce::KeyPress::spaceKey, 0)) { engine.togglePlay(); return true; }
        if (is ('r', 0)) { engine.setRecording (! engine.isRecording()); return true; }
        if (is ('l', 0)) { songBtn.setToggleState (! songBtn.getToggleState(), juce::sendNotificationSync); return true; }
        if (is (juce::KeyPress::F5Key, 0)) { setView (1); return true; }
        if (is (juce::KeyPress::F6Key, 0) || is (juce::KeyPress::F7Key, 0)) { setView (0); if (k.getKeyCode() == juce::KeyPress::F7Key) piano->grabKeyboardFocus(); return true; }
        if (is (juce::KeyPress::F8Key, 0)) { browserBtn.setToggleState (! showBrowser, juce::sendNotificationSync); return true; }
        if (is (juce::KeyPress::F9Key, 0)) { setView (2); return true; }
        if (is ('s', cmd)) { save (false); return true; }
        if (is ('s', cmd | shift)) { save (true); return true; }
        if (is ('o', cmd)) { open (false); return true; }
        if (is ('n', cmd)) { newProject(); return true; }
        if (is ('z', cmd) || is ('z', cmd | alt)) { studio.undoStep (-1); return true; }
        if (is ('z', cmd | shift) || is ('y', cmd)) { studio.undoStep (1); return true; }
        if (is ('m', cmd)) { metroBtn.setToggleState (! metroBtn.getToggleState(), juce::sendNotificationSync); return true; }
        if (is ('r', cmd)) { exportWav (-1); return true; }
        return false;
    }

    // ---- menus
    juce::StringArray getMenuBarNames() override { return { "File", "Edit", "View", "Options", "Help" }; }

    juce::PopupMenu getMenuForIndex (int index, const juce::String&) override
    {
        juce::PopupMenu m;
        auto add = [&m] (int id, const juce::String& text, const juce::String& shortcut = {}, bool enabled = true, bool ticked = false)
        {
            juce::PopupMenu::Item it (text);
            it.itemID = id; it.shortcutKeyDescription = shortcut; it.isEnabled = enabled; it.isTicked = ticked;
            m.addItem (it);
        };
        if (index == 0)
        {
            add (mNew, "New project", "Ctrl+N"); add (mOpen, "Open...", "Ctrl+O"); add (mOpenBackup, "Open backup...");
            m.addSeparator();
            add (mSave, "Save (+ backup)", "Ctrl+S"); add (mSaveAs, "Save as...", "Ctrl+Shift+S");
            {
                juce::PopupMenu tm;
                auto ts = templateFiles();
                for (int i = 0; i < ts.size() && i < 90; ++i) tm.addItem (300 + i, ts[i].getFileNameWithoutExtension());
                if (ts.isEmpty()) tm.addItem (-1, "(no templates yet)", false);
                m.addSubMenu ("New from template", tm);
            }
            add (mSaveTemplate, "Save as template...");
            add (mDefaultTemplate, "Use this project as default template");
            add (mClearDefaultTemplate, "Reset default template", {}, defaultTemplate().existsAsFile());
            m.addSeparator();
            add (mExport, "Export WAV...", "Ctrl+R"); add (mExportTrack, "Export selected mixer track (stem)...");
            m.addSeparator(); add (mQuit, "Quit");
        }
        else if (index == 1)
        {
            add (mUndo, "Undo" + (studio.undo.canUndo() ? " " + studio.undo.description (studio.undo.current()) : juce::String()), "Ctrl+Z", studio.undo.canUndo());
            add (mRedo, "Redo", "Ctrl+Shift+Z / Ctrl+Y", studio.undo.canRedo());
            juce::PopupMenu hist;
            const int n = studio.undo.size();
            for (int i = n - 1; i >= juce::jmax (0, n - 60); --i)
                hist.addItem (mHistory + i, juce::String (i + 1) + ". " + studio.undo.description (i), true, i == studio.undo.current());
            m.addSubMenu ("History (999 levels)", hist);
            m.addSeparator(); add (mBackupFolder, "Show backup folder");
        }
        else if (index == 2)
        {
            add (mBeat, "Beat (Channel Rack + Piano Roll)", "F6 / F7", true, view == 0);
            add (mRecord, "Record (Playlist)", "F5", true, view == 1);
            add (mMix, "Mix (Mixer)", "F9", true, view == 2);
            add (mBrowser, "Plugin House / Browser", "F8", true, showBrowser);
            m.addSeparator();
            add (mSamplesUsed, "Samples used in this project...");
        }
        else if (index == 3)
        {
            add (mAudio, "Audio settings (device, buffer size, sample rate)...");
            add (mPlugins, "Plugin House: scan / manage plugins...");
            add (mAutoBackup, "Auto-backup every 5 minutes", {}, true, autoBackup());
            m.addSeparator();
            add (mTheme, "Theme color wheel...");
            {
                juce::PopupMenu sc;
                const float cur = juce::Desktop::getInstance().getGlobalScaleFactor();
                const int pcts[] { 75, 90, 100, 110, 125, 150, 175, 200 };
                for (int i = 0; i < 8; ++i) sc.addItem (200 + i, juce::String (pcts[i]) + "%", true, std::abs (cur * 100.0f - (float) pcts[i]) < 1.0f);
                m.addSubMenu ("UI scale", sc);
            }
            add (mClearBlacklist, "Re-enable crashed plugins", {}, engine.knownPlugins.getBlacklistedFiles().size() > 0);
        }
        else
        {
            add (mShortcuts, "Keyboard shortcuts"); add (mAbout, "About HumHouse Studio");
        }
        return m;
    }

    void menuItemSelected (int id, int) override
    {
        if (id >= mHistory) { studio.jumpHistory (id - mHistory); return; }
        if (id >= 300 && id < 390)
        {
            auto ts = templateFiles();
            if (id - 300 < ts.size()) newProject (ts[id - 300]);
            return;
        }
        if (id >= 200 && id < 208)
        {
            const int pcts[] { 75, 90, 100, 110, 125, 150, 175, 200 };
            const float f = (float) pcts[id - 200] / 100.0f;
            juce::Desktop::getInstance().setGlobalScaleFactor (f);
            if (studio.settings) { studio.settings->setValue ("uiScale", (double) f); studio.settings->saveIfNeeded(); }
            return;
        }
        switch (id)
        {
            case mNew: newProject(); break;
            case mSaveTemplate: saveTemplate(); break;
            case mDefaultTemplate:
                engine.storePluginStates();
                templatesDir().createDirectory();
                saveProject (engine.project(), defaultTemplate());
                break;
            case mClearDefaultTemplate: defaultTemplate().deleteFile(); break;
            case mOpen: open (false); break;
            case mOpenBackup: open (true); break;
            case mSave: save (false); break;
            case mSaveAs: save (true); break;
            case mExport: exportWav (-1); break;
            case mExportTrack: exportWav (studio.selectedTrack); break;
            case mQuit: juce::JUCEApplication::getInstance()->systemRequestedQuit(); break;
            case mUndo: studio.undoStep (-1); break;
            case mRedo: studio.undoStep (1); break;
            case mBackupFolder: backupDir().createDirectory(); backupDir().startAsProcess(); break;
            case mBeat: setView (0); break;
            case mRecord: setView (1); break;
            case mMix: setView (2); break;
            case mSamplesUsed:
            {
                juce::StringArray used;
                for (auto& c : engine.project().channels)
                    if (c.type == ChannelType::Sampler && c.sampleFile != juce::File()) used.addIfNotAlreadyThere (c.name + "   -   " + c.sampleFile.getFullPathName());
                for (auto& c : engine.project().clips)
                    if (c.isAudio() && ! c.hidden) used.addIfNotAlreadyThere ("[audio] " + c.audioFile.getFileName() + "   -   " + c.audioFile.getFullPathName());
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, "Samples used (" + juce::String (used.size()) + ")",
                                                        used.isEmpty() ? juce::String ("No samples yet - built-in drums only.") : used.joinIntoString ("\n"));
                break;
            }
            case mBrowser: browserBtn.setToggleState (! showBrowser, juce::sendNotificationSync); break;
            case mAudio: showAudioSettings(); break;
            case mPlugins: showPluginScanner (studio); break;
            case mTheme: showThemeDialog (studio); break;
            case mClearBlacklist: engine.knownPlugins.clearBlacklistedFiles(); flash ("Crashed plugins re-enabled"); break;
            case mAutoBackup: studio.settings->setValue ("autoBackup", ! autoBackup()); break;
            case mShortcuts: showShortcuts(); break;
            case mAbout:
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "HumHouse Studio",
                    "Beat-making and vocal-recording studio.\nFL-style editing, BandLab-style flow.");
                break;
            default: break;
        }
    }

private:
    juce::ApplicationProperties props;
    Engine engine;
    Studio studio { engine };
    juce::TooltipWindow tooltips { this, 450 };
    std::unique_ptr<juce::Component> rack, piano, playlist, mixer, browser;
    juce::Label titleLabel, posLabel, cpuLabel, hintBar;
    juce::TextButton beatTab { "BEAT" }, recordTab { "RECORD" }, mixTab { "MIX" };
    juce::TextButton songBtn, playBtn, stopBtn, recBtn, metroBtn, countBtn, browserBtn, themeBtn;
    juce::Slider bpm;
    juce::MenuBarComponent menuBar { this };
    juce::Rectangle<int> topBar, logoArea;
    std::unique_ptr<juce::FileChooser> chooser;
    int view = 0, ticks = 0;
    bool showBrowser = true;
    juce::String flashText;
    juce::uint32 flashUntil = 0;

    static juce::File docsDir() { return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("HumHouse Studio"); }
    juce::File backupDir() const
    {
        auto& p = const_cast<Engine&> (engine).project();
        return (p.projectFile.existsAsFile() ? p.projectFile.getParentDirectory() : docsDir()).getChildFile ("Backup");
    }
    bool autoBackup() const { return studio.settings->getBoolValue ("autoBackup", true); }

    void flash (const juce::String& t) { flashText = t; flashUntil = juce::Time::getMillisecondCounter() + 2500; }

    void setView (int v)
    {
        view = v;
        juce::TextButton* tabs[] { &beatTab, &recordTab, &mixTab };
        tabs[v]->setToggleState (true, juce::dontSendNotification);
        resized();
    }

    void syncControls()
    {
        auto& p = engine.project();
        bpm.setValue (p.bpm, juce::dontSendNotification);
        songBtn.setToggleState (p.songMode, juce::dontSendNotification);
        songBtn.setButtonText (p.songMode ? "SONG" : "PAT");
        if (auto* w = getTopLevelComponent()) w->setName (p.title + (studio.dirty ? " *" : "") + " - HumHouse Studio");
    }

    void changeListenerCallback (juce::ChangeBroadcaster* src) override
    {
        if (src == &engine.knownPlugins)
        {
            if (auto x = engine.knownPlugins.createXml()) studio.settings->setValue ("plugins", x.get());
            studio.settings->saveIfNeeded();
            return;
        }
        syncControls();
    }

    void toggleRecord()
    {
        const bool on = ! engine.isRecording();
        engine.setRecording (on);
        if (on && ! engine.isPlaying()) engine.play();
    }

    void timerCallback() override
    {
        const double pos = engine.positionBeats();
        const int bpb = juce::jmax (1, engine.project().beatsPerBar);
        const int bar = (int) (pos / bpb) + 1, beat = (int) std::fmod (pos, (double) bpb) + 1, step = (int) (std::fmod (pos, 1.0) * 4) + 1;
        posLabel.setText (juce::String (bar) + ":" + juce::String (beat) + ":" + juce::String (step), juce::dontSendNotification);
        const int cpu = juce::roundToInt (engine.cpuLoad() * 100.0);
        cpuLabel.setText ("CPU " + juce::String (cpu) + "%", juce::dontSendNotification);
        cpuLabel.setColour (juce::Label::textColourId, cpu > 80 ? col::rec : cpu > 50 ? col::accent : col::good);
        recBtn.setToggleState (engine.isRecording(), juce::dontSendNotification);
        playBtn.setToggleState (engine.isPlaying(), juce::dontSendNotification);
        playBtn.setColour (juce::TextButton::buttonOnColourId, engine.isCountingIn() ? col::accent : col::good);

        juce::String hint;
        if (juce::Time::getMillisecondCounter() < flashUntil) hint = flashText;
        else
        {
            auto* c = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
            for (auto* p = c; p != nullptr && hint.isEmpty(); p = p->getParentComponent())
                if (auto* tc = dynamic_cast<juce::TooltipClient*> (p)) hint = tc->getTooltip();
            if (auto* s = dynamic_cast<juce::Slider*> (c)) hint << "    |    " << s->getTextFromValue (s->getValue());
        }
        if (hint != hintBar.getText()) hintBar.setText (hint, juce::dontSendNotification);

        if (++ticks >= 30 * 300)
        {
            ticks = 0;
            if (autoBackup() && studio.dirty)
            {
                engine.storePluginStates();
                if (writeBackup (engine.project()).existsAsFile()) flash ("Auto-backup saved");
            }
        }
    }

    void writeProject()
    {
        engine.storePluginStates();
        auto& p = engine.project();
        if (saveProject (p, p.projectFile))
        {
            writeBackup (p);
            studio.dirty = false;
            syncControls();
            flash ("Saved " + p.projectFile.getFileName() + " + backup");
        }
        else juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save", "Could not save the project.");
    }

    void save (bool as)
    {
        auto& p = engine.project();
        if (! as && p.projectFile.existsAsFile()) { writeProject(); return; }
        docsDir().createDirectory();
        chooser = std::make_unique<juce::FileChooser> ("Save project", docsDir().getChildFile (p.title + ".hhs"), "*.hhs");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this] (const juce::FileChooser& fc) {
                                  auto f = fc.getResult();
                                  if (f == juce::File()) return;
                                  f = f.withFileExtension ("hhs");
                                  engine.project().projectFile = f;
                                  engine.project().title = f.getFileNameWithoutExtension();
                                  writeProject();
                              });
    }

    void load (const juce::File& f)
    {
        Project q;
        if (! loadProject (q, f))
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Open", "Not a HumHouse project.");
            return;
        }
        if (f.getParentDirectory().getFileName() == "Backup") q.projectFile = juce::File();
        closeAllEditorWindows();
        engine.newProject (std::move (q));
        studio.undo.clear();
        studio.undo.push (engine.project(), "Open");
        studio.selectedChannel = engine.project().channels.empty() ? -1 : engine.project().channels.front().id;
        studio.dirty = false;
        studio.sendChangeMessage();
    }

    void open (bool backups)
    {
        auto dir = backups ? backupDir() : docsDir();
        chooser = std::make_unique<juce::FileChooser> (backups ? "Open backup" : "Open project", dir, "*.hhs");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) load (fc.getResult()); });
    }

    static juce::File templatesDir() { return docsDir().getChildFile ("Templates"); }
    static juce::File defaultTemplate() { return templatesDir().getChildFile ("_default.hhs"); }
    static juce::Array<juce::File> templateFiles()
    {
        auto fs = templatesDir().findChildFiles (juce::File::findFiles, false, "*.hhs");
        fs.removeIf ([] (const juce::File& f) { return f.getFileName() == "_default.hhs"; });
        fs.sort();
        return fs;
    }
    void saveTemplate()
    {
        auto* aw = new juce::AlertWindow ("Save as template", "Name this template (tracks, mixer, FX and kit are saved)", juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("name", "My template");
        aw->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([this, aw] (int r) {
            const auto name = juce::File::createLegalFileName (aw->getTextEditorContents ("name").trim());
            if (r != 1 || name.isEmpty()) return;
            engine.storePluginStates();
            templatesDir().createDirectory();
            saveProject (engine.project(), templatesDir().getChildFile (name + ".hhs"));
        }), true);
    }

    void newProject (juce::File templ = {})
    {
        if (studio.dirty) { engine.storePluginStates(); writeBackup (engine.project()); }
        closeAllEditorWindows();
        if (templ == juce::File()) templ = defaultTemplate();
        Project fresh = Project::createDefault();
        if (templ.existsAsFile())
        {
            Project t;
            if (loadProject (t, templ)) { fresh = std::move (t); fresh.title = Project().title; }
        }
        engine.newProject (std::move (fresh));
        studio.undo.clear();
        studio.undo.push (engine.project(), "New project");
        studio.selectedChannel = engine.project().channels.empty() ? -1 : engine.project().channels.front().id;
        studio.dirty = false;
        studio.sendChangeMessage();
    }

    void exportWav (int track)
    {
        auto& p = engine.project();
        auto name = p.title + (track >= 0 && track < (int) p.mixer.size() ? " - " + p.mixer[(size_t) track].name : juce::String());
        chooser = std::make_unique<juce::FileChooser> ("Export WAV", docsDir().getChildFile (name + ".wav"), "*.wav");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, track] (const juce::FileChooser& fc) {
                                  auto f = fc.getResult();
                                  if (f == juce::File()) return;
                                  engine.stop();
                                  (new RenderJob (engine, f.withFileExtension ("wav"), engine.project().songMode, track))->launchThread();
                              });
    }

    void showAudioSettings()
    {
        auto* sel = new juce::AudioDeviceSelectorComponent (engine.deviceManager, 0, 32, 0, 2, true, false, true, false);
        sel->setSize (560, 480);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (sel);
        o.dialogTitle = "Audio settings - device, inputs, buffer size, sample rate";
        o.dialogBackgroundColour = col::panel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.launchAsync();
    }

    void showShortcuts()
    {
        juce::String t;
        t << "GLOBAL\nSpace play/stop | R record | L pattern/song | F5 playlist | F6/F7 rack/piano roll | F8 Plugin House | F9 mixer\n"
          << "Ctrl+S save + backup | Ctrl+Shift+S save as | Ctrl+O open | Ctrl+N new | Ctrl+R export WAV | Ctrl+M metronome\n"
          << "Ctrl+Z undo | Ctrl+Shift+Z / Ctrl+Y redo | Ctrl+Alt+Z step back (999 levels)\n\n"
          << "PIANO ROLL\nAlt+A arpeggiator / rolls | Alt+S strum | Alt+U chop | Alt+F flam | Alt+Q quantize | Alt+R randomize\n"
          << "Alt+K limit | Alt+L articulate | Alt+X scale levels | Alt+Y flip | Alt+W claw | Alt+O LFO | Alt+M mute | Alt+V ghost notes\n"
          << "Ctrl+B duplicate | Ctrl+G glue | Ctrl+L legato | Ctrl+U quick chop | Ctrl+Q quick quantize | Shift+Q quantize starts\n"
          << "Shift+Up/Down semitone | Ctrl+Up/Down octave | Shift+Left/Right move | Alt+Left/Right nudge | Ctrl+A select all | Del delete\n"
          << "Ctrl+C / Ctrl+V copy / paste | Shift+D reset lengths | double-click note = slide/glide\n\n"
          << "PLAYLIST\nDrag ruler = punch range (double-click clears) | double-click empty = place pattern | drag clip corners = fades\n"
          << "Alt-drag fade = curve | right-click clip = takes, route to insert, split, fades | Ctrl+B duplicate | Del delete\n\n"
          << "Ctrl = Cmd and Alt = Option on macOS.";
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, "Keyboard shortcuts", t);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

std::unique_ptr<juce::Component> createMainComponent() { return std::make_unique<MainComponent>(); }
}
