#include "UI.h"
#include <thread>
#include "BinaryData.h"

namespace hhs
{
// ------------------------------------------------------------------ look and feel
HHLookAndFeel::HHLookAndFeel() { refreshColours(); }

struct PerfSettings { int threads = 0; bool mt = true, smart = true; };
static PerfSettings& perf();
static juce::File appDataFile (const char* name)
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("HumHouse Studio").getChildFile (name);
}
static PerfSettings& perf()
{
    static PerfSettings p = [] {
        PerfSettings r;
        auto t = juce::StringArray::fromTokens (appDataFile ("performance.txt").loadFileAsString(), " ", "");
        if (t.size() >= 3) { r.threads = t[0].getIntValue(); r.mt = t[1].getIntValue() != 0; r.smart = t[2].getIntValue() != 0; }
        return r; }();
    return p;
}
static void savePerf()
{
    appDataFile ("performance.txt").getParentDirectory().createDirectory();
    appDataFile ("performance.txt").replaceWithText (juce::String (perf().threads) + " " + juce::String ((int) perf().mt) + " " + juce::String ((int) perf().smart));
}
bool& captureFriendlyDrawing() { static bool on = ! appDataFile ("capture.txt").existsAsFile() || appDataFile ("capture.txt").loadFileAsString().getIntValue() != 0; return on; }
int& fontStyle() { static int st = appDataFile ("font.txt").existsAsFile() ? appDataFile ("font.txt").loadFileAsString().getIntValue() : 0; return st; }
void setFontStyle (int st)
{
    fontStyle() = st;
    appDataFile ("font.txt").getParentDirectory().createDirectory();
    appDataFile ("font.txt").replaceWithText (juce::String (st));
    juce::Typeface::clearTypefaceCache();
    for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
        if (auto* w = juce::TopLevelWindow::getTopLevelWindow (i)) { w->sendLookAndFeelChange(); w->repaint(); }
}
juce::Typeface::Ptr HHLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    static auto tattoo = juce::Typeface::createSystemTypefaceFor (BinaryData::PirataOneRegular_ttf, (size_t) BinaryData::PirataOneRegular_ttfSize);
    static auto flash = juce::Typeface::createSystemTypefaceFor (BinaryData::RyeRegular_ttf, (size_t) BinaryData::RyeRegular_ttfSize);
    if (fontStyle() == 0 && tattoo != nullptr) return tattoo;
    if (fontStyle() == 1 && flash != nullptr) return flash;
    return juce::LookAndFeel_V4::getTypefaceForFont (f);
}
void showWhatsNewIfUpdated()
{
    const auto v = juce::JUCEApplication::getInstance()->getApplicationVersion();
    auto f = appDataFile ("last-version.txt");
    if (f.loadFileAsString().trim() == v) return;
    f.getParentDirectory().createDirectory();
    f.replaceWithText (v);
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "HumHouse Studio " + v + " - What's new",
        "- Playlist: E + drag the bar ruler = time selection (copy / paste / Ctrl+B whole bars incl. empty space); Shift+drag ruler = punch\n"
        "- Piano Roll toolbar wraps to two rows when narrow; Glide always visible\n"
        "- Note names match FL: middle C = C5 (C0 to G10)\n"
        "- One-shots: smoother pitched playback (Hermite), SET (wrench) button on every channel, on/off buttons for Normalize, Reverse, Declick...\n"
        "- One-shots / 808 cut mode: no click at the start or when the next hit cuts the last one\n"
        "- Piano Roll LOCK: notes you draw, drag or stamp snap to your key and scale\n"
        "- Sample previews stop when you click anywhere, press Stop or Space\n"
        "- Recording: earlier takes and the beat play instantly when you hit record (no dropout while take files are created, punch pre-roll preloaded)\n"
        "- Studio Vocal (Studio FX): 0-latency vocal smoother - Smooth, De-Ess, Silk air, Warmth + 7 presets\n"
        "- No more phasing on buses: automatic plugin delay compensation like FL, the Mix knob lines dry up with latent FX, mono plugins get the mono sum instead of just the left side\n"
        "- AutoPitch rebuilt: Auto-Tune style single read head spliced on whole pitch cycles - full voice, no clicks or phasing, ~0.1-6 ms\n"
        "- Knobs work like FL: drag up/down or scroll, Shift = fine, double-click to type a value, Alt-click resets\n"
        "- Studio EQ: Pro-Q style band bar (type / freq / gain / Q, type values in), selected band shape, listen (Alt+drag a dot), Shift = fine, display range 6 / 12 / 18 / 30 dB\n"
        "- Trackpad pinch zoom + Ctrl/Cmd+scroll zoom at the mouse in Playlist and Piano Roll\n"
        "- Snap menu in the Playlist and finer snaps everywhere: (none), 1/6, 1/4, 1/3, 1/2 step, 1/6, 1/3, 1/2 beat, beat, bar\n"
        "- FL Playlist keys: E select (drag a box over a chorus), P/B paint, D delete, T mute, C slice\n"
        "- Ctrl/Cmd+C / X / V copy, cut, paste at the playhead; Ctrl/Cmd+B duplicate; Shift+drag clones; Ctrl/Cmd+D deselect\n"
        "- Audio beats no longer cut off at the start: clip audio is loaded before playback begins\n"
        "- Start mid-note (e.g. bar 1.4) and 808s / samples play from the matching point in the sample\n"
        "- Notes no longer cut off in playback: 2x voices, fading voices stolen first, plugins never sleep mid-note\n"
        "- Drag samples from the browser into the Channel Rack or onto the Playlist\n"
        "- Piano Roll MUTE (M): silence note previews while you edit\n"
        "- Play / Stop / Rec buttons, draggable playhead handle in the ruler, 4 ms declick when starting mid-sound\n"
        "- Fixed: first note cut when a held note (808 tail) ran over the loop; crash on New project; auto-backup every 8 edits\n"
        "- New Flanger and Flangus (multi-voice) effects with factory presets\n"
        "- Fade bends: drag the round handle on any fade up / down for curved fades\n"
        "- Smoother UI: Playlist / Piano Roll only redraw what moves while playing; Options > capture-friendly drawing toggle (Windows)\n"
        "- See the take being recorded: live red clip + waveform; quiet takes drawn scaled up\n"
        "- New takes over an earlier take drop to the next lane (FL style) instead of covering it\n"
        "- NORM button on every track: zero-latency auto-normalize for your voice\n"
        "- Louder, fuller gain staging: default faders = 0 dB (no hidden loss), synths +5 dB\n"
        "- Knobs move in clean steps (0.01 / 0.1 / 1), Shift = fine\n"
        "- Studio EQ: 12-band graph EQ like Parametric EQ 2 (7 on by default, double-click to add, drag, wheel = Q, right-click = type / dynamic)\n"
        "- Factory presets on every effect (PRESETS menu)\n"
        "- Options > Performance: processing threads, multithreading, smart disable; CPU % per FX slot\n"
        "- AutoPitch: Pitch (+/-12 st), Detune (+/-100 cents), Tuning A4 400-500 Hz; no more cracks (note hold, octave-error filter, smooth bypass)\n"
        "- Recording input Mono (centred in both ears) or Stereo per track\n- Mix knob on every FX slot now blends dry / wet (e.g. 10% EQ)\n- Double-click a channel to change its sound (keeps notes, steps, routing)\n"
        "- Chord stamp in the Piano Roll (STAMP / Alt+T): 24 chords, inversions, lock to scale\n"
        "- Playhead line across Channel Rack, Piano Roll and Playlist; notes under the playhead always sound\n"
        "- Studio EQ 7: draggable EQ graph + live spectrum, Natural / Linear phase, right-click any band for Dynamic EQ\n"
        "- Compressor transfer curve + GR meter; reverb / delay / filter pictures\n"
        "- Duck knob + SC on every mixer insert (beat dips under your vocal); cleaner wires under the strips (WIRES = show all)\n"
        "- Crash-safe plugin scanning / loading and recording\n- HumHouse logo, color wheel, classic tattoo lettering (Options > Font)\n"
        "- Playlist tools: select, paint, delete, mute, slice, group (Ctrl/Cmd+G), split pattern by channel\n"
        "- Piano roll tool bar, notes and steps light up as they play\n- Channel settings SMP / INS / FUNC: normalize, reverse, trim, fades, Resample / Stretch / Stretch X\n"
        "- One-shot, cut itself, cut groups (808s never overlap)\n- Scope: oscilloscope, waveform and spectrum (SCOPE button + Scope insert)\n"
        "- Mixer routing wires, track select / insert / delete, 10 numbered tracks\n- Presets, FX chain presets (right-click a track), templates, UI scale\n"
        "- Samples browser opens Downloads, with search\n- Shows up in OBS / Discord capture");
}

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
    : juce::Slider (juce::Slider::RotaryVerticalDrag, juce::Slider::NoTextBox)
{
    setName (name);
    setTooltip (name + " - " + help + "  (drag up/down or scroll, Shift = fine, double-click to type, Alt-click resets)");
    // clean steps (0.01 / 0.1 / 1 / 10 by range) instead of 0.000001 values
    const double span = mx - mn;
    const double step = span <= 2.0 ? 0.01 : span <= 60.0 ? 0.1 : span <= 3000.0 ? 1.0 : 10.0;
    setRange (mn, mx, step);
    setNumDecimalPlacesToDisplay (step < 0.1 ? 2 : step < 1.0 ? 1 : 0);
    setVelocityModeParameters (0.4, 1, 0.0, true, juce::ModifierKeys::shiftModifier);   // Shift = fine
    setValue (def, juce::dontSendNotification);
    setDoubleClickReturnValue (true, def);
    setTextValueSuffix (suffix);
    setPopupDisplayEnabled (true, true, nullptr, 1500);
    setMouseDragSensitivity (180);
}

void Knob::mouseDoubleClick (const juce::MouseEvent&)
{
    juce::Component::SafePointer<Knob> self (this);
    askText (getName(), juce::String (getValue(), getNumDecimalPlacesToDisplay()), [self] (juce::String t) {
        if (self != nullptr && t.trim().isNotEmpty())
            self->setValue (juce::jlimit (self->getMinimum(), self->getMaximum(), self->getValueFromText (t)), juce::sendNotificationSync);
    });
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
    if (++editsSinceBackup >= 8) { editsSinceBackup = 0; engine.storePluginStates(); writeBackup (p()); }   // auto-backup every few edits
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
struct BpmSlider : public juce::Slider   // drag / wheel in whole BPM, Shift for 0.01, double-click to type e.g. 144.23
{
    double snapValue (double v, DragMode m) override
    {
        return (m == notDragging || juce::ModifierKeys::currentModifiers.isShiftDown()) ? std::round (v * 100.0) / 100.0 : std::round (v);
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (w.deltaY == 0.0f) return;
        const double step = e.mods.isShiftDown() ? 0.01 : 1.0;
        setValue (std::round ((getValue() + (w.deltaY > 0 ? step : -step)) / step) * step, juce::sendNotificationSync);
    }
    juce::String getTextFromValue (double v) override
    {
        return juce::String (v, std::abs (v - std::round (v)) < 0.005 ? 0 : 2) + " BPM";
    }
};
struct TransportButton : public juce::TextButton   // drawn icons: font glyphs can be missing in the tattoo fonts
{
    int kind = 0;   // 0 play/pause, 1 stop, 2 record
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        getLookAndFeel().drawButtonBackground (g, *this, findColour (getToggleState() ? buttonOnColourId : buttonColourId), over, down);
        auto r = getLocalBounds().toFloat().withSizeKeepingCentre (14.0f, 14.0f);
        const bool on = getToggleState();
        g.setColour (kind == 2 ? (on ? juce::Colours::white : col::rec) : (on ? juce::Colours::black : col::text));
        if (kind == 0 && on) { g.fillRect (r.removeFromLeft (5.0f)); r.removeFromLeft (4.0f); g.fillRect (r.removeFromLeft (5.0f)); }
        else if (kind == 0) { juce::Path p; p.addTriangle (r.getX() + 1, r.getY(), r.getX() + 1, r.getBottom(), r.getRight(), r.getCentreY()); g.fillPath (p); }
        else if (kind == 1) g.fillRect (r.reduced (1.0f));
        else g.fillEllipse (r);
    }
};
enum MenuIds
{
    mNew = 1, mOpen, mOpenBackup, mSave, mSaveAs, mExport, mExportTrack, mQuit, mSaveTemplate, mDefaultTemplate, mClearDefaultTemplate,
    mUndo = 20, mRedo, mBackupFolder,
    mBeat = 40, mRecord, mMix, mBrowser, mSamplesUsed, mDetachRack, mDetachPiano, mDetachPlaylist, mDetachMixer, mDetachBrowser,
    mAudio = 60, mPlugins, mAutoBackup, mTheme, mClearBlacklist, mTypingKeys, mMidiInputs,
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

class MainComponent : public juce::Component, public juce::DragAndDropContainer, public juce::MenuBarModel, private juce::Timer, private juce::ChangeListener
{
public:
    MainComponent()
    {
        juce::Desktop::getInstance().addGlobalMouseListener (&previewStopper);
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
        engine.setPerformance (perf().threads, perf().mt, perf().smart);
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
        setupButton (playBtn, {}, "Play / pause (Space)"); playBtn.kind = 0;
        playBtn.onClick = [this] { engine.togglePlay(); };
        setupButton (stopBtn, {}, "Stop: back to where you started (press again for the song start)"); stopBtn.kind = 1;
        stopBtn.onClick = [this] { engine.stop(); };
        recBtn.kind = 2; setupButton (recBtn, {}, "Record (R): records every armed track at once. Drag the playlist ruler to punch in/out");
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
        setupButton (scopeBtn, "SCOPE", "Scope: oscilloscope, scrolling waveform and spectrum of the master (pops out, resizable)");
        scopeBtn.onClick = [this] { openScopeWindow (studio); };
        addAndMakeVisible (scopeBtn);
        for (juce::Button* b : { (juce::Button*) &songBtn, (juce::Button*) &playBtn, (juce::Button*) &stopBtn, (juce::Button*) &recBtn, (juce::Button*) &metroBtn, (juce::Button*) &countBtn, (juce::Button*) &browserBtn, (juce::Button*) &themeBtn }) addAndMakeVisible (b);

        bpm.setSliderStyle (juce::Slider::LinearBar);
        bpm.setRange (40.0, 300.0, 0.01);
        bpm.setTooltip ("Tempo - drag / scroll in whole BPM, Shift for 0.01, double-click to type (e.g. 144.23)");
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
        typingKeyboard() = studio.settings->getBoolValue ("typingKeys", false);
        liveMidi.fn = [safe = juce::Component::SafePointer<MainComponent> (this)] (int key, float vel, bool on)
        {
            if (safe != nullptr) safe->liveNote (key, vel, on);
        };
        enableMidiInputs();
        engine.deviceManager.addMidiInputDeviceCallback ({}, &liveMidi);
        startTimerHz (30);
    }

    ~MainComponent() override
    {
        juce::Desktop::getInstance().removeGlobalMouseListener (&previewStopper);
        stopTimer();
        engine.deviceManager.removeMidiInputDeviceCallback ({}, &liveMidi);
        popouts.clear();
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
        scopeBtn.setBounds (t.removeFromRight (64)); t.removeFromRight (6);
        cpuLabel.setBounds (t.removeFromRight (86));
        hintBar.setBounds (r.removeFromBottom (22));
        auto here = [this] (const std::unique_ptr<juce::Component>& c) { return popouts.count (c.get()) == 0; };
        if (here (browser)) { browser->setVisible (showBrowser); if (showBrowser) browser->setBounds (r.removeFromLeft (260)); }
        const bool rackHere = here (rack), pianoHere = here (piano);
        if (rackHere) rack->setVisible (view == 0);
        if (pianoHere) piano->setVisible (view == 0);
        if (here (playlist)) playlist->setVisible (view == 1);
        if (here (mixer)) mixer->setVisible (view == 2);
        if (view == 0)
        {
            if (rackHere && pianoHere) { rack->setBounds (r.removeFromTop (juce::jmax (180, (int) (r.getHeight() * 0.42f)))); piano->setBounds (r); }
            else if (rackHere) rack->setBounds (r);
            else if (pianoHere) piano->setBounds (r);
        }
        else if (view == 1) { if (here (playlist)) playlist->setBounds (r); }
        else if (here (mixer)) mixer->setBounds (r);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        const int cmd = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier, alt = juce::ModifierKeys::altModifier;
        auto is = [&k] (int key, int mods) { return k == juce::KeyPress (key, mods, 0); };
        if (is (juce::KeyPress::spaceKey, 0)) { engine.togglePlay(); return true; }
        if (isTypingNoteKey (k))
        {
            const int key = 48 + typingKeyOffset (k.getKeyCode());
            if (heldTyping.count (k.getKeyCode()) == 0) { heldTyping[k.getKeyCode()] = key; liveNote (key, 0.8f, true); }
            return true;
        }
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

    bool keyStateChanged (bool) override
    {
        for (auto it = heldTyping.begin(); it != heldTyping.end();)
        {
            if (! juce::KeyPress::isKeyCurrentlyDown (it->first)) { liveNote (it->second, 0.0f, false); it = heldTyping.erase (it); }
            else ++it;
        }
        return false;
    }

    // ---- live playing: MIDI keyboards + typing keyboard play the selected channel; recorded into the pattern while recording
    struct LiveMidi : juce::MidiInputCallback
    {
        std::function<void (int, float, bool)> fn;
        void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
        {
            if (! (m.isNoteOn() || m.isNoteOff())) return;
            const int key = m.getNoteNumber(); const float vel = m.getFloatVelocity(); const bool on = m.isNoteOn();
            juce::MessageManager::callAsync ([f = fn, key, vel, on] { if (f) f (key, vel, on); });
        }
    };
    LiveMidi liveMidi;
    std::map<int, int> heldTyping;
    std::map<int, std::pair<double, float>> heldNotes;

    void enableMidiInputs()
    {
        for (auto& d : juce::MidiInput::getAvailableDevices())
            if (! engine.deviceManager.isMidiInputDeviceEnabled (d.identifier))
                engine.deviceManager.setMidiInputDeviceEnabled (d.identifier, true);
    }

    void liveNote (int key, float vel, bool on)
    {
        int ch = studio.selectedChannel;
        if (studio.channel (ch) == nullptr && ! studio.p().channels.empty()) ch = studio.p().channels.front().id;
        if (studio.channel (ch) == nullptr) return;
        engine.previewNote (ch, key, vel, on);
        if (on)
        {
            heldNotes[key] = { engine.isPlaying() && engine.isRecording() ? engine.positionBeats() : -1.0, vel };
            return;
        }
        auto it = heldNotes.find (key);
        if (it == heldNotes.end()) return;
        const double start = it->second.first; const float velocity = it->second.second;
        heldNotes.erase (it);
        if (start < 0.0 || ! engine.isPlaying()) return;
        const double end = engine.positionBeats();
        studio.edit ("Record notes", [=] (Project& p) {
            if (p.patterns.empty()) return;
            auto& pat = p.patterns[(size_t) juce::jlimit (0, (int) p.patterns.size() - 1, p.currentPattern)];
            const double len = juce::jmax (1.0, pat.lengthBeats());
            double s0 = std::fmod (start, len), e0 = s0 + juce::jmax (0.0, end - start);
            Note n; n.key = key; n.velocity = velocity;
            n.start = std::round (s0 * 16.0) / 16.0;
            n.length = juce::jmax (1.0 / 16.0, e0 - n.start);
            pat.notes[ch].push_back (n);
        });
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
            m.addSeparator();
            add (mDetachRack, "Detach Channel Rack (own window)", {}, true, popouts.count (rack.get()) > 0);
            add (mDetachPiano, "Detach Piano Roll (own window)", {}, true, popouts.count (piano.get()) > 0);
            add (mDetachPlaylist, "Detach Playlist (own window)", {}, true, popouts.count (playlist.get()) > 0);
            add (mDetachMixer, "Detach Mixer (own window)", {}, true, popouts.count (mixer.get()) > 0);
            add (mDetachBrowser, "Detach Plugin House / Browser (own window)", {}, true, popouts.count (browser.get()) > 0);
        }
        else if (index == 3)
        {
            add (mAudio, "Audio settings (device, buffer size, sample rate)...");
            add (mPlugins, "Plugin House: scan / manage plugins...");
            add (mAutoBackup, "Auto-backup (every 8 edits + every 2 minutes)", {}, true, autoBackup());
            add (mTypingKeys, "Typing keyboard to piano (play live: Z-M / Q-I rows)", {}, true, typingKeyboard());
            add (mMidiInputs, "Connect MIDI keyboards");
            m.addSeparator();
            add (mTheme, "Theme color wheel...");
            {
                juce::PopupMenu sc;
                const float cur = juce::Desktop::getInstance().getGlobalScaleFactor();
                const int pcts[] { 75, 90, 100, 110, 125, 150, 175, 200 };
                for (int i = 0; i < 8; ++i) sc.addItem (200 + i, juce::String (pcts[i]) + "%", true, std::abs (cur * 100.0f - (float) pcts[i]) < 1.0f);
                m.addSubMenu ("UI scale", sc);
                juce::PopupMenu fm;
                const char* fonts[] { "Tattoo (classic lettering)", "Flash (old-school sign)", "Clean" };
                for (int i = 0; i < 3; ++i) fm.addItem (250 + i, fonts[i], true, fontStyle() == i);
                m.addSubMenu ("Font", fm);
            }
            {
                juce::PopupMenu pm;
                const int hw = juce::jmax (1, (int) std::thread::hardware_concurrency());
                pm.addSectionHeader ("Processing threads (" + juce::String (engine.processingThreads()) + " in use, " + juce::String (hw) + " cores)");
                const int opts[] { 0, 1, 2, 4, 6, 8, 12, 16 };
                for (int i = 0; i < 8; ++i)
                    pm.addItem (460 + i, opts[i] == 0 ? juce::String ("Auto (all cores)") : juce::String (opts[i]) + (opts[i] == 1 ? " (no multithreading)" : ""), opts[i] <= 1 || opts[i] <= hw * 2, perf().threads == opts[i]);
                pm.addSeparator();
                pm.addItem (470, "Multithreaded mixer + instruments (spread tracks over cores)", true, perf().mt);
                pm.addItem (471, "Smart disable (silent plugins / instruments sleep)", true, perf().smart);
                m.addSubMenu ("Performance", pm);
               #if JUCE_WINDOWS
                m.addItem (480, "OBS / Discord capture-friendly drawing (untick = faster GPU drawing)", true, captureFriendlyDrawing());
               #endif
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
        if (id >= 250 && id < 253) { setFontStyle (id - 250); return; }
        if (id == 480)
        {
            captureFriendlyDrawing() = ! captureFriendlyDrawing();
            appDataFile ("capture.txt").getParentDirectory().createDirectory();
            appDataFile ("capture.txt").replaceWithText (captureFriendlyDrawing() ? "1" : "0");
            auto& d = juce::Desktop::getInstance();
            for (int i = 0; i < d.getNumComponents(); ++i)
                if (auto* c = d.getComponent (i); c != nullptr && c->getPeer() != nullptr)
                {
                    auto* peer = c->getPeer();
                    const int k = peer->getAvailableRenderingEngines().indexOf (captureFriendlyDrawing() ? "Software Renderer" : "Direct2D");
                    if (k >= 0) peer->setCurrentRenderingEngine (k);
                }
            return;
        }
        if (id >= 460 && id <= 471)
        {
            const int opts[] { 0, 1, 2, 4, 6, 8, 12, 16 };
            if (id < 468) perf().threads = opts[id - 460];
            else if (id == 470) perf().mt = ! perf().mt;
            else if (id == 471) perf().smart = ! perf().smart;
            savePerf();
            engine.setPerformance (perf().threads, perf().mt, perf().smart);
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
            case mDetachRack: toggleDetach (rack.get(), "Channel Rack"); break;
            case mDetachPiano: toggleDetach (piano.get(), "Piano Roll"); break;
            case mDetachPlaylist: toggleDetach (playlist.get(), "Playlist"); break;
            case mDetachMixer: toggleDetach (mixer.get(), "Mixer"); break;
            case mDetachBrowser: toggleDetach (browser.get(), "Plugin House"); break;
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
            case mTypingKeys: typingKeyboard() = ! typingKeyboard(); studio.settings->setValue ("typingKeys", typingKeyboard()); break;
            case mMidiInputs: enableMidiInputs(); break;
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
    // a click anywhere stops a sample preview that is already playing (a click that starts a new one keeps it)
    struct PreviewStopper : juce::MouseListener
    {
        explicit PreviewStopper (Engine& en) : e (en) {}
        void mouseDown (const juce::MouseEvent&) override { e.stopPreview (150); }
        Engine& e;
    } previewStopper { engine };
    std::unique_ptr<juce::Component> rack, piano, playlist, mixer, browser;
    juce::Label titleLabel, posLabel, cpuLabel, hintBar;
    juce::TextButton beatTab { "BEAT" }, recordTab { "RECORD" }, mixTab { "MIX" };
    TransportButton playBtn, stopBtn, recBtn;
    juce::TextButton songBtn, metroBtn, countBtn, browserBtn, themeBtn, scopeBtn;
    BpmSlider bpm;
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

    // FL-style detached windows: any panel can pop out into its own resizable window (multi-monitor)
    struct PopOut : public juce::DocumentWindow
    {
        PopOut (const juce::String& t) : juce::DocumentWindow ("HumHouse Studio - " + t, col::bg, juce::DocumentWindow::allButtons) {}
        std::function<void()> onClose;
        void closeButtonPressed() override { if (onClose) onClose(); }
    };
    std::map<juce::Component*, std::unique_ptr<PopOut>> popouts;

    void toggleDetach (juce::Component* c, const juce::String& title)
    {
        if (popouts.count (c) > 0) { reattach (c); return; }
        auto w = std::make_unique<PopOut> (title);
        w->setUsingNativeTitleBar (true);
        w->setContentNonOwned (c, false);
        c->setVisible (true);
        w->setResizable (true, true);
        w->setResizeLimits (360, 220, 10000, 8000);
        w->centreWithSize (juce::jmax (900, c->getWidth()), juce::jmax (520, c->getHeight()));
        w->setVisible (true);
        makeCaptureFriendly (*w);
        w->onClose = [safe = juce::Component::SafePointer<MainComponent> (this), c]
        {
            juce::MessageManager::callAsync ([safe, c] { if (safe != nullptr) safe->reattach (c); });
        };
        popouts[c] = std::move (w);
        resized();
    }
    void reattach (juce::Component* c)
    {
        auto it = popouts.find (c);
        if (it == popouts.end()) return;
        it->second->clearContentComponent();
        popouts.erase (it);
        addAndMakeVisible (c);
        resized();
    }

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

        if (++ticks >= 30 * 120)
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
        studio.selectedTrack = 1; studio.selectedLanes.clear(); studio.laneAnchor = -1;
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
        studio.selectedTrack = 1; studio.selectedLanes.clear(); studio.laneAnchor = -1;
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
