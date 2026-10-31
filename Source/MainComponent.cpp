#include <set>
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
int& fontStyle() { static int st = appDataFile ("font-v2.txt").existsAsFile() ? appDataFile ("font-v2.txt").loadFileAsString().getIntValue() : 3; return st; }
void setFontStyle (int st)
{
    fontStyle() = st;
    appDataFile ("font-v2.txt").getParentDirectory().createDirectory();
    appDataFile ("font-v2.txt").replaceWithText (juce::String (st));
    juce::Typeface::clearTypefaceCache();
    for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
        if (auto* w = juce::TopLevelWindow::getTopLevelWindow (i)) { w->sendLookAndFeelChange(); w->repaint(); }
}
// combo boxes marked "smallArrow" (Playlist track routing) get a slim arrow so the name gets the room
void HHLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox& box)
{
    if (! box.getProperties().contains ("smallArrow")) { LookAndFeel_V4::drawComboBox (g, w, h, down, bx, by, bw, bh, box); return; }
    const auto b = juce::Rectangle<int> (0, 0, w, h).toFloat();
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId)); g.fillRoundedRectangle (b, 2.0f);
    g.setColour (box.findColour (juce::ComboBox::outlineColourId)); g.drawRoundedRectangle (b.reduced (0.5f), 2.0f, 1.0f);
    const float cx = (float) w - 7.0f, cy = (float) h * 0.5f;
    juce::Path a; a.startNewSubPath (cx - 2.5f, cy - 1.2f); a.lineTo (cx, cy + 1.3f); a.lineTo (cx + 2.5f, cy - 1.2f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withAlpha (box.isEnabled() ? 0.8f : 0.2f));
    g.strokePath (a, juce::PathStrokeType (1.2f));
}
void HHLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    if (! box.getProperties().contains ("smallArrow")) { LookAndFeel_V4::positionComboBoxText (box, label); return; }
    label.setBounds (1, 1, box.getWidth() - 13, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}
juce::Typeface::Ptr HHLookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    static auto tattoo = juce::Typeface::createSystemTypefaceFor (BinaryData::PirataOneRegular_ttf, (size_t) BinaryData::PirataOneRegular_ttfSize);
    static auto flash = juce::Typeface::createSystemTypefaceFor (BinaryData::RyeRegular_ttf, (size_t) BinaryData::RyeRegular_ttfSize);
    static auto pro = juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowRegular_ttf, (size_t) BinaryData::BarlowRegular_ttfSize);
    static auto proBold = juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowSemiBold_ttf, (size_t) BinaryData::BarlowSemiBold_ttfSize);
    if (fontStyle() == 3 && pro != nullptr) return f.isBold() && proBold != nullptr ? proBold : pro;
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
        "- Samples play exactly like the WAV: clean sample-rate conversion, no added fades; drums/FX get KEY off so every note is the sample as-is\n"
        "- Double-click a channel: wave, Amp / INS and sample settings on one MAIN page\n"
        "- Drag the playhead on the bar numbers in Piano Roll, Playlist and Record\n"
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
    const auto onAccent = col::accent.getPerceivedBrightness() > 0.5f ? juce::Colours::black : juce::Colours::white;
    setColourScheme ({ col::bg, col::panel2, col::panel, col::gridBar, col::text, col::accent,
                       juce::Colours::black, col::accent, col::text });
    setColour (juce::TextButton::buttonColourId, col::panel2);
    setColour (juce::TextButton::buttonOnColourId, col::accent);
    setColour (juce::TextButton::textColourOffId, col::text);
    setColour (juce::TextButton::textColourOnId, onAccent);
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
    setColour (juce::PopupMenu::highlightedTextColourId, onAccent);
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

void HHLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool over, bool down)
{
    const auto font = getTextButtonFont (b, b.getHeight());
    juce::ignoreUnused (over, down);
    g.setFont (font);
    const int yIndent = juce::jmin (4, b.proportionOfHeight (0.3f)), cornerSize = juce::jmin (b.getHeight(), b.getWidth()) / 2;
    const int fontHeight = juce::roundToInt (font.getHeight() * 0.6f);
    const int leftIndent = juce::jmin (fontHeight, 2 + cornerSize / (b.isConnectedOnLeft() ? 4 : 2));
    const int rightIndent = juce::jmin (fontHeight, 2 + cornerSize / (b.isConnectedOnRight() ? 4 : 2));
    const juce::Rectangle<int> area (leftIndent, yIndent, b.getWidth() - leftIndent - rightIndent, b.getHeight() - yIndent * 2);
    if (area.getWidth() <= 0) return;
    if (! col::fontGradient || b.getToggleState() || ! col::isHeadline (font) || b.findColour (juce::TextButton::textColourOffId) != col::text)
    {
        // plain text: never the same color as the button it sits on (custom on-colors, user font color, theme accents)
        const auto bg = b.findColour (b.getToggleState() ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId);
        const auto txt = b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId);
        g.setColour (col::readableOn (txt, bg).withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.5f));
        g.drawFittedText (b.getButtonText(), area, juce::Justification::centred, 2);
        return;
    }
    col::drawWithHalo (g, b.isEnabled() ? 1.0f : 0.5f, [&] (int dx, int dy) { g.drawFittedText (b.getButtonText(), area.translated (dx, dy), juce::Justification::centred, 2); });
    col::fillGradientText (g, b.getButtonText(), area.toFloat(), juce::Justification::centred, 2, 0.0f, b.isEnabled() ? 1.0f : 0.5f);
}

void HHLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& l)
{
    const auto font = getLabelFont (l);
    if (! col::fontGradient || l.isBeingEdited() || ! col::isHeadline (font) || l.findColour (juce::Label::textColourId) != col::text)
    {
        const auto bg = l.findColour (juce::Label::backgroundColourId);
        const auto txt = l.findColour (juce::Label::textColourId);
        const auto fixedTxt = col::readableOn (txt, bg.getAlpha() >= 40 ? bg : col::panel);
        if (l.isBeingEdited() || fixedTxt == txt) return LookAndFeel_V4::drawLabel (g, l);
        g.fillAll (bg);
        g.setFont (font);
        g.setColour (fixedTxt.withMultipliedAlpha (l.isEnabled() ? 1.0f : 0.5f));
        const auto area = getLabelBorderSize (l).subtractedFrom (l.getLocalBounds());
        g.drawFittedText (l.getText(), area, l.getJustificationType(), juce::jmax (1, (int) ((float) area.getHeight() / font.getHeight())), l.getMinimumHorizontalScale());
        return;
    }
    g.fillAll (l.findColour (juce::Label::backgroundColourId));
    g.setFont (font);
    const auto area = getLabelBorderSize (l).subtractedFrom (l.getLocalBounds());
    const float alpha = l.isEnabled() ? 1.0f : 0.5f;
    const int lines = juce::jmax (1, (int) ((float) area.getHeight() / font.getHeight()));
    col::drawWithHalo (g, alpha, [&] (int dx, int dy) { g.drawFittedText (l.getText(), area.translated (dx, dy), l.getJustificationType(), lines, l.getMinimumHorizontalScale()); });
    col::fillGradientText (g, l.getText(), area.toFloat(), l.getJustificationType(), lines, l.getMinimumHorizontalScale(), alpha);
    g.setColour (l.findColour (juce::Label::outlineColourId).withMultipliedAlpha (alpha));
    g.drawRect (l.getLocalBounds());
}

void HHLookAndFeel::drawFileBrowserRow (juce::Graphics& g, int w, int h, const juce::File& file, const juce::String& filename, juce::Image* icon,
                                        const juce::String& size, const juce::String& time, bool isDir, bool selected, int index,
                                        juce::DirectoryContentsDisplayComponent& c)
{
    const auto shown = isDir || ! filename.containsChar ('.') ? filename : filename.upToLastOccurrenceOf (".", false, false);
    LookAndFeel_V4::drawFileBrowserRow (g, w, h, file, shown, icon, size, time, isDir, selected, index, c);
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
// auto-backups write the .hhs on a background thread: serialising + disk writes on the message thread froze the
// waveforms / meters for a moment every few edits while the song played
static void writeBackupAsync (const Project& p)
{
    static juce::ThreadPool pool (1);
    if (pool.getNumJobs() > 1) return;   // one queued is enough
    pool.addJob ([copy = std::make_shared<Project> (p)] { writeBackup (*copy); });
}

void Studio::commit (const juce::String& what)
{
    undo.push (p(), what);
    engine.sync();
    dirty = true;
    if (++editsSinceBackup >= 8) { editsSinceBackup = 0; engine.storePluginStates(); writeBackupAsync (p()); }   // auto-backup every few edits
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
    int kind = 0;   // 0 play/pause, 1 stop, 2 record, 3 back to start, 4 mixer (3 fader lines)
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        getLookAndFeel().drawButtonBackground (g, *this, findColour (getToggleState() ? buttonOnColourId : buttonColourId), over, down);
        auto r = getLocalBounds().toFloat().withSizeKeepingCentre (14.0f, 14.0f);
        const bool on = getToggleState();
        g.setColour (kind == 2 ? (on ? juce::Colours::white : col::rec) : (on ? juce::Colours::black : col::text));
        if (kind == 0 && on) { g.fillRect (r.removeFromLeft (5.0f)); r.removeFromLeft (4.0f); g.fillRect (r.removeFromLeft (5.0f)); }
        else if (kind == 0) { juce::Path p; p.addTriangle (r.getX() + 1, r.getY(), r.getX() + 1, r.getBottom(), r.getRight(), r.getCentreY()); g.fillPath (p); }
        else if (kind == 1) g.fillRect (r.reduced (1.0f));
        else if (kind == 3) { r = r.reduced (1.0f); g.fillRect (r.removeFromLeft (2.5f)); juce::Path p; p.addTriangle (r.getRight(), r.getY(), r.getRight(), r.getBottom(), r.getX() + 0.5f, r.getCentreY()); g.fillPath (p); }
        else if (kind == 4)   // the HumHouse logo waveform (4 bars)
        {
            static const float bh[] { 0.5f, 1.0f, 0.71f, 0.33f };
            const float step = r.getWidth() / 4.0f, w = juce::jmax (2.0f, step * 0.62f);
            for (int k = 0; k < 4; ++k)
            {
                const float h = r.getHeight() * bh[k];
                g.fillRoundedRectangle (r.getX() + step * (float) k + (step - w) * 0.5f, r.getCentreY() - h * 0.5f, w, h, w * 0.5f);
            }
        }
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
    mMidiDevice = 90,
    mHistory = 1000
};

struct RenderJob : public juce::ThreadWithProgressWindow
{
    struct Item { juce::File file; int track; };
    RenderJob (Engine& e, std::vector<Item> it, bool song, bool stemMode)
        : juce::ThreadWithProgressWindow ("Exporting " + (it.size() == 1 ? it.front().file.getFileName() : juce::String ((int) it.size()) + " stems") + " ...", true, true),
          engine (e), items (std::move (it)), songMode (song), stems (stemMode) {}
    void run() override
    {
        const float n = (float) items.size();
        for (size_t i = 0; i < items.size() && ! threadShouldExit(); ++i)
        {
            auto& it = items[i];
            setStatusMessage (it.file.getFileName());
            const bool ok = engine.renderToWav (it.file, songMode, [this, i, n] (float p) { setProgress (((float) i + p) / n); return ! threadShouldExit(); },
                                                it.track, engine.exportBits, engine.exportRate, engine.exportFormat);
            if (! ok) { if (! threadShouldExit()) ++failed; continue; }
            if (stems && engine.lastRenderPeak < 1.0e-5f) { it.file.deleteFile(); ++skipped; }   // silent insert: no empty stem
            else { ++saved; last = it.file; }
        }
    }
    void threadComplete (bool cancelled) override
    {
        if (! cancelled)
        {
            juce::String msg;
            if (saved == 0) msg = engine.exportFormat == 2 ? "Export failed (M4A needs the macOS / Windows AAC encoder)." : "Export failed.";
            else if (! stems) msg = "Saved " + last.getFullPathName();
            else msg = "Saved " + juce::String (saved) + " stems (all start at bar 1) to\n" + last.getParentDirectory().getFullPathName()
                       + (skipped > 0 ? "\n" + juce::String (skipped) + " silent inserts skipped" : juce::String());
            if (failed > 0 && saved > 0) msg << "\n" << failed << " failed";
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Export", msg);
            if (saved > 0) last.revealToUser();
        }
        juce::MessageManager::callAsync ([this] { delete this; });
    }
    Engine& engine; std::vector<Item> items; bool songMode, stems; int saved = 0, skipped = 0, failed = 0; juce::File last;
};
}

class MainComponent : public juce::Component, public juce::DragAndDropContainer, public juce::FileDragAndDropTarget, public juce::MenuBarModel, private juce::Timer, private juce::ChangeListener
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
        col::lightMode = studio.settings->getBoolValue ("themeLight", false);
        col::fontColour = juce::Colour::fromString (studio.settings->getValue ("fontColour", "00000000"));
        if (! studio.settings->containsKey ("themeColour") && (col::lightMode || col::fontColour.getAlpha() > 0)) applyTheme (col::accent, 0.5f, false);
        if (studio.settings->containsKey ("themeColour"))
            applyTheme (juce::Colour::fromString (studio.settings->getValue ("themeColour")), (float) studio.settings->getDoubleValue ("themeTint", 0.5), studio.settings->getBoolValue ("themeBlack", false));
        if (studio.settings->containsKey ("gradColours")) applyGradient (studio.settings->getValue ("gradColours"));
        if (studio.settings->containsKey ("fontGrad")) applyFontGradient (studio.settings->getValue ("fontGrad"));
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
        addChildComponent (pianoSplit);
        pianoSplit.onDrag = [this] (int y) {
            if (splitTotal <= 0) return;
            pianoMin = y > splitTop + splitTotal - 70;
            if (! pianoMin) rackFrac = juce::jlimit (0.15f, 0.9f, (float) (y - splitTop) / (float) splitTotal);
            resized(); };
        pianoSplit.onToggle = [this] { pianoMin = ! pianoMin; resized(); };

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
        setupButton (startBtn, {}, "Back to the start (Home)"); startBtn.kind = 3;
        startBtn.onClick = [this] { engine.setPositionBeats (0.0); };
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
        setupButton (mixPopBtn, {}, "Mixer: pop it out into its own window so it stays visible on Beat and Rec (click again to dock it)", true); mixPopBtn.kind = 4;
        mixPopBtn.onClick = [this] { if ((popouts.count (mixer.get()) > 0) != mixPopBtn.getToggleState()) toggleDetach (mixer.get(), "Mixer"); mixPopBtn.setToggleState (popouts.count (mixer.get()) > 0, juce::dontSendNotification); };
        addAndMakeVisible (mixPopBtn);
        setupButton (rackPopBtn, "RACK", "Channel Rack: pop it out into its own window so you can edit samples and one-shots on Rec and in full screen (click again to dock it)", true);
        rackPopBtn.onClick = [this] { if ((popouts.count (rack.get()) > 0) != rackPopBtn.getToggleState()) toggleDetach (rack.get(), "Channel Rack"); rackPopBtn.setToggleState (popouts.count (rack.get()) > 0, juce::dontSendNotification); };
        addAndMakeVisible (rackPopBtn);
        rackPopOut = [this] { toggleDetach (rack.get(), "Channel Rack"); rackPopBtn.setToggleState (popouts.count (rack.get()) > 0, juce::dontSendNotification); rack->resized(); };
        setupButton (saveBtn, "SAVE", "Save the project (Cmd/Ctrl+S). Flashes SAVED when it's written", false);
        saveBtn.onClick = [this] { save (false); };
        addAndMakeVisible (saveBtn);
        for (juce::Button* b : { (juce::Button*) &songBtn, (juce::Button*) &startBtn, (juce::Button*) &playBtn, (juce::Button*) &stopBtn, (juce::Button*) &recBtn, (juce::Button*) &metroBtn, (juce::Button*) &countBtn, (juce::Button*) &browserBtn, (juce::Button*) &themeBtn }) addAndMakeVisible (b);

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

        hostKeyHandler = [this] (const juce::KeyPress& k) { return keyPressed (k); };
        if (! floatWithTopmost) juce::Desktop::getInstance().addGlobalMouseListener (&floatRaiser);
#if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (this);
#else
        if (! phoneUI) addAndMakeVisible (menuBar);
#endif
        if (phoneUI)
        {
            closeAllBtn.setTooltip ("Close everything that's open (windows, pop-outs, menus, Browser)");
            menuBtn.setTooltip ("File, Edit, View, Options (audio buffer etc.) and Help in one folder");
            importBtn.setTooltip ("Import audio, MIDI or a HumHouse file");
            closeAllBtn.onClick = [this] { closeEverything(); };
            menuBtn.onClick = [this] { showAllMenus(); };
            importBtn.onClick = [this] { importFiles(); };
            copyBtn.setTooltip ("Copy the selected clips / notes (lights up when copied)");
            pasteBtn.setTooltip ("Paste at the playhead");
            copyBtn.setClickingTogglesState (false);
            copyBtn.setColour (juce::TextButton::buttonOnColourId, col::accent);
            copyBtn.onClick = [this] { phoneCopyPaste ('c'); };
            pasteBtn.onClick = [this] { phoneCopyPaste ('v'); };
            for (auto* b : { &closeAllBtn, &menuBtn, &importBtn, &copyBtn, &pasteBtn }) addAndMakeVisible (b);
        }
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
        rackPopOut = nullptr;
        juce::Desktop::getInstance().removeGlobalMouseListener (&previewStopper);
        stopTimer();
        engine.deviceManager.removeMidiInputDeviceCallback ({}, &liveMidi);
        popouts.clear();
#if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (nullptr);
#endif
        hostKeyHandler = nullptr;
        juce::Desktop::getInstance().removeGlobalMouseListener (&floatRaiser);
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
if (phoneUI)
        {   // phone: the same controls, wrapped into as many rows as the width needs (portrait or landscape)
            const std::pair<juce::Component*, int> items[] {
                { &menuBtn, 64 }, { nullptr, 130 }, { &closeAllBtn, 44 }, { &startBtn, 40 }, { &playBtn, 40 }, { &stopBtn, 40 }, { &recBtn, 40 }, { &songBtn, 48 },
                { &beatTab, 72 }, { &recordTab, 72 }, { &mixTab, 72 }, { &importBtn, 72 }, { &copyBtn, 76 }, { &pasteBtn, 60 },
                { &bpm, 110 }, { &posLabel, 110 },
                { &metroBtn, 62 }, { &countBtn, 80 }, { &rackPopBtn, 52 }, { &mixPopBtn, 40 }, { &scopeBtn, 64 }, { &themeBtn, 64 },
                { &browserBtn, 120 }, { &saveBtn, 70 }, { &cpuLabel, 86 } };
            const int rowH = 40, avail = juce::jmax (200, r.getWidth() - 16);
            int x = 0, rows = 1;
            for (auto& it : items) { if (x > 0 && x + it.second > avail) { ++rows; x = 0; } x += it.second + 4; }
            topBar = r.removeFromTop (rows * rowH + 8);
            logoArea = {};
            int y = topBar.getY() + 4; x = 0;
            for (auto& it : items)
            {
                if (x > 0 && x + it.second > avail) { y += rowH; x = 0; }
                const juce::Rectangle<int> cell (topBar.getX() + 8 + x, y + 2, it.second, rowH - 4);
                if (it.first != nullptr) it.first->setBounds (cell); else logoArea = cell;
                x += it.second + 4;
            }
            mixPopBtn.setToggleState (popouts.count (mixer.get()) > 0, juce::dontSendNotification);
            rackPopBtn.setToggleState (popouts.count (rack.get()) > 0, juce::dontSendNotification);
        }
        else
        {
#if ! JUCE_MAC
        menuBar.setBounds (r.removeFromTop (22));
#endif
        topBar = r.removeFromTop (48);
        auto t = topBar.reduced (8, 8);
        logoArea = t.removeFromLeft (150);
        for (auto* b : { &beatTab, &recordTab, &mixTab }) { b->setBounds (t.removeFromLeft (84)); t.removeFromLeft (4); }
        t.removeFromLeft (16);
        songBtn.setBounds (t.removeFromLeft (48)); t.removeFromLeft (4);
        for (auto* b : { &startBtn, &playBtn, &stopBtn, &recBtn }) { b->setBounds (t.removeFromLeft (40)); t.removeFromLeft (4); }
        t.removeFromLeft (8);
        bpm.setBounds (t.removeFromLeft (110)); t.removeFromLeft (6);
        posLabel.setBounds (t.removeFromLeft (110)); t.removeFromLeft (6);
        metroBtn.setBounds (t.removeFromLeft (62)); t.removeFromLeft (4);
        countBtn.setBounds (t.removeFromLeft (80)); t.removeFromLeft (8);
        saveBtn.setBounds (t.removeFromRight (70)); t.removeFromRight (6);
        browserBtn.setBounds (t.removeFromRight (120)); t.removeFromRight (8);
        themeBtn.setBounds (t.removeFromRight (64)); t.removeFromRight (6);
        scopeBtn.setBounds (t.removeFromRight (64)); t.removeFromRight (6);
        mixPopBtn.setToggleState (popouts.count (mixer.get()) > 0, juce::dontSendNotification);
        mixPopBtn.setBounds (t.removeFromRight (40)); t.removeFromRight (6);
        rackPopBtn.setToggleState (popouts.count (rack.get()) > 0, juce::dontSendNotification);
        rackPopBtn.setBounds (t.removeFromRight (52)); t.removeFromRight (6);
        cpuLabel.setBounds (t.removeFromRight (86));
        }
        hintBar.setBounds (r.removeFromBottom (22));
        auto here = [this] (const std::unique_ptr<juce::Component>& c) { return popouts.count (c.get()) == 0; };
        if (here (browser)) { browser->setVisible (showBrowser); if (showBrowser) browser->setBounds (r.removeFromLeft (phoneUI ? juce::jmin (260, r.getWidth() * 36 / 100) : 260)); }
        const bool rackHere = here (rack), pianoHere = here (piano);
        if (rackHere) rack->setVisible (view == 0);
        if (pianoHere) piano->setVisible (view == 0);
        if (here (playlist)) playlist->setVisible (view == 1);
        if (here (mixer)) mixer->setVisible (view == 2);
        pianoSplit.setVisible (false);
        if (view == 0)
        {
            if (rackHere && pianoHere)
            {
                splitTop = r.getY(); splitTotal = r.getHeight();
                const int rh = pianoMin ? splitTotal - 22 : juce::jlimit (80, juce::jmax (80, splitTotal - 140), (int) (splitTotal * rackFrac));
                rack->setBounds (r.removeFromTop (rh));
                pianoSplit.minimized = pianoMin; pianoSplit.setVisible (true); pianoSplit.setBounds (r.removeFromTop (22)); pianoSplit.repaint();
                piano->setVisible (! pianoMin); piano->setBounds (r);
            }
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
        if (is (juce::KeyPress::homeKey, 0)) { engine.setPositionBeats (0.0); return true; }
        if (is ('u', cmd)) { showShortcuts(); return true; }   // Cmd/Ctrl+U: Shortcuts window on / off
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

    juce::Array<juce::MidiDeviceInfo> midiDevs;
    int midiSeen = -1;
    // hot-plug from the OS device-list notification: polling the device list every 2 s stalled the screen on Windows
    juce::MidiDeviceListConnection midiConn = juce::MidiDeviceListConnection::make ([this] { enableMidiInputs(); });
    // the chosen device (or every connected one when none is chosen) plays live; a keyboard plugged in later just works
    void enableMidiInputs()
    {
        const auto pick = studio.settings->getValue ("midiIn");
        const auto all = juce::MidiInput::getAvailableDevices();
        bool found = false;
        for (auto& d : all) found = found || d.identifier == pick;
        for (auto& d : all)
        {
            const bool want = pick.isEmpty() || ! found || d.identifier == pick;
            if (engine.deviceManager.isMidiInputDeviceEnabled (d.identifier) != want)
                engine.deviceManager.setMidiInputDeviceEnabled (d.identifier, want);
        }
        midiSeen = all.size();
    }

    void liveNote (int key, float vel, bool on)
    {
        std::vector<int> chans;
        for (auto& c : studio.p().channels) if (c.midiLink) chans.push_back (c.id);
        if (chans.empty())
        {
            int ch = studio.selectedChannel;
            if (studio.channel (ch) == nullptr && ! studio.p().channels.empty()) ch = studio.p().channels.front().id;
            if (studio.channel (ch) == nullptr) return;
            chans.push_back (ch);
        }
        for (int ch : chans) engine.previewNote (ch, key, vel, on);
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
            for (int ch : chans) pat.notes[ch].push_back (n);
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
                for (int i = 0; i < ts.size() && i < 90; ++i)
                    tm.addItem (300 + i, ts[i].getFileNameWithoutExtension() + (ts[i].isAChildOf (templatesDir()) ? juce::String() : "   (" + ts[i].getParentDirectory().getFileName() + ")"));
                if (ts.isEmpty()) tm.addItem (-1, "(no templates yet)", false);
                tm.addSeparator();
                tm.addItem (396, "Add a templates folder (e.g. Desktop)...");
                tm.addItem (397, "Show templates folder");
                tm.addSeparator();
                tm.addItem (398, "Share this project as a template file (sounds inside)...");
                if (! ts.isEmpty())
                {
                    juce::PopupMenu share;
                    for (int i = 0; i < ts.size() && i < 90; ++i) share.addItem (700 + i, ts[i].getFileNameWithoutExtension());
                    tm.addSubMenu ("Share a saved template as a file", share);
                }
                tm.addItem (399, "Import a template / preset / FX chain file someone sent you...");
                m.addSubMenu ("New from template", tm);
            }
            add (mSaveTemplate, "Save as template...");
            if (lastTemplate.isNotEmpty()) m.addItem (395, "Update template \"" + lastTemplate + "\" (overwrite)");
            add (mDefaultTemplate, "Use this project as default template");
            add (mClearDefaultTemplate, "Reset default template", {}, defaultTemplate().existsAsFile());
            m.addSeparator();
            add (mExport, "Export (WAV / MP3 / M4A / stems)...", "Ctrl+R"); add (mExportTrack, "Export selected track / stems...");
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
            {
                juce::PopupMenu md;   // pick the keyboard / piano: it plays the selected channel at once, no other setup
                midiDevs = juce::MidiInput::getAvailableDevices();
                const auto pick = studio.settings->getValue ("midiIn");
                md.addItem (mMidiInputs, "All connected (auto, plug and play)", true, pick.isEmpty());
                for (int i = 0; i < juce::jmin (10, midiDevs.size()); ++i)
                    md.addItem (mMidiDevice + i, midiDevs[i].name, true, pick == midiDevs[i].identifier);
                if (midiDevs.isEmpty()) md.addItem (-1, "(no MIDI device connected)", false);
                m.addSubMenu ("MIDI input device", md);
            }
            m.addSeparator();
            add (mTheme, "Theme color wheel...");
            {
                juce::PopupMenu sc;
                const float cur = juce::Desktop::getInstance().getGlobalScaleFactor();
                const int pcts[] { 75, 90, 100, 110, 125, 150, 175, 200 };
                for (int i = 0; i < 8; ++i) sc.addItem (200 + i, juce::String (pcts[i]) + "%", true, std::abs (cur * 100.0f - (float) pcts[i]) < 1.0f);
                m.addSubMenu ("UI scale", sc);
                juce::PopupMenu fm;
                const char* fonts[] { "Tattoo (classic lettering)", "Flash (old-school sign)", "System", "Pro (sleek, default)" };
                for (int i = 0; i < 4; ++i) fm.addItem (250 + i, fonts[i], true, fontStyle() == i);
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
            m.addSeparator();
            add (mShortcuts, "Shortcuts", "Cmd/Ctrl+U", true, shortcutsWin != nullptr && shortcutsWin->isVisible());
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
        if (id >= mMidiDevice && id < mMidiDevice + 10 && id - mMidiDevice < midiDevs.size())
        {
            const auto& d = midiDevs[id - mMidiDevice];
            studio.settings->setValue ("midiIn", d.identifier);
            enableMidiInputs();
            flash ("MIDI: " + d.name);
            return;
        }
        if (id == 395 && lastTemplate.isNotEmpty()) { writeTemplate (lastTemplate); return; }
        if (id == 396) { addTemplateFolder(); return; }
        if (id == 397) { templatesDir().createDirectory(); templatesDir().startAsProcess(); return; }
        if (id == 398) { shareProjectAsTemplate(); return; }
        if (id == 399) { importSharedFile(); return; }
        if (id >= 700 && id < 790)
        {
            auto ts = templateFiles();
            Project q;
            if (id - 700 < ts.size() && loadProject (q, ts[id - 700])) shareTemplatePack (q, ts[id - 700].getFileNameWithoutExtension());
            return;
        }
        if (id >= 300 && id < 390)
        {
            auto ts = templateFiles();
            if (id - 300 < ts.size()) newProject (ts[id - 300]);
            return;
        }
        if (id >= 250 && id < 254) { setFontStyle (id - 250); return; }
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
            case mMidiInputs: studio.settings->setValue ("midiIn", juce::String()); enableMidiInputs(); flash ("MIDI: all connected keyboards"); break;
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
    juce::TooltipWindow tooltips { this, 1000 };   // hover anything for 1 second = help
    // a click anywhere stops a sample preview that is already playing (a click that starts a new one keeps it)
    struct PreviewStopper : juce::MouseListener
    {
        explicit PreviewStopper (Engine& en) : e (en) {}
        void mouseDown (const juce::MouseEvent&) override { e.stopPreview (150); }
        Engine& e;
    } previewStopper { engine };
    std::unique_ptr<juce::Component> rack, piano, playlist, mixer, browser;
    struct SplitBar : juce::Component   // drag to resize Channel Rack vs Piano Roll; click to minimize / show the Piano Roll
    {
        SplitBar() { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); }
        std::function<void (int)> onDrag; std::function<void()> onToggle; bool minimized = false; int startY = 0;
        void paint (juce::Graphics& g) override
        {
            g.fillAll (col::lightMode ? col::panel2 : juce::Colour (0xff15181d));
            g.setColour (juce::Colour (0xffff8a4c).withAlpha (0.8f)); g.fillRoundedRectangle (getLocalBounds().toFloat().withSizeKeepingCentre (40, 4).withX (8), 2);
            g.setColour (col::text.withAlpha (0.6f)); g.setFont (12.0f);
            g.drawText (minimized ? "PIANO ROLL  [+] click to show" : "PIANO ROLL  [-] click to minimize  |  drag to resize",
                        getLocalBounds(), juce::Justification::centred);
        }
        void mouseDown (const juce::MouseEvent&) override { startY = getY(); }
        void mouseDrag (const juce::MouseEvent& e) override { if (onDrag && std::abs (e.getDistanceFromDragStartY()) > 2) onDrag (startY + e.getDistanceFromDragStartY()); }
        void mouseUp (const juce::MouseEvent& e) override { if (! e.mouseWasDraggedSinceMouseDown() && onToggle) onToggle(); }
    } pianoSplit;
    float rackFrac = 0.55f; bool pianoMin = false; int splitTop = 0, splitTotal = 0;
    juce::Label titleLabel, posLabel, cpuLabel, hintBar;
    juce::TextButton beatTab { "BEAT" }, recordTab { "RECORD" }, mixTab { "MIX" };
    TransportButton startBtn, playBtn, stopBtn, recBtn, mixPopBtn;
    juce::TextButton rackPopBtn;
    juce::TextButton songBtn, metroBtn, countBtn, browserBtn, themeBtn, scopeBtn, saveBtn;
    int savedFlash = 0;
    struct FloatRaiser : public juce::MouseListener   // macOS: clicking the main window keeps pop-outs / plug-ins above it
    {
        void mouseUp (const juce::MouseEvent& e) override
        {
            auto* top = e.eventComponent != nullptr ? e.eventComponent->getTopLevelComponent() : nullptr;
            if (top != nullptr && ! isFloating (top) && juce::Process::isForegroundProcess()) raiseFloatingWindows();
        }
    } floatRaiser;
    BpmSlider bpm;
    juce::MenuBarComponent menuBar { this };
    juce::Rectangle<int> topBar, logoArea;
    std::unique_ptr<juce::FileChooser> chooser, importChooser;
    juce::TextButton closeAllBtn { "X" }, menuBtn { "MENU" }, importBtn { "+ IMPORT" }, copyBtn { "COPY" }, pasteBtn { "PASTE" };
    void phoneCopyPaste (int key)
    {
        juce::Component* target = view == 1 ? playlist.get() : view == 0 ? piano.get() : nullptr;
        if (target == nullptr) return;
        int n = 0;
        if (view == 1) for (auto& c : studio.p().clips) n += c.selected ? 1 : 0;
        if (key == 'c' && view == 1 && n == 0) { hintBar.setText ("Nothing selected: tap clips or patterns first, then COPY", juce::dontSendNotification); return; }
        target->keyPressed (juce::KeyPress (key, juce::ModifierKeys::commandModifier, 0));
        if (key == 'c')
        {
            copyBtn.setToggleState (true, juce::dontSendNotification);
            copyBtn.setButtonText (view == 1 ? "COPIED " + juce::String (n) : "COPIED");
            hintBar.setText (view == 1 ? "Copied " + juce::String (n) + (n == 1 ? " clip" : " clips") + " - PASTE puts them at the playhead"
                                       : "Copied the selected notes - PASTE puts them at the playhead", juce::dontSendNotification);
        }
        else hintBar.setText ("Pasted at the playhead", juce::dontSendNotification);
    }
    void showAllMenus()
    {
        juce::PopupMenu m;
        const auto names = getMenuBarNames();
        for (int i = 0; i < names.size(); ++i) m.addSubMenu (names[i], getMenuForIndex (i, names[i]));
        juce::Component::SafePointer<MainComponent> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuBtn).withStandardItemHeight (36),
                         [safe] (int r) { if (safe != nullptr && r != 0) safe->menuItemSelected (r, 0); });
    }
    void importFiles()
    {
        importChooser = std::make_unique<juce::FileChooser> ("Import any file (Downloads, iCloud, On My iPhone, connected drives)",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*");
        juce::Component::SafePointer<MainComponent> safe (this);
        importChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
            [safe] (const juce::FileChooser& fc)
            {
                if (safe == nullptr) return;
                juce::StringArray paths;
                for (auto& f : fc.getResults()) if (f.existsAsFile()) paths.add (f.getFullPathName());
                if (! paths.isEmpty()) safe->filesDropped (paths, safe->getWidth() / 2, safe->getHeight() / 2);
            });
    }
    void closeEverything()
    {
        juce::PopupMenu::dismissAllActiveMenus();
        juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        closeAllEditorWindows();
        std::vector<juce::Component*> pops;
        for (auto& p : popouts) pops.push_back (p.first);
        for (auto* c : pops) reattach (c);
        juce::Array<juce::Component::SafePointer<juce::TopLevelWindow>> others;
        for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i)
            if (auto* w = juce::TopLevelWindow::getTopLevelWindow (i); w != nullptr && w != getTopLevelComponent() && w->isVisible()) others.add (w);
        for (auto& w : others)
        {
            if (w == nullptr) continue;
            if (auto* dw = dynamic_cast<juce::DocumentWindow*> (w.getComponent())) dw->closeButtonPressed();
            else w->setVisible (false);
        }
        showBrowser = false;
        resized();
    }
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
    struct PopOut : public juce::DocumentWindow, private juce::Timer
    {
        PopOut (const juce::String& t) : juce::DocumentWindow ("HumHouse Studio - " + t, col::bg, juce::DocumentWindow::allButtons)
        {
            markFloating (*this);
            fsBtn.setTooltip ("Full screen on / off (F11)");
            fsBtn.onClick = [this] { toggleFull(); };
            juce::Component::addAndMakeVisible (fsBtn);
            startTimer (300);
        }
        std::function<void()> onClose;
        juce::TextButton fsBtn { "FULL SCREEN" };
        // fills the screen in the same desktop space as the main window, so it stays in view while you record
        juce::Rectangle<int> normalBounds; bool filled = false;
        bool isFull() const { return filled || isFullScreen(); }
        void toggleFull()
        {
            if (isFullScreen()) { setFullScreen (false); return; }
            if (filled) { filled = false; setBounds (normalBounds); toFront (true); return; }
            const auto* d = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds());
            if (d == nullptr) return;
            normalBounds = getBounds();
            auto area = d->userArea;
            if (auto* peer = getPeer())
                if (auto f = peer->getFrameSizeIfPresent()) area = f->subtractedFrom (area);
            filled = true;
            setBounds (area);
            toFront (true);
        }
        void closeButtonPressed() override { if (onClose) onClose(); }
        void resized() override { juce::DocumentWindow::resized(); fsBtn.setBounds (getWidth() - 104, getHeight() - 28, 96, 22); fsBtn.toFront (false); }
        bool keyPressed (const juce::KeyPress& k) override
        {
            if (k.getKeyCode() == juce::KeyPress::F11Key || k == juce::KeyPress ('f', juce::ModifierKeys::commandModifier | juce::ModifierKeys::ctrlModifier, 0)) { toggleFull(); return true; }
            return hostKeyHandler && hostKeyHandler (k);
        }
        void timerCallback() override   // stays visible over the main window while you work, not over other apps
        {
            updateFloating (*this);
            if (! floatWithTopmost)   // Mac: a pop-out (full screen or not) stays over the Beat / Rec screens while the app is in front, so it never drops behind
                if (const bool want = juce::Process::isForegroundProcess(); want != isAlwaysOnTop()) setAlwaysOnTop (want);
            fsBtn.setButtonText (isFull() ? "EXIT FULL" : "FULL SCREEN");
        }
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
        c->resized();
    }

    void setView (int v)
    {
        view = v;
        juce::TextButton* tabs[] { &beatTab, &recordTab, &mixTab };
        tabs[v]->setToggleState (true, juce::dontSendNotification);
        resized();
        if (! popouts.empty()) juce::MessageManager::callAsync ([] { raiseFloatingWindows(); });
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
        if (! popouts.empty()) raiseFloatingWindows();
    }

    void timerCallback() override
    {
        if (savedFlash > 0 && --savedFlash == 0) { saveBtn.setButtonText ("SAVE"); saveBtn.setToggleState (false, juce::dontSendNotification); }
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
                writeBackupAsync (engine.project());
                flash ("Auto-backup saved");
            }
        }
    }

    // Pro Tools-style session folder: every save copies the audio the song uses (takes, imported vocals, your own
    // samples) into the project's folder, so the folder alone opens, plays and exports stems anywhere, years later
    void collectAudio()
    {
        auto& p = engine.project();
        if (p.projectFile == juce::File()) return;
        const auto base = p.projectFile.getParentDirectory();
        const auto app = juce::File::getSpecialLocation (juce::File::currentApplicationFile), exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
        std::vector<std::pair<juce::File, juce::File>> jobs;
        std::set<juce::String> seen;
        auto take = [&] (const juce::File& f, const juce::File& dest) {
            if (f == juce::File() || f.isAChildOf (base) || f.isAChildOf (app) || f.isAChildOf (exeDir) || ! seen.insert (f.getFullPathName()).second) return;
            jobs.push_back ({ f, dest });
        };
        const auto shared = docsDir().getChildFile ("Recordings");
        for (auto& c : p.clips) take (c.audioFile, base.getChildFile (c.audioFile.isAChildOf (shared) ? "Recordings" : "Audio Files"));
        for (auto& f : p.audioPool) take (f, base.getChildFile ("Audio Files"));
        for (auto& c : p.channels) take (c.sampleFile, base.getChildFile ("Samples"));
        for (auto& m : p.mixer) take (m.frozenFile, base.getChildFile ("Recordings").getChildFile ("Freeze"));
        if (jobs.empty()) return;
        // copying a project's sounds next to it runs in the background: the project is open and playable at once
        juce::Component::SafePointer<MainComponent> sp (this);
        const auto projFile = p.projectFile;
        juce::Thread::launch ([sp, jobs, projFile] {
            std::map<juce::String, juce::File> moved;
            for (auto& [f, dest] : jobs)
            {
                if (! f.existsAsFile()) continue;
                dest.createDirectory();
                auto t = dest.getChildFile (f.getFileName());
                if (t.existsAsFile() && t.getSize() != f.getSize()) t = dest.getNonexistentChildFile (f.getFileNameWithoutExtension(), f.getFileExtension());
                if (t.existsAsFile() || f.copyFileTo (t)) moved[f.getFullPathName()] = t;
            }
            if (moved.empty()) return;
            juce::MessageManager::callAsync ([sp, moved, projFile] {
                if (sp == nullptr || sp->engine.project().projectFile != projFile) return;
                sp->studio.live ([&moved] (Project& pr) {
                    auto re = [&moved] (juce::File& f) { if (auto it = moved.find (f.getFullPathName()); it != moved.end()) f = it->second; };
                    for (auto& c : pr.clips) re (c.audioFile);
                    for (auto& f : pr.audioPool) re (f);
                    for (auto& c : pr.channels) re (c.sampleFile);
                    for (auto& m : pr.mixer) re (m.frozenFile);
                });
                sp->engine.sync();
            });
        });
    }

    void locateMissingAudio()
    {
        juce::StringArray missing;
        for (auto& c : engine.project().clips)
            if (c.isAudio() && c.audioFile != juce::File() && ! c.audioFile.existsAsFile()) missing.addIfNotAlreadyThere (c.audioFile.getFileName());
        if (missing.isEmpty()) return;
        // first search the whole user folder in the background (Music, Desktop, Documents, Downloads, iCloud, Movies) and glue what's found
        flash ("Searching your disk for " + juce::String (missing.size()) + " missing audio file(s)...");
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::Thread::launch ([sp, missing] {
            std::set<juce::String> want; for (auto& m : missing) want.insert (m.toLowerCase());
            std::map<juce::String, juce::File> byName;
            const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
            juce::Array<juce::File> roots;   // whole user folder (minus Library caches) + every mounted drive, most likely places first
            for (auto* sub : { "Music", "Desktop", "Documents", "Downloads", "Library/Mobile Documents" }) roots.add (home.getChildFile (sub));
            for (const auto& e : juce::RangedDirectoryIterator (home, false, "*", juce::File::findDirectories))
                if (! e.getFile().getFileName().startsWith (".") && e.getFile().getFileName() != "Library") roots.addIfNotAlreadyThere (e.getFile());
            for (const auto& e : juce::RangedDirectoryIterator (juce::File ("/Volumes"), false, "*", juce::File::findDirectories)) roots.add (e.getFile());
            { juce::Array<juce::File> drives; juce::File::findFileSystemRoots (drives);
              for (auto& d : drives) if (! home.isAChildOf (d)) roots.add (d); }   // other Windows drives (D:, E:, USB)
            int seen = 0;
            for (auto& r : roots)
            {
                if (! r.isDirectory() || byName.size() == want.size()) continue;
                for (const auto& e : juce::RangedDirectoryIterator (r, true, "*", juce::File::findFiles, juce::File::FollowSymlinks::no))
                {
                    if (++seen > 4000000 || sp == nullptr) break;
                    const auto n = e.getFile().getFileName().toLowerCase();
                    if (want.count (n) && ! e.getFile().getFullPathName().contains ("/.Trash")) byName.emplace (n, e.getFile());
                    if (byName.size() == want.size()) break;
                }
            }
            juce::MessageManager::callAsync ([sp, byName] {
                auto* mc = sp.getComponent(); if (mc == nullptr) return;
                int found = 0;
                if (! byName.empty())
                    mc->studio.edit ("Find lost audio", [&] (Project& pr) {
                        for (auto& c : pr.clips)
                            if (c.isAudio() && ! c.audioFile.existsAsFile())
                                if (auto it = byName.find (c.audioFile.getFileName().toLowerCase()); it != byName.end()) { c.audioFile = it->second; ++found; }
                        for (auto& f : pr.audioPool)
                            if (! f.existsAsFile()) if (auto it = byName.find (f.getFileName().toLowerCase()); it != byName.end()) f = it->second;
                    });
                if (found > 0) { mc->collectAudio(); mc->engine.sync(); mc->flash ("Found " + juce::String (found) + " audio clip(s) on disk and glued them into this song's folder - save to keep"); }
                mc->askForMissingAudio();
            });
        });
    }

    void askForMissingAudio()
    {
        juce::StringArray missing;
        for (auto& c : engine.project().clips)
            if (c.isAudio() && c.audioFile != juce::File() && ! c.audioFile.existsAsFile()) missing.addIfNotAlreadyThere (c.audioFile.getFileName());
        if (missing.isEmpty()) return;
        auto* w = new juce::AlertWindow ("Audio not found", juce::String (missing.size()) + " audio file(s) in this project weren't found where they were saved:\n"
                                         + missing.joinIntoString ("\n").substring (0, 600) + "\n\nPick the folder they're in (sub-folders are searched too).",
                                         juce::MessageBoxIconType::WarningIcon);
        w->addButton ("Locate folder...", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Skip", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this] (int r) {
            if (r != 1) return;
            chooser = std::make_unique<juce::FileChooser> ("Folder with the missing audio", engine.project().projectFile.getParentDirectory());
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc) {
                const auto dir = fc.getResult();
                if (! dir.isDirectory()) return;
                std::map<juce::String, juce::File> byName;
                for (const auto& e : juce::RangedDirectoryIterator (dir, true, "*", juce::File::findFiles))
                    byName.emplace (e.getFile().getFileName().toLowerCase(), e.getFile());
                int found = 0;
                studio.edit ("Locate audio", [&] (Project& pr) {
                    for (auto& c : pr.clips)
                        if (c.isAudio() && ! c.audioFile.existsAsFile())
                            if (auto it = byName.find (c.audioFile.getFileName().toLowerCase()); it != byName.end()) { c.audioFile = it->second; ++found; }
                    for (auto& f : pr.audioPool)
                        if (! f.existsAsFile()) if (auto it = byName.find (f.getFileName().toLowerCase()); it != byName.end()) f = it->second;
                });
                engine.sync();
                flash ("Found " + juce::String (found) + " audio clip(s)");
            });
        }), true);
    }

    void writeProject()
    {
        engine.storePluginStates();
        collectAudio();
        auto& p = engine.project();
        if (saveProject (p, p.projectFile))
        {
            writeBackup (p);
            studio.dirty = false;
            syncControls();
            flash ("Saved " + p.projectFile.getFileName() + " + backup");
            saveBtn.setButtonText ("SAVED"); saveBtn.setToggleState (true, juce::dontSendNotification);   // opposite color = confirmed
            savedFlash = 45;
        }
        else juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save", "Could not save the project.");
    }

    // FL-style: Save / Don't Save / Cancel before the current project is closed
    void askToSave (std::function<void()> then)
    {
        if (! studio.dirty) { then(); return; }
        if (savePromptOpen) return;
        savePromptOpen = true;
        auto* w = new juce::AlertWindow ("Save changes?", "Save changes to \"" + engine.project().title + "\" first?", juce::MessageBoxIconType::QuestionIcon);
        w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Don't Save", 2);
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this, then] (int r) {
            savePromptOpen = false;
            if (r == 2) { engine.storePluginStates(); writeBackup (engine.project()); then(); }   // "Don't Save" still keeps a backup
            else if (r == 1) save (false, then);
        }), true);
    }
public:
    void requestQuit() { askToSave ([] { juce::JUCEApplication::getInstance()->quit(); }); }
private:
    bool savePromptOpen = false;

    void save (bool as, std::function<void()> after = {})
    {
        auto& p = engine.project();
        if (! as && p.projectFile.existsAsFile()) { writeProject(); if (after && ! studio.dirty) after(); return; }
        docsDir().createDirectory();
        chooser = std::make_unique<juce::FileChooser> ("Save project", docsDir().getChildFile (p.title + ".hhs"), "*.hhs");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, after] (const juce::FileChooser& fc) {
                                  auto f = fc.getResult();
                                  if (f == juce::File()) return;
                                  f = f.withFileExtension ("hhs");
                                  engine.project().projectFile = f;
                                  engine.project().title = f.getFileNameWithoutExtension();
                                  writeProject();
                                  if (after && ! studio.dirty) after();
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
        juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<MainComponent> (this)] { if (sp) { sp->collectAudio(); sp->locateMissingAudio(); } });   // glue: copy outside audio into the song folder
    }

    void open (bool backups) { askToSave ([this, backups] { openChooser (backups); }); }
    void openChooser (bool backups)
    {
        auto dir = backups ? backupDir() : docsDir();
        chooser = std::make_unique<juce::FileChooser> (backups ? "Open backup" : "Open project", dir, "*.hhs");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) load (fc.getResult()); });
    }

    static juce::File templatesDir() { return docsDir().getChildFile ("Templates"); }
    static juce::File defaultTemplate() { return templatesDir().getChildFile ("_default.hhs"); }
    juce::StringArray templateFolders() const
    {
        juce::StringArray dirs { templatesDir().getFullPathName() };
        if (studio.settings) for (auto& d : juce::StringArray::fromLines (studio.settings->getValue ("templateFolders"))) if (d.isNotEmpty()) dirs.addIfNotAlreadyThere (d);
        return dirs;
    }
    juce::Array<juce::File> templateFiles() const
    {
        juce::Array<juce::File> fs;
        for (auto& d : templateFolders())
            for (auto& f : juce::File (d).findChildFiles (juce::File::findFiles, true, "*.hhs"))
                if (f.getFileName() != "_default.hhs" && ! f.getFullPathName().contains ("Backup") && ! fs.contains (f)) fs.add (f);
        std::sort (fs.begin(), fs.end(), [] (const juce::File& a, const juce::File& b) { return a.getFileNameWithoutExtension().compareNatural (b.getFileNameWithoutExtension()) < 0; });
        return fs;
    }
    void addTemplateFolder()
    {
        chooser = std::make_unique<juce::FileChooser> ("Folder with templates", juce::File::getSpecialLocation (juce::File::userDesktopDirectory));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc) {
            const auto d = fc.getResult();
            if (! d.isDirectory() || ! studio.settings) return;
            auto dirs = juce::StringArray::fromLines (studio.settings->getValue ("templateFolders"));
            dirs.addIfNotAlreadyThere (d.getFullPathName()); dirs.removeEmptyStrings();
            studio.settings->setValue ("templateFolders", dirs.joinIntoString ("\n")); studio.settings->saveIfNeeded();
            flash (juce::String (templateFiles().size()) + " templates");
        });
    }
    void saveTemplate()
    {
        auto* aw = new juce::AlertWindow ("Save as template", "Name this template (tracks, mixer, FX and kit are saved)", juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("name", lastTemplate.isNotEmpty() ? lastTemplate : juce::String ("My template"));
        aw->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([this, aw] (int r) {
            const auto name = juce::File::createLegalFileName (aw->getTextEditorContents ("name").trim());
            if (r != 1 || name.isEmpty()) return;
            writeTemplate (name);
        }), true);
    }
    juce::String lastTemplate;   // the template this project came from / was last saved as
    void writeTemplate (const juce::String& name)
    {
        engine.storePluginStates();
        templatesDir().createDirectory();
        auto f = templatesDir().getChildFile (name + ".hhs");
        auto tmp = f.getSiblingFile (name + ".hhs.tmp");
        if (saveProject (engine.project(), tmp) && tmp.moveFileTo (f)) lastTemplate = name;   // same name = replace, never half-written
        else tmp.deleteFile();
    }

    void shareTemplatePack (const Project& p, const juce::String& name)
    {
        auto proj = std::make_shared<Project> (p);
        chooser = std::make_unique<juce::FileChooser> ("Share template (one file with its sounds inside - email it, AirDrop it, text it)",
            juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile (juce::File::createLegalFileName (name) + ".hhpack"), "*.hhpack");
        juce::Component::SafePointer<MainComponent> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe, proj, name] (const juce::FileChooser& fc) {
                auto f = fc.getResult();
                if (safe == nullptr || f == juce::File()) return;
                f = f.withFileExtension ("hhpack");
                if (packTemplate (*proj, name, f)) { safe->flash ("Template file ready: " + f.getFileName()); f.revealToUser(); }
                else juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Share template", "Couldn't write " + f.getFullPathName());
            });
    }
    void shareProjectAsTemplate()
    {
        engine.storePluginStates();
        auto& p = engine.project();
        shareTemplatePack (p, lastTemplate.isNotEmpty() ? lastTemplate
                            : p.projectFile.existsAsFile() ? p.projectFile.getFileNameWithoutExtension() : juce::String ("My template"));
    }
    void importSharedFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Import a HumHouse file", juce::File::getSpecialLocation (juce::File::userDesktopDirectory), "*.hhpack;*.hhpreset;*.hhchain;*.hhs");
        juce::Component::SafePointer<MainComponent> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc) {
            if (safe != nullptr && fc.getResult().existsAsFile()) safe->openFile (fc.getResult());
        });
    }

public:
    // a file opened from Finder / Explorer / an email, or dropped on the window
    void openFile (const juce::File& f)
    {
        auto fail = [] (const juce::String& what) {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Open", "That file isn't a HumHouse " + what + ".");
        };
        if (f.hasFileExtension ("hhpack"))
        {
            auto t = unpackTemplate (f, templatesDir());
            if (t == juce::File()) { fail ("template"); return; }
            flash ("Template added: " + t.getFileNameWithoutExtension() + " (File > New from template)");
            newProject (t);
        }
        else if (f.hasFileExtension ("hhpreset"))
        {
            auto t = installSharedPreset (f);
            if (t == juce::File()) fail ("preset");
            else flash ("Preset added: " + t.getFileNameWithoutExtension() + " (in that effect's PRESETS menu)");
        }
        else if (f.hasFileExtension ("hhchain"))
        {
            auto t = installSharedChain (f);
            if (t == juce::File()) fail ("FX chain");
            else flash ("FX chain added: " + t.getFileNameWithoutExtension() + " (right-click a track > FX chain presets)");
        }
        else if (f.hasFileExtension ("hhs"))
            askToSave ([this, f] { load (f); });
    }
    bool isInterestedInFileDrag (const juce::StringArray& files) override { return allHumHouseShareFiles (files); }
    void filesDropped (const juce::StringArray& files, int, int) override
    {
        for (auto& p : files) openFile (juce::File (p));
    }
private:

    void newProject (juce::File templ = {}) { askToSave ([this, templ] { startNewProject (templ); }); }
    void startNewProject (juce::File templ)
    {
        if (studio.dirty) { engine.storePluginStates(); writeBackup (engine.project()); }
        closeAllEditorWindows();
        if (templ == juce::File()) templ = defaultTemplate();
        lastTemplate = templ.existsAsFile() && templ != defaultTemplate() ? templ.getFileNameWithoutExtension() : juce::String();
        Project fresh = Project::createDefault();
        if (templ.existsAsFile())
        {
            Project t;
            if (loadProject (t, templ)) { fresh = std::move (t); fresh.title = Project().title; fresh.projectFile = juce::File(); }   // a new song never records into / saves over the template
        }
        engine.newProject (std::move (fresh));
        studio.selectedTrack = 1; studio.selectedLanes.clear(); studio.laneAnchor = -1;
        studio.undo.clear();
        studio.undo.push (engine.project(), "New project");
        studio.selectedChannel = engine.project().channels.empty() ? -1 : engine.project().channels.front().id;
        studio.dirty = false;
        studio.sendChangeMessage();
    }

    // File > Export: name, WAV / MP3 / M4A, full mix / one track / every track as a stem, then pick the folder
    void exportWav (int track)
    {
        static const int bitsTab[] { 16, 24, 32, 64 };
        static const double rateTab[] { 0, 44100, 48000, 88200, 96000, 192000 };
        auto& p = engine.project();
        const bool hasTrack = track > 0 && track < (int) p.mixer.size();
        auto* w = new juce::AlertWindow ("Export", "Name it, pick the format and what to export - then choose the folder", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", p.title, "Name");
        w->addComboBox ("what", { "Full mix",
                                  hasTrack ? "Selected track only (" + juce::String (track) + " " + p.mixer[(size_t) track].name + ")" : juce::String ("Selected track only (select a mixer insert first)"),
                                  "All tracks as stems (each starts at bar 1)" }, "Export");
        w->addComboBox ("fmt", { "WAV", "MP3 (320 kbps)", "M4A / AAC" }, "Format");
        w->addComboBox ("bits", { "16-bit", "24-bit", "32-bit float", "64-bit float" }, "WAV bit depth");
        w->addComboBox ("rate", { "Project / device rate", "44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz", "192 kHz" }, "Sample rate (MP3/M4A: 44.1 or 48)");
        int bi = 1, ri = 0;
        for (int i = 0; i < 4; ++i) if (bitsTab[i] == engine.exportBits) bi = i;
        for (int i = 0; i < 6; ++i) if (rateTab[i] == engine.exportRate) ri = i;
        w->getComboBoxComponent ("what")->setSelectedItemIndex (hasTrack ? 1 : 0);
        w->getComboBoxComponent ("fmt")->setSelectedItemIndex (juce::jlimit (0, 2, engine.exportFormat));
        w->getComboBoxComponent ("bits")->setSelectedItemIndex (bi);
        w->getComboBoxComponent ("rate")->setSelectedItemIndex (ri);
        w->addButton ("Choose folder...", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w, track, hasTrack] (int r) {
            if (r != 1) return;
            engine.exportBits = bitsTab[juce::jlimit (0, 3, w->getComboBoxComponent ("bits")->getSelectedItemIndex())];
            engine.exportRate = rateTab[juce::jlimit (0, 5, w->getComboBoxComponent ("rate")->getSelectedItemIndex())];
            engine.exportFormat = juce::jlimit (0, 2, w->getComboBoxComponent ("fmt")->getSelectedItemIndex());
            int what = juce::jlimit (0, 2, w->getComboBoxComponent ("what")->getSelectedItemIndex());
            if (what == 1 && ! hasTrack) what = 0;
            auto name = juce::File::createLegalFileName (w->getTextEditorContents ("name").trim());
            if (name.isEmpty()) name = juce::File::createLegalFileName (engine.project().title);
            chooseExportFolder (name, what, track);
        }), true);
    }

    static bool insertUsed (const Project& p, int t)
    {
        for (auto& c : p.channels) if (c.mixerTrack == t) return true;
        for (auto& l : p.lanes) if (l.mixerTrack == t) return true;
        for (size_t i = 0; i < p.mixer.size(); ++i)
            if ((int) i != t) for (auto& sd : p.mixer[i].sends) if (sd.target == t) return true;
        return false;
    }

    void chooseExportFolder (juce::String name, int what, int track)
    {
        juce::File start (studio.settings ? studio.settings->getValue ("exportDir") : juce::String());
        if (! start.isDirectory()) start = docsDir();
        chooser = std::make_unique<juce::FileChooser> ("Choose the export folder", start);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this, name, what, track] (const juce::FileChooser& fc) {
                                  const auto dir = fc.getResult();
                                  if (! dir.isDirectory()) return;
                                  if (studio.settings) { studio.settings->setValue ("exportDir", dir.getFullPathName()); studio.settings->saveIfNeeded(); }
                                  const juce::String ext = engine.exportFormat == 1 ? ".mp3" : engine.exportFormat == 2 ? ".m4a" : ".wav";
                                  auto& p = engine.project();
                                  std::vector<RenderJob::Item> items;
                                  auto trackName = [&p] (int t) { return juce::File::createLegalFileName (p.mixer[(size_t) t].name.isEmpty() ? "Insert " + juce::String (t) : p.mixer[(size_t) t].name); };
                                  if (what == 2)
                                  {
                                      for (int t = 1; t < (int) p.mixer.size(); ++t)
                                          if (insertUsed (p, t))
                                              items.push_back ({ dir.getChildFile (name + " - " + juce::String (t).paddedLeft ('0', 2) + " " + trackName (t) + ext), t });
                                      if (items.empty()) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Export", "No mixer inserts have anything routed to them."); return; }
                                  }
                                  else items.push_back ({ dir.getChildFile (name + (what == 1 ? " - " + trackName (track) : juce::String()) + ext), what == 1 ? track : -1 });
                                  engine.stop();
                                  // stems always render the whole song from bar 1 so they line up in any DAW
                                  (new RenderJob (engine, std::move (items), what == 2 ? true : p.songMode, what == 2))->launchThread();
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

    struct ShortcutsWindow : juce::DocumentWindow
    {
        ShortcutsWindow (const juce::String& text, std::function<void()> close)
            : juce::DocumentWindow ("Shortcuts  (Cmd/Ctrl+U to close)", col::panel, juce::DocumentWindow::closeButton), onClose (std::move (close))
        {
            auto* ed = new juce::TextEditor();
            ed->setMultiLine (true); ed->setReadOnly (true); ed->setCaretVisible (false); ed->setScrollbarsShown (true);
            ed->setFont (juce::FontOptions (14.0f)); ed->setText (text, false);
            ed->setColour (juce::TextEditor::backgroundColourId, col::bg); ed->setColour (juce::TextEditor::textColourId, col::text);
            ed->setColour (juce::TextEditor::outlineColourId, col::panel2);
            setContentOwned (ed, false);
            setUsingNativeTitleBar (true); setResizable (true, false); centreWithSize (860, 620); setVisible (true);
        }
        void closeButtonPressed() override { if (onClose) onClose(); }
        bool keyPressed (const juce::KeyPress& k) override
        {
            if (k == juce::KeyPress ('u', juce::ModifierKeys::commandModifier, 0) || k == juce::KeyPress::escapeKey) { if (onClose) onClose(); return true; }
            return false;
        }
        std::function<void()> onClose;
    };
    std::unique_ptr<ShortcutsWindow> shortcutsWin;
    void showShortcuts()
    {
        if (shortcutsWin != nullptr) { juce::MessageManager::callAsync ([this] { shortcutsWin.reset(); }); return; }   // toggle off
        juce::String t;
        t << "GLOBAL\nSpace play/stop | R record | L pattern/song | F5 playlist | F6/F7 rack/piano roll | F8 Plugin House | F9 mixer\n"
          << "Ctrl+S save + backup | Ctrl+Shift+S save as | Ctrl+O open | Ctrl+N new | Ctrl+R export (WAV / MP3 / M4A / stems) | Ctrl+M metronome\n"
          << "Ctrl+Z undo | Ctrl+Shift+Z / Ctrl+Y redo | Ctrl+Alt+Z step back (999 levels)\n\n"
          << "PIANO ROLL\nAlt+A arpeggiator / rolls | Alt+S strum | Alt+U chop | Alt+F flam | Alt+Q quantize | Alt+R randomize\n"
          << "Alt+K limit | Alt+L articulate | Alt+X scale levels | Alt+Y flip | Alt+W claw | Alt+O LFO | Alt+M mute | Alt+V ghost notes\n"
          << "Ctrl+B duplicate | Ctrl+G glue | Ctrl+L legato | Ctrl+Shift+U quick chop | Ctrl+Q quick quantize | Shift+Q quantize starts\n"
          << "Shift+Up/Down semitone | Ctrl+Up/Down octave | Shift+Left/Right move | Alt+Left/Right nudge | Ctrl+A select all | Del delete\n"
          << "Ctrl+C / Ctrl+V copy / paste | Shift+D reset lengths | double-click note = slide/glide\n\n"
          << "PLAYLIST\nDrag ruler = punch range (double-click clears) | double-click empty = place pattern | drag clip corners = fades\n"
          << "Alt-drag fade = curve | right-click clip = takes, route to insert, split, fades | Ctrl+B duplicate | Del delete\n\n"
          << "PLAYLIST TRACKS\nRight-click a track = color, HumHouse Drummer, routing, delete | side bar = all track heights | mini box (track corner) = this track's height, double-click = small / large\n"
          << "Alt+A roll audio clips | E select | P/B paint | D delete | T mute | C slice | Alt+wheel = track heights\n\n"
          << "HELP\nHover anything for 1 second to see what it does | Cmd/Ctrl+U = this Shortcuts window\n\n"
          << "Ctrl = Cmd and Alt = Option on macOS.";
        shortcutsWin = std::make_unique<ShortcutsWindow> (t, [this] { juce::MessageManager::callAsync ([this] { shortcutsWin.reset(); }); });
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

std::unique_ptr<juce::Component> createMainComponent() { return std::make_unique<MainComponent>(); }
void openFileInMain (juce::Component* main, const juce::File& f)
{
    if (auto* m = dynamic_cast<MainComponent*> (main)) m->openFile (f);
}
void requestQuit (juce::Component* main)
{
    if (auto* m = dynamic_cast<MainComponent*> (main)) m->requestQuit();
    else juce::JUCEApplication::getInstance()->quit();
}
}
