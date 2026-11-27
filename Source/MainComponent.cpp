#include <optional>
#include <set>
#include "UI.h"
#include "ProjectImport.h"
#include <thread>
#include "BinaryData.h"
#if JUCE_IOS
void* hhsWatchPhoneRoute (std::atomic<bool>& speakerOut);   // IosRoute.mm
void hhsUnwatchPhoneRoute (void* token);
#if defined (HHS_APPSTORE) && HHS_APPSTORE
bool hhsStoreStart (std::function<void (bool unlocked, std::string message)> onChange);   // AppStore.mm
bool hhsStoreUnlocked();
void hhsStoreBuy();
void hhsStoreRestore();
void hhsStoreManage();
std::string hhsStorePrice();
#define HHS_STORE 1
#endif
#endif

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
static void writePaintCost()   // ms per full paint of each view, including its children (10-frame average)
{
    struct Row { double ms; juce::String name; };
    std::vector<Row> rows;
    std::function<void (juce::Component&, int, const juce::String&)> walk = [&] (juce::Component& c, int depth, const juce::String& path)
    {
        if (! c.isShowing() || c.getWidth() < 40 || c.getHeight() < 20) return;
        const auto name = path + "/" + (c.getName().isNotEmpty() ? c.getName() : juce::String (typeid (c).name()));
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < 10; ++i) (void) c.createComponentSnapshot (c.getLocalBounds(), true, 1.0f);
        rows.push_back ({ (juce::Time::getMillisecondCounterHiRes() - t0) / 10.0,
                          name + "  " + juce::String (c.getWidth()) + "x" + juce::String (c.getHeight()) });
        if (depth < 3) for (auto* k : c.getChildren()) walk (*k, depth + 1, name);
    };
    auto& d = juce::Desktop::getInstance();
    for (int i = 0; i < d.getNumComponents(); ++i) if (auto* c = d.getComponent (i)) walk (*c, 0, {});
    std::sort (rows.begin(), rows.end(), [] (const Row& a, const Row& b) { return a.ms > b.ms; });
    juce::String out;
    for (auto& r : rows) out << juce::String (r.ms, 2) << " ms  " << r.name << "\n";
    appDataFile ("paintcost.txt").replaceWithText (out);
}
bool& humhouseTitleBars() { static bool on = ! appDataFile ("titlebars.txt").existsAsFile() || appDataFile ("titlebars.txt").loadFileAsString().getIntValue() != 0; return on; }
int& fontStyle() { static int st = 3; return st; }   // Pro is the only clean one: Tattoo / Flash / System drew odd characters
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
    if (col::neon && ! col::lightMode && ! box.getProperties().contains ("smallArrow"))   // Neon 3D: glass dropdown with a lit rim and chevron
    {
        const auto b = juce::Rectangle<int> (0, 0, w, h).toFloat().reduced (0.5f);
        const float rr = juce::jmin (6.0f, b.getHeight() * 0.3f);
        col::well (g, b, rr);   // Neon 3D iron: dropdown sits in a recessed slot
        g.setColour (col::accent.withAlpha (box.hasKeyboardFocus (true) || down ? 0.9f : 0.35f)); g.drawRoundedRectangle (b, rr, 1.0f);
        const float cx = (float) w - 12.0f, cy = (float) h * 0.5f;
        juce::Path a; a.startNewSubPath (cx - 4.0f, cy - 2.0f); a.lineTo (cx, cy + 2.0f); a.lineTo (cx + 4.0f, cy - 2.0f);
        g.setColour (col::text.withAlpha (box.isEnabled() ? 0.9f : 0.3f)); g.strokePath (a, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return;
    }
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
    static auto pro = juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowRegular_ttf, (size_t) BinaryData::BarlowRegular_ttfSize);
    static auto proBold = juce::Typeface::createSystemTypefaceFor (BinaryData::BarlowSemiBold_ttf, (size_t) BinaryData::BarlowSemiBold_ttfSize);
    if (pro != nullptr) return f.isBold() && proBold != nullptr ? proBold : pro;   // HumHouse font (Barlow) everywhere
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
        "- Note names: middle C = C5 (C0 to G10)\n"
        "- One-shots: smoother pitched playback (Hermite), SET (wrench) button on every channel, on/off buttons for Normalize, Reverse, Declick...\n"
        "- One-shots / 808 cut mode: no click at the start or when the next hit cuts the last one\n"
        "- Piano Roll LOCK: notes you draw, drag or stamp snap to your key and scale\n"
        "- Sample previews stop when you click anywhere, press Stop or Space\n"
        "- Recording: earlier takes and the beat play instantly when you hit record (no dropout while take files are created, punch pre-roll preloaded)\n"
        "- Studio Vocal (Studio FX): 0-latency vocal smoother - Smooth, De-Ess, Silk air, Warmth + 7 presets\n"
        "- No more phasing on buses: automatic plugin delay compensation, the Mix knob lines dry up with latent FX, mono plugins get the mono sum instead of just the left side\n"
        "- AutoPitch rebuilt: Auto-Tune style single read head spliced on whole pitch cycles - full voice, no clicks or phasing, ~0.1-6 ms\n"
        "- Knobs: drag up/down or scroll, Shift = fine, double-click to type a value, Alt-click resets\n"
        "- Studio EQ: Pro-Q style band bar (type / freq / gain / Q, type values in), selected band shape, listen (Alt+drag a dot), Shift = fine, display range 6 / 12 / 18 / 30 dB\n"
        "- Trackpad pinch zoom + Ctrl/Cmd+scroll zoom at the mouse in Playlist and Piano Roll\n"
        "- Snap menu in the Playlist and finer snaps everywhere: (none), 1/6, 1/4, 1/3, 1/2 step, 1/6, 1/3, 1/2 beat, beat, bar\n"
        "- Playlist keys: E select (drag a box over a chorus), P/B paint, D delete, T mute, C slice\n"
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
        "- New takes over an earlier take drop to the next lane instead of covering it\n"
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
    const float ang = a0 + pos * (a1 - a0), rad = size * 0.5f - 2.5f;
    const auto body = c.reduced (size * 0.17f);
    const float br = body.getWidth() * 0.5f;
    const juce::PathStrokeType ring (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    if (col::neon && ! col::lightMode)   // Neon 3D: a lit glass dome on a dark well, the value arc glows
    {
        const auto fill = s.isEnabled() ? s.findColour (juce::Slider::rotarySliderFillColourId) : col::dim;
        const auto key = col::mix (col::mix (col::mix (0x4b4e4f42ull, fill.getARGB()), (juce::uint64) juce::roundToInt (a0 * 100.0f)),
                                   (juce::uint64) juce::roundToInt (a1 * 100.0f));
        col::drawCached (g, c, key, [&] (juce::Graphics& g, juce::Rectangle<float> c)   // the knob without its value: drawn once, blitted
        {
        const auto body = c.reduced (size * 0.17f);
        g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillEllipse (body.expanded (2.0f).translated (0.0f, 2.5f));
        juce::ColourGradient dome (juce::Colour (0xff3a3f4e), body.getX() + br * 0.6f, body.getY() + br * 0.45f, juce::Colour (0xff05060b), body.getRight(), body.getBottom(), true);
        g.setGradientFill (dome); g.fillEllipse (body);
        {   // Neon 3D iron: machined cap, concentric turning rings + anisotropic sheen + chrome rim
            const auto cc = body.getCentre();
            for (int i = 1; i <= 3; ++i) { g.setColour (juce::Colours::white.withAlpha (0.045f)); g.drawEllipse (body.reduced (br * 0.2f * (float) i), 0.8f); }
            for (float a : { 0.0f, juce::MathConstants<float>::pi })
            {
                juce::Path sheen; sheen.addPieSegment (body.reduced (1.0f), a - 0.45f, a + 0.45f, 0.15f);
                g.setColour (juce::Colours::white.withAlpha (a == 0.0f ? 0.13f : 0.06f)); g.fillPath (sheen);
            }
            g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.5f), cc.x, body.getY(), juce::Colours::black.withAlpha (0.7f), cc.x, body.getBottom(), false));
            g.drawEllipse (body.reduced (0.5f), 1.2f);
        }
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.22f), body.getCentreX(), body.getY(), juce::Colours::transparentWhite, body.getCentreX(), body.getCentreY(), false));
        g.fillEllipse (body.reduced (br * 0.12f).withTrimmedBottom (br * 0.7f));   // specular highlight
        g.setColour (fill.withAlpha (0.35f)); g.drawEllipse (body, 1.0f);
        juce::Path track;
        track.addCentredArc (c.getCentreX(), c.getCentreY(), rad, rad, 0, a0, a1, true);
        g.setColour (juce::Colour (0xff1a1f33)); g.strokePath (track, ring);
        });
        juce::Path arc;
        arc.addCentredArc (c.getCentreX(), c.getCentreY(), rad, rad, 0, a0, ang, true);
        g.setColour (fill.withAlpha (0.10f)); g.strokePath (arc, juce::PathStrokeType (11.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (fill.withAlpha (0.25f)); g.strokePath (arc, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (fill.brighter (0.25f)); g.strokePath (arc, ring);
        const auto tip = c.getCentre().getPointOnCircumference (rad, ang);
        g.setColour (fill.withAlpha (0.4f)); g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre (tip));
        g.setColour (juce::Colours::white); g.fillEllipse (juce::Rectangle<float> (3.5f, 3.5f).withCentre (tip));
        juce::Path ptr; ptr.startNewSubPath (c.getCentre().getPointOnCircumference (br * 0.35f, ang)); ptr.lineTo (c.getCentre().getPointOnCircumference (br - 3.0f, ang));
        g.setColour (juce::Colours::white.withAlpha (0.9f)); g.strokePath (ptr, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return;
    }
    g.setColour (juce::Colours::black.withAlpha (0.4f)); g.fillEllipse (body.translated (0.0f, 1.5f));   // machined cap with drop shadow
    g.setGradientFill (juce::ColourGradient (col::panel2.brighter (0.3f), body.getCentreX(), body.getY(), col::panel2.darker (0.35f), body.getCentreX(), body.getBottom(), false));
    g.fillEllipse (body);
    g.setColour (juce::Colours::white.withAlpha (0.09f)); g.drawEllipse (body.reduced (0.5f), 1.0f);
    juce::Path track, arc;
    track.addCentredArc (c.getCentreX(), c.getCentreY(), rad, rad, 0, a0, a1, true);
    g.setColour (col::bg.darker (0.3f));
    g.strokePath (track, ring);
    arc.addCentredArc (c.getCentreX(), c.getCentreY(), rad, rad, 0, a0, ang, true);
    const auto fill = s.isEnabled() ? s.findColour (juce::Slider::rotarySliderFillColourId) : col::dim;
    g.setColour (fill.withAlpha (0.22f)); g.strokePath (arc, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (fill); g.strokePath (arc, ring);
    juce::Path ptr; ptr.startNewSubPath (c.getCentre().getPointOnCircumference (br * 0.35f, ang)); ptr.lineTo (c.getCentre().getPointOnCircumference (br - 2.0f, ang));
    g.setColour (col::text); g.strokePath (ptr, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void HHLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    auto base = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : b.findColour (juce::TextButton::buttonColourId);
    if (down) base = base.brighter (0.15f);
    else if (over) base = base.brighter (0.06f);
    if (b.getProperties().contains ("swatch"))   // color pickers: the color itself, in every theme (Neon's metal keys hid it)
    {
        g.setColour (base); g.fillRoundedRectangle (r, 3.0f);
        g.setColour (b.getToggleState() || over ? col::text : juce::Colours::black.withAlpha (0.45f)); g.drawRoundedRectangle (r, 3.0f, b.getToggleState() ? 2.0f : 1.0f);
        return;
    }
    if (col::neon && ! col::lightMode)   // Neon 3D: a raised glass key, lit from above; ON glows in the accent
    {
        const bool on = b.getToggleState();
        const float rr = juce::jmin (7.0f, r.getHeight() * 0.3f);
        const auto key = col::mix (col::mix (0x4b455953ull, col::accent.getARGB()), (juce::uint64) ((on ? 4 : 0) + (down ? 2 : over ? 1 : 0)));
        col::drawCached (g, b.getLocalBounds().toFloat(), key, [&] (juce::Graphics& g, juce::Rectangle<float> area)   // drawn once per size / state, blitted
        {
        const auto r = area.reduced (0.5f);
        g.setColour (juce::Colours::black.withAlpha (0.5f)); g.fillRoundedRectangle (r.translated (0.0f, 1.5f), rr);
        col::metal (g, r, rr, down ? 0.0f : over ? 0.15f : 0.05f);   // Neon 3D iron: brushed gunmetal key with a chrome bevel
        if (on) { g.setColour (col::accent.withAlpha (0.26f)); g.fillRoundedRectangle (r, rr); }
        g.setColour (on ? col::accent : col::accent.withAlpha (over ? 0.55f : 0.22f)); g.drawRoundedRectangle (r.reduced (0.5f), rr, on ? 1.5f : 1.0f);
        if (on) { g.setColour (col::accent.withAlpha (0.18f)); g.drawRoundedRectangle (r.reduced (1.5f), rr, 3.0f); }
        });
        return;
    }
    g.setGradientFill (juce::ColourGradient (base.brighter (0.07f), 0.0f, r.getY(), base.darker (0.07f), 0.0f, r.getBottom(), false));   // studio console look: soft top light
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.drawRoundedRectangle (r, 3.0f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawHorizontalLine ((int) r.getY() + 1, r.getX() + 3.0f, r.getRight() - 3.0f);
    if (b.getToggleState() && r.getHeight() > 12.0f) { g.setColour (col::accent); g.fillRoundedRectangle (r.getX() + 4.0f, r.getBottom() - 3.0f, r.getWidth() - 8.0f, 2.0f, 1.0f); }
}

void HHLookAndFeel::drawTabButton (juce::TabBarButton& b, juce::Graphics& g, bool over, bool)
{
    auto r = b.getActiveArea().toFloat();
    const bool front = b.isFrontTab();
    if (col::neon && ! col::lightMode)   // Neon 3D: the open tab is a lit glass pill
    {
        const auto pill = r.reduced (3.0f, 3.0f);
        g.setColour (col::panel); g.fillRect (r);
        if (front || over)
        {
            g.setGradientFill (juce::ColourGradient (col::panel2.interpolatedWith (col::accent, front ? 0.3f : 0.12f).brighter (0.1f), 0.0f, pill.getY(), col::panel2, 0.0f, pill.getBottom(), false));
            g.fillRoundedRectangle (pill, 5.0f);
            g.setColour (col::accent.withAlpha (front ? 0.9f : 0.4f)); g.drawRoundedRectangle (pill, 5.0f, 1.0f);
        }
        g.setColour (front ? col::text : col::dim);
        g.setFont (juce::FontOptions (phoneUI ? 15.0f : 12.0f, juce::Font::bold));
        g.drawText (b.getButtonText(), b.getTextArea(), juce::Justification::centred, false);
        return;
    }
    g.setColour (front ? col::panel2 : over ? col::panel2.withAlpha (0.5f) : col::panel);
    g.fillRect (r);
    if (front) { g.setColour (col::accent); g.fillRect (r.removeFromBottom (2.0f).reduced (8.0f, 0.0f)); }
    g.setColour (front ? col::text : col::dim);
    g.setFont (juce::FontOptions (phoneUI ? 15.0f : 12.0f, juce::Font::bold));
    g.drawText (b.getButtonText(), b.getTextArea(), juce::Justification::centred, false);
}

void HHLookAndFeel::drawTabbedButtonBarBackground (juce::TabbedButtonBar& bar, juce::Graphics& g)
{
    g.setColour (col::panel); g.fillRect (bar.getLocalBounds());
    g.setColour (juce::Colours::black.withAlpha (0.35f)); g.drawHorizontalLine (bar.getHeight() - 1, 0.0f, (float) bar.getWidth());
}

void HHLookAndFeel::drawTabAreaBehindFrontButton (juce::TabbedButtonBar&, juce::Graphics&, int, int) {}

juce::Font HHLookAndFeel::getTextButtonFont (juce::TextButton& b, int h)
{
    if (! phoneUI) return LookAndFeel_V4::getTextButtonFont (b, h);
    return juce::Font (juce::FontOptions (juce::jmin (18.0f, (float) h * 0.5f), juce::Font::bold));
}

namespace
{
struct TitleBarButton : juce::Button   // round, lit like the rest of the UI; drawn inside the window so nothing can cover it
{
    explicit TitleBarButton (int t)
        : juce::Button (t == juce::DocumentWindow::closeButton ? "close" : t == juce::DocumentWindow::minimiseButton ? "minimise" : "maximise"), type (t)
    {
        setTooltip (t == juce::DocumentWindow::closeButton ? "Close (Esc)" : t == juce::DocumentWindow::minimiseButton ? "Minimize" : "Maximize");
        setWantsKeyboardFocus (false);
    }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto r = getLocalBounds().toFloat();
        const float d = juce::jmin (r.getWidth(), r.getHeight()) * 0.7f;
        const auto c = r.withSizeKeepingCentre (d, d);
        const juce::Colour base = type == juce::DocumentWindow::closeButton ? juce::Colour (0xffe8424f)
                                : type == juce::DocumentWindow::minimiseButton ? juce::Colour (0xff2f7df6) : juce::Colour (0xff35b865);
        const auto f = down ? base.darker (0.3f) : over ? base.brighter (0.25f) : base;
        g.setGradientFill (juce::ColourGradient (f.brighter (0.35f), c.getCentreX(), c.getY(), f.darker (0.3f), c.getCentreX(), c.getBottom(), false));
        g.fillEllipse (c);
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.drawEllipse (c, 1.0f);
        const auto k = c.reduced (d * 0.3f);
        juce::Path p;
        if (type == juce::DocumentWindow::closeButton)
        {
            p.startNewSubPath (k.getTopLeft()); p.lineTo (k.getBottomRight());
            p.startNewSubPath (k.getTopRight()); p.lineTo (k.getBottomLeft());
        }
        else if (type == juce::DocumentWindow::minimiseButton) { p.startNewSubPath (k.getX(), k.getCentreY()); p.lineTo (k.getRight(), k.getCentreY()); }
        else p.addRectangle (k);
        g.setColour (juce::Colours::white);
        g.strokePath (p, juce::PathStrokeType (juce::jmax (1.6f, d * 0.11f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    int type;
};
}

juce::Button* HHLookAndFeel::createDocumentWindowButton (int buttonType) { return new TitleBarButton (buttonType); }

void HHLookAndFeel::drawDocumentWindowTitleBar (juce::DocumentWindow& w, juce::Graphics& g, int wd, int h, int titleSpaceX, int titleSpaceW,
                                                const juce::Image*, bool)
{
    g.setGradientFill (juce::ColourGradient (col::panel2.brighter (0.08f), 0.0f, 0.0f, col::bg, 0.0f, (float) h, false));
    g.fillAll();
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.drawHorizontalLine (0, 0.0f, (float) wd);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawHorizontalLine (h - 1, 0.0f, (float) wd);
    g.setColour (juce::Colours::white.withAlpha (w.isActiveWindow() ? 0.92f : 0.55f));
    g.setFont (juce::Font (juce::FontOptions ((float) h * 0.46f)).boldened());
    g.drawText (w.getName(), titleSpaceX + 8, 0, juce::jmax (0, titleSpaceW - 16), h, juce::Justification::centred, true);
}

void HHLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    if (! col::neon || col::lightMode) { LookAndFeel_V4::drawPopupMenuBackground (g, w, h); return; }
    g.setGradientFill (juce::ColourGradient (col::panel2, 0.0f, 0.0f, col::bg, 0.0f, (float) h, false)); g.fillAll();
    g.setColour (col::accent.withAlpha (0.6f)); g.drawRect (0, 0, w, h, 1);
}

juce::Font HHLookAndFeel::getPopupMenuFont()
{
    return phoneUI ? juce::Font (juce::FontOptions (19.0f)) : LookAndFeel_V4::getPopupMenuFont();
}

void HHLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool over, bool down)
{
    auto font = getTextButtonFont (b, b.getHeight());
    juce::ignoreUnused (over, down);
    const int yIndent = juce::jmin (4, b.proportionOfHeight (0.3f)), cornerSize = juce::jmin (b.getHeight(), b.getWidth()) / 2;
    const int fontHeight = juce::roundToInt (font.getHeight() * 0.6f);
    const int leftIndent = juce::jmin (fontHeight, 2 + cornerSize / (b.isConnectedOnLeft() ? 4 : 2));
    const int rightIndent = juce::jmin (fontHeight, 2 + cornerSize / (b.isConnectedOnRight() ? 4 : 2));
    const juce::Rectangle<int> area (leftIndent, yIndent, b.getWidth() - leftIndent - rightIndent, b.getHeight() - yIndent * 2);
    if (area.getWidth() <= 0) return;
    font = fitFont (font, b.getButtonText(), (float) area.getWidth(), area.getHeight() >= font.getHeight() * 2.1f && b.getButtonText().trim().containsChar (' ') ? 2 : 1);
    g.setFont (font);
    if (! col::fontGradient || b.getToggleState() || ! col::isHeadline (font) || b.findColour (juce::TextButton::textColourOffId) != col::text)
    {
        // plain text: never the same color as the button it sits on (custom on-colors, user font color, theme accents)
        const auto bg = b.findColour (b.getToggleState() ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId);
        const auto txt = b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId);
        g.setColour (col::readableOn (txt, bg).withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.5f));
        g.drawFittedText (b.getButtonText(), area, juce::Justification::centred, 2, 1.0f);
        return;
    }
    col::drawWithHalo (g, b.isEnabled() ? 1.0f : 0.5f, [&] (int dx, int dy) { g.drawFittedText (b.getButtonText(), area.translated (dx, dy), juce::Justification::centred, 2, 1.0f); });
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
    if (col::neon && ! col::lightMode)   // Neon 3D: lit groove and a glass fader cap with a glowing line
    {
        g.setColour (juce::Colour (0xff04050a)); g.fillRoundedRectangle (cx - 2.5f, (float) y, 5.0f, (float) h, 2.5f);
        g.setColour (col::accent.withAlpha (0.18f)); g.fillRoundedRectangle (cx - 4.5f, pos, 9.0f, (float) (y + h) - pos, 4.5f);
        g.setGradientFill (juce::ColourGradient (col::accent.brighter (0.3f), 0.0f, pos, col::accent.darker (0.3f), 0.0f, (float) (y + h), false));
        g.fillRoundedRectangle (cx - 2.0f, pos, 4.0f, (float) (y + h) - pos, 2.0f);
        juce::Rectangle<float> cap (cx - 13.0f, pos - 8.0f, 26.0f, 16.0f);
        g.setColour (juce::Colours::black.withAlpha (0.5f)); g.fillRoundedRectangle (cap.translated (0.0f, 2.0f), 4.0f);
        g.setGradientFill (juce::ColourGradient (col::panel2.brighter (0.6f), 0.0f, cap.getY(), col::panel2.darker (0.3f), 0.0f, cap.getBottom(), false));
        g.fillRoundedRectangle (cap, 4.0f);
        g.setColour (col::accent.withAlpha (0.5f)); g.drawRoundedRectangle (cap, 4.0f, 1.0f);
        g.setColour (col::accent.brighter (0.5f)); g.drawLine (cap.getX() + 4, pos, cap.getRight() - 4, pos, 1.5f);
        return;
    }
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
    defValue = def;
    setTextValueSuffix (suffix);
    setPopupDisplayEnabled (true, true, nullptr, 1500);
    setMouseDragSensitivity (180);
}

void Knob::mouseDoubleClick (const juce::MouseEvent&) { showMenu(); }

void Knob::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { showMenu(); return; }
    if (e.mods.isAltDown()) { setValue (defValue, juce::sendNotificationSync); return; }
    swipe.down();
    juce::Slider::mouseDown (e);
}

void Knob::showMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader (getName() + ": " + getTextFromValue (getValue()));
    m.addItem (1, "Type a value...");
    m.addItem (2, "Reset to default (" + getTextFromValue (defValue) + ")");
    m.addSeparator();
    m.addItem (3, "Min"); m.addItem (4, "Middle"); m.addItem (5, "Max");
    if (onAutomate && ! phoneLite) { m.addSeparator(); m.addItem (6, "Automate / LFO / link..."); }
    juce::Component::SafePointer<Knob> self (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [self] (int r) {
        if (self == nullptr || r <= 0) return;
        auto set = [&] (double v) { self->setValue (juce::jlimit (self->getMinimum(), self->getMaximum(), v), juce::sendNotificationSync); if (self != nullptr && self->onDragEnd) self->onDragEnd(); };
        if (r == 1)
            askText (self->getName(), juce::String (self->getValue(), self->getNumDecimalPlacesToDisplay()), [self] (juce::String t) {
                if (self != nullptr && t.trim().isNotEmpty())
                {
                    self->setValue (juce::jlimit (self->getMinimum(), self->getMaximum(), self->getValueFromText (t)), juce::sendNotificationSync);
                    if (self->onDragEnd) self->onDragEnd();
                }
            });
        else if (r == 2) set (self->defValue);
        else if (r == 3) set (self->getMinimum());
        else if (r == 4) set (self->proportionOfLengthToValue (0.5));
        else if (r == 5) set (self->getMaximum());
        else if (r == 6 && self->onAutomate) self->onAutomate();
    });
}

void setupButton (juce::Button& b, const juce::String& text, const juce::String& tooltip, bool toggle)
{
    b.setButtonText (text);
    b.setTooltip (tooltip);
    b.setClickingTogglesState (toggle);
    b.setWantsKeyboardFocus (false);   // Space / Return go to the transport, never re-press the last clicked button
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

// a pattern with notes and no Playlist clip lands on the REC page by itself (own track, bar 1): no dragging it in
static void autoPlacePatterns (Project& p)
{
    for (int i = 0; i < (int) p.patterns.size(); ++i)
    {
        auto& pt = p.patterns[(size_t) i];
        if (pt.autoClip != 0)
            for (auto& c : p.clips)
                if (c.uid == pt.autoClip && c.patternIndex == i && c.patOffset == 0.0 && std::abs (c.length - pt.autoLen) < 1.0e-6)
                    { c.length = pt.autoLen = juce::jmax (c.length, pt.lengthBeats()); break; }
        if (pt.placed) continue;
        bool hasNotes = false;
        for (auto& [id, nl] : pt.notes) hasNotes |= ! nl.empty();
        if (! hasNotes) continue;
        pt.placed = true;
        bool onList = false;
        for (auto& c : p.clips)
            if (c.patternIndex == i)
            {
                if (! onList && c.patOffset == 0.0) { pt.autoClip = c.uid; pt.autoLen = c.length = juce::jmax (c.length, pt.lengthBeats()); }
                onList = true;
            }
        if (onList) continue;
        int lane = -1;
        for (int l = 0; l < (int) p.lanes.size() && lane < 0; ++l)
        {
            const auto& ln = p.lanes[(size_t) l];
            bool used = ln.compLane >= 0 || ln.armed || ln.muted;
            for (auto& c : p.clips) used |= c.lane == l;
            if (! used) lane = l;
        }
        if (lane < 0) { Lane l; l.name = pt.name; p.lanes.push_back (l); lane = (int) p.lanes.size() - 1; }
        else if (p.lanes[(size_t) lane].name.startsWith ("Track ")) p.lanes[(size_t) lane].name = pt.name;
        Clip c; c.uid = p.newUid(); c.lane = lane; c.start = 0.0; c.patternIndex = i; c.length = pt.lengthBeats();
        pt.autoClip = c.uid; pt.autoLen = c.length;
        p.clips.push_back (c);
    }
}

void Studio::commit (const juce::String& what)
{
    autoPlacePatterns (p());
    undo.push (p(), what);
    engine.sync();
    dirty = true;
    if (++editsSinceBackup >= 8 && ! engine.isPlaying()) { editsSinceBackup = 0; engine.storePluginStates(); writeBackupAsync (p()); }   // auto-backup every few edits; never mid-playback (plug-in state + disk write)
    sendChangeMessage();
}

void Studio::undoStep (int delta)
{
    jumpHistory (undo.current() + (delta < 0 ? -1 : 1));
}

// the snapshot is parsed before the audio lock is taken and the old song is freed after it: parsing a big song
// (60 FX, long vocals) under the lock stalled playback = static / lag on undo, redo and history jumps
void Studio::jumpHistory (int index)
{
    if (index < 0 || index >= undo.size() || index == undo.current()) return;
    Project next;
    if (! undo.stateAt (index, next)) return;
    {
        const juce::ScopedLock sl (engine.getLock());
        next.projectFile = p().projectFile;
        std::swap (p(), next);
        undo.setCurrent (index);
    }
    engine.sync(); dirty = true; sendChangeMessage();
}

std::function<void (const juce::String&)>& Studio::readout() { static std::function<void (const juce::String&)> f; return f; }

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
    mBeat = 40, mRecord, mMix, mBrowser, mSamplesUsed, mDetachRack, mDetachPiano, mDetachPlaylist, mDetachMixer, mDetachBrowser, mDJ, mPatchBay,
    mAudio = 60, mPlugins, mAutoBackup, mTheme, mClearBlacklist, mTypingKeys, mMidiInputs,
    mShortcuts = 80, mAbout,
    mMidiDevice = 90,
    mHistory = 1000
};

struct RenderJob : public juce::ThreadWithProgressWindow
{
    struct Item { juce::File file; int track; };
    RenderJob (Engine& e, std::vector<Item> it, bool song, bool stemMode)
        : juce::ThreadWithProgressWindow ("Exporting " + (it.size() == 1 ? it.front().file.getFileName() : juce::String ((int) it.size()) + " stems") + " ...", true, true, -1),
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

// Options > Key commands: every action with its default key; any of them can be given your own key, or reset
struct KeyAction { const char* id; const char* name; juce::KeyPress def; };
static const std::vector<KeyAction>& keyActions()
{
    const int cmd = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier;
    static const std::vector<KeyAction> a {
        { "play", "Play / Stop", juce::KeyPress (juce::KeyPress::spaceKey) },  { "home", "Go to start", juce::KeyPress (juce::KeyPress::homeKey) },
        { "rec", "Record on / off", juce::KeyPress ('r') },                    { "song", "Pattern / Song mode", juce::KeyPress ('l') },
        { "playlist", "Show REC (Playlist)", juce::KeyPress (juce::KeyPress::F5Key) }, { "piano", "Show Piano Roll", juce::KeyPress (juce::KeyPress::F7Key) },
        { "browser", "Browser on / off", juce::KeyPress (juce::KeyPress::F8Key) }, { "mixer", "Show Mixer", juce::KeyPress (juce::KeyPress::F9Key) },
        { "save", "Save", juce::KeyPress ('s', cmd, 0) },                      { "saveas", "Save as", juce::KeyPress ('s', cmd | shift, 0) },
        { "open", "Open", juce::KeyPress ('o', cmd, 0) },                      { "new", "New project", juce::KeyPress ('n', cmd, 0) },
        { "undo", "Undo", juce::KeyPress ('z', cmd, 0) },                      { "redo", "Redo", juce::KeyPress ('z', cmd | shift, 0) },
        { "metro", "Metronome", juce::KeyPress ('m', cmd, 0) },                { "export", "Export WAV", juce::KeyPress ('r', cmd, 0) },
        { "shortcuts", "Shortcuts window", juce::KeyPress ('u', cmd, 0) } };
    return a;
}

// "Set to" another DAW: that DAW's keys for the commands it has; a HumHouse default it would collide with gets no key
struct DawKeys { const char* daw; std::vector<std::pair<const char*, juce::KeyPress>> keys; };
static const std::vector<DawKeys>& dawKeyPresets()
{
    using K = juce::KeyPress;
    const int cmd = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier, alt = juce::ModifierKeys::altModifier;
    static const std::vector<DawKeys> d {
        { "Pro Tools", { { "home", K (K::returnKey) }, { "rec", K (K::F12Key) }, { "mixer", K ('=', cmd, 0) }, { "export", K ('b', cmd | alt, 0) }, { "metro", K ('7') } } },
        { "Logic Pro", { { "home", K (K::returnKey) }, { "rec", K ('r') }, { "mixer", K ('x') }, { "piano", K ('p') }, { "browser", K ('f') }, { "metro", K ('k') } } },   // Logic's Cmd+B bounce is our Cmd+B duplicate: export stays Cmd+R
        { "FL Studio", { { "rec", K ('r') }, { "song", K ('l') }, { "playlist", K (K::F5Key) }, { "piano", K (K::F7Key) }, { "mixer", K (K::F9Key) }, { "metro", K ('m', cmd, 0) }, { "export", K ('r', cmd, 0) } } },
        { "Ableton Live", { { "rec", K (K::F9Key) }, { "song", K (K::tabKey) }, { "mixer", K ('m', cmd | alt, 0) }, { "browser", K ('b', cmd | alt, 0) }, { "export", K ('r', cmd | shift, 0) } } },
        { "Studio One", { { "mixer", K (K::F3Key) }, { "metro", K ('c') }, { "export", K ('e', cmd, 0) } } },
        { "Cubase", { { "mixer", K (K::F3Key) }, { "metro", K ('c') }, { "export", K ('e', cmd, 0) } } },
        { "Reaper", { { "rec", K ('r', cmd, 0) }, { "mixer", K ('m', cmd, 0) }, { "export", K ('r', cmd | alt, 0) }, { "browser", K ('x', cmd | alt, 0) } } },
        { "BandLab", { { "rec", K ('r') }, { "metro", K ('m') } } } };
    return d;
}

struct KeyCmdEditor : juce::Component
{
    KeyCmdEditor (std::map<juce::String, juce::KeyPress>& m, std::function<void()> onSave) : keys (m), save (std::move (onSave))
    {
        setWantsKeyboardFocus (true);
        for (size_t i = 0; i < keyActions().size(); ++i)
        {
            auto* l = names.add (new juce::Label ({}, keyActions()[i].name)); addAndMakeVisible (l);
            auto* b = setBtns.add (new juce::TextButton()); b->setWantsKeyboardFocus (false); addAndMakeVisible (b);
            b->onClick = [this, i] { waiting = (int) i; refresh(); grabKeyboardFocus(); };
            auto* r = resetBtns.add (new juce::TextButton ("Default")); r->setWantsKeyboardFocus (false); addAndMakeVisible (r);
            r->onClick = [this, i] { keys.erase (keyActions()[i].id); waiting = -1; refresh(); save(); };
            auto* c = clearBtns.add (new juce::TextButton ("Clear")); c->setWantsKeyboardFocus (false); addAndMakeVisible (c);
            c->onClick = [this, i] { keys[keyActions()[i].id] = juce::KeyPress(); waiting = -1; refresh(); save(); };
        }
        resetAll.setButtonText ("Reset all to default"); resetAll.setWantsKeyboardFocus (false); addAndMakeVisible (resetAll);
        resetAll.onClick = [this] { keys.clear(); waiting = -1; refresh(); save(); };
        addAndMakeVisible (status);
        addAndMakeVisible (search); search.setTextToShowWhenEmpty ("Search commands...", col::dim);
        setTo.setTextWhenNothingSelected ("Set to: HumHouse");
        setTo.addItem ("Set to: HumHouse (defaults)", 1);
        for (int i = 0; i < (int) dawKeyPresets().size(); ++i) setTo.addItem (juce::String ("Set to: ") + dawKeyPresets()[(size_t) i].daw, i + 2);
        setTo.onChange = [this] { applyDaw (setTo.getSelectedId() - 2); };
        addAndMakeVisible (setTo);
        search.onTextChange = [this] { resized(); };
        refresh();
        setSize (540, 92 + 28 * (int) keyActions().size());
    }
    // someone coming from another DAW gets its key commands (Set to: Pro Tools, Logic, FL, Ableton...)
    void applyDaw (int i)
    {
        for (auto& a : keyActions()) keys.erase (a.id);   // your + ADD SHORTCUT menu keys stay
        if (i >= 0 && i < (int) dawKeyPresets().size())
        {
            const auto& p = dawKeyPresets()[(size_t) i];
            for (auto& [id, k] : p.keys) keys[id] = k;
            for (auto& a : keyActions())
                if (keys.find (a.id) == keys.end())
                    for (auto& [id, k] : p.keys) if (k == a.def) keys[a.id] = juce::KeyPress();   // one key, one command
            int moved = 0;   // an added menu shortcut on one of this DAW's keys loses that key (it would hide the command)
            for (auto& [id, k] : keys)
                if (id.startsWith ("menu:") && k.isValid())
                    for (auto& [pid, pk] : p.keys) if (pk == k) { k = juce::KeyPress(); ++moved; break; }
            status.setText (juce::String (p.daw) + " key commands set." + (moved > 0 ? " " + juce::String (moved) + " of your added shortcuts used a " + p.daw + " key and now has no key." : juce::String())
                            + " Anything without a key: click it to set one.", juce::dontSendNotification);
        }
        else status.setText ("HumHouse key commands set.", juce::dontSendNotification);
        waiting = -1; refresh(); save();
    }
    void refresh()
    {
        for (size_t i = 0; i < keyActions().size(); ++i)
        {
            auto& a = keyActions()[i]; const auto it = keys.find (a.id);
            const auto k = it != keys.end() ? it->second : a.def;
            setBtns[(int) i]->setButtonText (waiting == (int) i ? "Press a key... (Esc = cancel)" : ! k.isValid() ? juce::String ("(no key) - click to set") : k.getTextDescriptionWithIcons() + (it != keys.end() ? "  (yours)" : ""));
        }
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        { auto top = r.removeFromTop (30); setTo.setBounds (top.removeFromRight (200).reduced (2)); search.setBounds (top.reduced (2)); }
        const auto q = search.getText().trim().toLowerCase();
        for (int i = 0; i < names.size(); ++i)
        {
            const bool show = q.isEmpty() || names[i]->getText().toLowerCase().contains (q) || setBtns[i]->getButtonText().toLowerCase().contains (q);
            for (auto* c : std::initializer_list<juce::Component*> { names[i], resetBtns[i], clearBtns[i], setBtns[i] }) c->setVisible (show);
            if (! show) continue;
            auto row = r.removeFromTop (28);
            names[i]->setBounds (row.removeFromLeft (170)); resetBtns[i]->setBounds (row.removeFromRight (72).reduced (2));
            clearBtns[i]->setBounds (row.removeFromRight (60).reduced (2)); setBtns[i]->setBounds (row.reduced (2));
        }
        resetAll.setBounds (r.removeFromTop (28).reduced (2));
        status.setBounds (r.removeFromTop (28));
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (waiting < 0) return false;
        if (k != juce::KeyPress (juce::KeyPress::escapeKey))
        {
            auto& a = keyActions()[(size_t) waiting];
            const KeyAction* taken = nullptr;   // one key, one command: a key another command already uses is refused
            for (auto& b : keyActions())
                if (juce::String (b.id) != a.id) { const auto it = keys.find (b.id); if ((it != keys.end() ? it->second : b.def) == k) taken = &b; }
            if (taken != nullptr) status.setText (k.getTextDescriptionWithIcons() + " is already " + taken->name + ". Change that one first.", juce::dontSendNotification);
            else { status.setText ({}, juce::dontSendNotification); if (k == a.def) keys.erase (a.id); else keys[a.id] = k; save(); }
        }
        waiting = -1; refresh(); return true;
    }
    std::map<juce::String, juce::KeyPress>& keys; std::function<void()> save;
    juce::OwnedArray<juce::Label> names; juce::OwnedArray<juce::TextButton> setBtns, resetBtns, clearBtns; juce::TextButton resetAll; juce::TextEditor search; juce::ComboBox setTo; juce::Label status; int waiting = -1;
};

// phone: menus taller than the screen scroll with a finger (JUCE's popup menu only scrolls by holding its edge arrows)
struct PhoneMenuList : juce::Component
{
    struct Row { juce::String text; int id = 0; bool header = false, enabled = true, ticked = false; };
    static void flatten (const juce::PopupMenu& m, std::vector<Row>& out)
    {
        for (juce::PopupMenu::MenuItemIterator it (m); it.next();)
        {
            const auto& i = it.getItem();
            if (i.isSeparator || i.text.isEmpty()) continue;
            if (i.subMenu != nullptr) { out.push_back ({ i.text, 0, true, true, false }); flatten (*i.subMenu, out); continue; }
            out.push_back ({ i.text, i.itemID, i.isSectionHeader, i.isEnabled, i.isTicked });
        }
    }
    struct Body : juce::Component
    {
        PhoneMenuList& o;
        explicit Body (PhoneMenuList& m) : o (m) {}
        void paint (juce::Graphics& g) override
        {
            const auto clip = g.getClipBounds();
            for (int i = juce::jmax (0, clip.getY() / rowH); i < (int) o.rows.size() && i * rowH < clip.getBottom(); ++i)
            {
                const auto& r = o.rows[(size_t) i];
                auto a = juce::Rectangle<int> (0, i * rowH, getWidth(), rowH);
                if (r.header) { g.setColour (col::accent); g.setFont (juce::FontOptions (15.0f, juce::Font::bold)); }
                else { g.setColour (r.enabled ? col::text : col::dim); g.setFont (juce::FontOptions (18.0f)); }
                if (o.canDelete && ! r.header && o.canDelete (r.id))   // X: delete this song
                {
                    auto x = a.removeFromRight (rowH).reduced (8);
                    g.setColour (juce::Colour (0xffd04a5a)); g.drawRoundedRectangle (x.toFloat(), 6.0f, 1.5f);
                    g.setFont (juce::FontOptions (18.0f, juce::Font::bold)); g.drawText ("X", x, juce::Justification::centred);
                    g.setColour (r.enabled ? col::text : col::dim); g.setFont (juce::FontOptions (18.0f));
                }
                g.drawText ((r.ticked ? juce::String (juce::CharPointer_UTF8 ("\xe2\x9c\x93 ")) : juce::String()) + r.text, a.reduced (16, 0), juce::Justification::centredLeft, true);
                g.setColour (col::gridBar); g.fillRect (a.removeFromBottom (1));
            }
        }
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (e.mouseWasDraggedSinceMouseDown()) return;   // that was a scroll
            const int i = e.y / rowH;
            if (i < 0 || i >= (int) o.rows.size()) return;
            const auto& r = o.rows[(size_t) i];
            if (o.canDelete && o.onDelete && ! r.header && o.canDelete (r.id) && e.x >= getWidth() - rowH)
            {
                const int id = r.id;
                juce::Component::SafePointer<Body> sp (this);
                o.onDelete (id, [sp, id] { if (sp == nullptr) return; auto& rs = sp->o.rows; rs.erase (std::remove_if (rs.begin(), rs.end(), [id] (const Row& x) { return x.id == id; }), rs.end()); sp->o.resized(); sp->repaint(); });
                return;
            }
            if (! r.header && r.enabled && r.id != 0 && o.done) o.done (r.id);
        }
    };
    static constexpr int rowH = 46;
    std::vector<Row> rows;
    std::function<void (int)> done;
    std::function<bool (int)> canDelete;                                   // rows with an X on the right
    std::function<void (int, std::function<void()>)> onDelete;            // id, remove-the-row callback
    Body body { *this };
    juce::Viewport view;
    juce::TextButton close { "X  CLOSE" };
    explicit PhoneMenuList (const juce::PopupMenu& m)
    {
        flatten (m, rows);
        setOpaque (true);
        addAndMakeVisible (view); view.setViewedComponent (&body, false);
        view.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::all); view.setScrollBarsShown (true, false);
        addAndMakeVisible (close); close.onClick = [this] { if (done) done (0); };
    }
    void paint (juce::Graphics& g) override { g.fillAll (col::panel); }
    void resized() override
    {
        auto r = getLocalBounds();
        close.setBounds (r.removeFromTop (48).reduced (6));
        view.setBounds (r);
        body.setSize (r.getWidth() - view.getScrollBarThickness(), (int) rows.size() * rowH);
    }
};

class MainComponent : public juce::Component, public juce::DragAndDropContainer, public juce::FileDragAndDropTarget, public juce::MenuBarModel, private juce::Timer, private juce::ChangeListener
{
public:
    MainComponent()
    {
        juce::Desktop::getInstance().addGlobalMouseListener (&previewStopper);
        if (phoneUI) juce::Desktop::getInstance().addGlobalMouseListener (&doubleTapMenu);
        openPianoMain = [this] (int ch) { studio.selectedChannel = ch; rackHidden = false; setView (0); studio.sendChangeMessage(); };
        juce::PropertiesFile::Options o;
        o.applicationName = "HumHouse Studio";
        o.filenameSuffix = ".settings";
        o.folderName = "HumHouse Studio";
        o.osxLibrarySubFolder = "Application Support";
        props.setStorageParameters (o);
        studio.settings = props.getUserSettings();
        applyLanguage (studio.settings != nullptr ? studio.settings->getIntValue ("language", 0) : 0);
        kbState.addListener (&kbListen);

        if (studio.settings->containsKey ("uiScale"))
            juce::Desktop::getInstance().setGlobalScaleFactor ((float) juce::jlimit (0.75, phoneUI ? 1.5 : 2.0, studio.settings->getDoubleValue ("uiScale", 1.0)));   // phone: 150% max, bigger never fits
        col::lightMode = studio.settings->getBoolValue ("themeLight", false);
        col::neon = studio.settings->getBoolValue ("themeNeon", true);
        playheadLock() = studio.settings->getBoolValue ("playheadLock", false);
        engine.setDualOutput (studio.settings->getBoolValue ("dualOut", false));
        engine.pluginSandbox = false;   // crash protection removed: plug-ins always load directly
       #if JUCE_IOS
        routeWatch = hhsWatchPhoneRoute (engine.speakerOut);
       #endif
       #if HHS_STORE
        phoneLite = ! hhsStoreStart ([safe = juce::Component::SafePointer<MainComponent> (this)] (bool full, std::string msg) {
            if (safe == nullptr) return;
            if (phoneLite == full) { phoneLite = ! full; safe->relayoutAll (*safe); }
            if (safe->welcome != nullptr) safe->welcome->fill();   // the price in the buyer's currency
            if (! msg.empty()) juce::AlertWindow::showAsync (juce::MessageBoxOptions().withTitle ("HumHouse Pro").withMessage (juce::String (msg)).withButton ("OK"), nullptr);
        });
        if (phoneUI && ! studio.settings->getBoolValue ("welcomeOff", false)) studio.settings->setValue ("proOffered", true);   // the Welcome page shows Pro
        if (phoneLite && ! studio.settings->getBoolValue ("proOffered", false))   // first launch: show what Full adds, once
        {
            studio.settings->setValue ("proOffered", true);
            juce::Timer::callAfterDelay (2500, [safe = juce::Component::SafePointer<MainComponent> (this)] { if (safe != nullptr) safe->showUnlock(); });
        }
       #endif
        if (phoneUI && ! studio.settings->getBoolValue ("welcomeOff", false))
        {
            welcome = std::make_unique<PhoneWelcome>();
            welcome->onDone = [this, safe = juce::Component::SafePointer<MainComponent> (this)] (bool off) {
                if (off) studio.settings->setValue ("welcomeOff", true);
                juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->welcome = nullptr; });
            };
            addAndMakeVisible (*welcome);
            welcome->setBounds (getLocalBounds());
        }
        engine.lowLatency = studio.settings->getBoolValue ("lowLatency", true);   // No Latency Policy: on by default, no right-click steps to record on time
        packAudioInSong() = studio.settings->getBoolValue ("packAudioInSong", phoneUI);
        if (! phoneUI) if (const juce::File bgf (studio.settings->getValue ("bgPicture")); bgf.existsAsFile()) bgPicture = juce::ImageFileFormat::loadFrom (bgf);   // phone: songs carry every sample, one-shot and clip inside, so they open exactly on a computer
        engine.recordOffsetMs = (float) studio.settings->getDoubleValue ("recordOffsetMs", 0.0);
        engine.ramCache = studio.settings->getBoolValue ("ramCache", true);
        col::fontColour = juce::Colour::fromString (studio.settings->getValue ("fontColour", "00000000"));
        if (! studio.settings->containsKey ("themeColour") && (col::lightMode || col::fontColour.getAlpha() > 0)) applyTheme (col::accent, 0.5f, false);
        if (! studio.settings->getBoolValue ("purpleDefault", false))   // once: an orange saved by older builds becomes the purple default
        {
            const auto old = juce::Colour::fromString (studio.settings->getValue ("themeColour", "ff000000"));
            if (studio.settings->containsKey ("themeColour") && old.getSaturation() > 0.4f && old.getHue() > 0.02f && old.getHue() < 0.13f)
                studio.settings->removeValue ("themeColour");
            studio.settings->setValue ("purpleDefault", true);
        }
        if (studio.settings->containsKey ("themeColour"))
            applyTheme (juce::Colour::fromString (studio.settings->getValue ("themeColour")), (float) studio.settings->getDoubleValue ("themeTint", 0.5), studio.settings->getBoolValue ("themeBlack", false));
        if (! studio.settings->containsKey ("themeColour")) applyTheme (juce::Colour (0xff9b5cff), 0.15f, false);   // default look: HumHouse purple
        if (studio.settings->containsKey ("gradColours")) applyGradient (studio.settings->getValue ("gradColours"));
        if (studio.settings->containsKey ("fontGrad")) applyFontGradient (studio.settings->getValue ("fontGrad"));
        applySavedLayout (studio);
        auto audioXml = studio.settings->getXmlValue ("audio");
        engine.noLatencyDriver = studio.settings->getBoolValue ("noLatencyDriver", true);
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
        lanes = createLanes (studio);
        for (auto* c : { rack.get(), piano.get(), playlist.get(), mixer.get(), browser.get(), lanes.get() }) addChildComponent (c);
        addChildComponent (pianoSplit);
        pianoSplit.onDrag = [this] (int y) {
            if (splitTotal <= 0) return;
            pianoMin = y > splitTop + splitTotal - 70;
            if (build::rackBelow && ! phoneUI)   // rack under the Piano Roll: the split works upside down
            {
                pianoMin = y < splitTop + 70;
                if (! pianoMin) rackFrac = juce::jlimit (0.15f, 0.9f, (float) (splitTop + splitTotal - y) / (float) splitTotal);
            }
            else if (! pianoMin) rackFrac = juce::jlimit (0.15f, 0.9f, (float) (y - splitTop) / (float) splitTotal);
            resized(); };
        pianoSplit.onToggle = [this] { pianoMin = ! pianoMin; resized(); };

        titleLabel.setText ("HUMHOUSE", juce::dontSendNotification);
        titleLabel.setFont (juce::FontOptions (19.0f, juce::Font::bold));
        titleLabel.setColour (juce::Label::textColourId, col::accent);

        int i = 0;
        for (auto* t : { &beatTab, &recordTab, &mixTab, &patchTab, &videoTab })
        {
            static const char* tips[] { "Beat: Channel Rack + Piano Roll (F6 / F7)", "Record: Playlist - arm tracks, record vocals, arrange (F5)",
                                        "Mix: Mixer - inserts, FX, click-to-route (F9)", "Patch Bay: node bays, live while it plays (right-click / double-tap to add)",
                                        "Video: score to picture, synced to the playhead" };
            t->setTooltip (tips[i]);
            t->setRadioGroupId (100);
            t->setClickingTogglesState (true);
            t->setWantsKeyboardFocus (false);   // a tab never keeps focus: keys from a plug-in window (Space, Return) can't flip to VIDEO
            const int v = i++;
            t->onClick = [this, v] { setView (v); };
            addAndMakeVisible (t);
        }
        if (phoneUI)   // phone: one PAGE folder instead of 4 tabs
        {
            setupButton (pagesBtn, "PAGE: REC", "Pages: Beat, Record, Mix, Patch Bay");
            pagesBtn.onClick = [this]
            {
                juce::PopupMenu m;
                const char* names[] { "BEAT  (Channel Rack + Piano Roll)", "RECORD  (Playlist)", "MIX  (Mixer)", "PATCH  (Patch Bay)" };
                for (int k = 0; k < 4; ++k) if (! (phoneLite && k == 3)) m.addItem (k + 1, names[k], true, view == k);
                m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&pagesBtn), [this] (int r) { if (r > 0) setView (r - 1); });
            };
            addAndMakeVisible (pagesBtn);
            // BEAT / REC / MIX: one tap each (owner: no page folder for these three); PAGE keeps Patch Bay
            const char* pg[] { "BEAT", "REC", "MIX" };
            for (int k = 0; k < 3; ++k)
            {
                auto& b = k == 0 ? beatPg : k == 1 ? recPg : mixPg;
                setupButton (b, pg[k], juce::String ("Go to ") + pg[k], false);
                b.setColour (juce::TextButton::buttonOnColourId, col::accent);
                b.onClick = [this, k] { setView (k); };
                addAndMakeVisible (b);
            }
            // BandLab-style: selecting clips pops SELECT / TRASH / STAMP (copy) / PAINT (paste) right above them
            setupButton (cbSelect, "SELECT", "Select tool"); cbSelect.onClick = [this] { if (playlist != nullptr) playlist->keyPressed (juce::KeyPress ('e')); };
            setupButton (cbTrash, "TRASH", "Delete the selected clips"); cbTrash.onClick = [this] { phoneDelete(); };
            setupButton (cbStamp, "STAMP", "Copy the selected clips (then tap where they go and PAINT)"); cbStamp.onClick = [this] { phoneCopyPaste ('c'); };
            setupButton (cbPaint, "PAINT", "Paste what you stamped"); cbPaint.onClick = [this] { phoneCopyPaste ('v'); };
            for (auto* b : { &cbSelect, &cbTrash, &cbStamp, &cbPaint }) addChildComponent (b);
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
        setupButton (retroBtn, "RETRO", "Retro Record: drops the last 60 s you sang or played onto REC, even if nothing was armed or recording", false);
        setupButton (pianoBtn, "PIANO", "Keyboard inset: play the selected channel or sample on keys to hear it before placing", true);
        pianoBtn.onClick = [this]
        {
            if (kbInset == nullptr)
            {
                kbInset = std::make_unique<juce::MidiKeyboardComponent> (kbState, juce::MidiKeyboardComponent::horizontalKeyboard);
                kbInset->setAvailableRange (24, 108);
                kbInset->setKeyWidth (phoneUI ? 34.0f : 22.0f);
                kbInset->setLowestVisibleKey (48);
                kbInset->setWantsKeyboardFocus (false);
                kbInset->setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, col::accent.withAlpha (0.7f));
                kbInset->setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, col::accent.withAlpha (0.25f));
                addChildComponent (*kbInset);
            }
            if (! pianoBtn.getToggleState()) kbState.allNotesOff (1);
            resized();
        };
        setupButton (duoBtn, "DUO", "Split screen: the page on top, a strip below that switches between KEYS, PADS and a mini MIXER", true);
        duoBtn.setToggleState (studio.settings->getBoolValue ("duo", false), juce::dontSendNotification);
        duoBtn.onClick = [this]
        {
            studio.settings->setValue ("duo", duoBtn.getToggleState());
            if (duoBtn.getToggleState() && duo == nullptr) { duo = std::make_unique<DuoStrip> (*this); addChildComponent (*duo); }
            if (! duoBtn.getToggleState()) kbState.allNotesOff (1);
            resized();
        };
        if (phoneUI && duoBtn.getToggleState()) { duo = std::make_unique<DuoStrip> (*this); addChildComponent (*duo); }
        retroBtn.onClick = [this] { flash (engine.retroRecord() ? "Retro Record: last take added to REC" : "Retro Record: nothing heard on the input yet"); };
        setupButton (browserBtn, "PLUGIN HOUSE", "Show / hide Plugin House and the sample + kit browser (F8)", true);
        browserBtn.setToggleState (true, juce::dontSendNotification);
        browserBtn.onClick = [this] { showBrowser = browserBtn.getToggleState(); resized(); };
        setupButton (laneBtn, "LANE", "LANE: FX lanes + automation lanes of the selected mixer track (Ableton style). Click again to collapse", true);
        laneBtn.onClick = [this] { showLanes = laneBtn.getToggleState(); if (popouts.count (lanes.get()) > 0) reattach (lanes.get()); resized(); };
        setupButton (lanePop, "POP", "Detach the LANE panel into its own window (click again to dock it)");
        lanePop.onClick = [this] { if (! showLanes) laneBtn.setToggleState (true, juce::sendNotificationSync); toggleDetach (lanes.get(), "Lanes"); };
        addAndMakeVisible (laneBtn); addChildComponent (lanePop);
        if (phoneUI)
        {
            browserBtn.setToggleState (showBrowser, juce::dontSendNotification);
            browserTab.setTooltip ("Samples / Patterns / Plugin House: tap to bring it out or tuck it back in");
            browserTab.onClick = [this] { browserBtn.setToggleState (! showBrowser, juce::sendNotificationSync); };
            addAndMakeVisible (browserTab);
        }
        setupButton (themeBtn, "COLOR", "Color wheel: change the overall color of the DAW");
        themeBtn.onClick = [this] { showThemeDialog (studio); };
        setupButton (scopeBtn, "SCOPE", "Scope: oscilloscope, scrolling waveform and spectrum of the master (pops out, resizable)");
        scopeBtn.onClick = [this] { openScopeWindow (studio); };
        setupButton (vidPopBtn, "VID POP", "VID POP: pop out the video so you see the picture while you record or add audio to it", false);
        vidPopBtn.onClick = [this]
        {
            if (videoView == nullptr) { videoView = createVideoView (studio); addChildComponent (*videoView); }
            if (videoPopHook()) videoPopHook()(); else setView (4);
        };
        addChildComponent (vidPopBtn);
        addAndMakeVisible (scopeBtn);
        setupButton (mixPopBtn, {}, "Mixer: pop it out into its own window so it stays visible on Beat and Rec (click again to dock it)", true); mixPopBtn.kind = 4;
        mixPopBtn.onClick = [this] { if (phoneUI) { phoneMixToggle(); return; } if ((popouts.count (mixer.get()) > 0) != mixPopBtn.getToggleState()) toggleDetach (mixer.get(), "Mixer"); mixPopBtn.setToggleState (popouts.count (mixer.get()) > 0, juce::dontSendNotification); };
        addAndMakeVisible (mixPopBtn);
        setupButton (rackPopBtn, "RACK", "Channel Rack: pop it out into its own window so you can edit samples and one-shots on Rec and in full screen (click again to dock it)", true);
        rackPopBtn.onClick = [this] { if (phoneUI) { phoneRackToggle(); return; } if ((popouts.count (rack.get()) > 0) != rackPopBtn.getToggleState()) toggleDetach (rack.get(), "Channel Rack"); rackPopBtn.setToggleState (popouts.count (rack.get()) > 0, juce::dontSendNotification); };
        addAndMakeVisible (rackPopBtn);
        rackPopOut = [this] { if (phoneUI) { phoneRackToggle(); return; } toggleDetach (rack.get(), "Channel Rack"); rackPopBtn.setToggleState (popouts.count (rack.get()) > 0, juce::dontSendNotification); rack->resized(); };
        setupButton (saveBtn, "SAVE", "Save the project (Cmd/Ctrl+S). Flashes SAVED when it's written", false);
        saveBtn.onClick = [this] { save (false); };
        addAndMakeVisible (saveBtn);
        for (juce::Button* b : { (juce::Button*) &songBtn, (juce::Button*) &startBtn, (juce::Button*) &playBtn, (juce::Button*) &stopBtn, (juce::Button*) &recBtn, (juce::Button*) &metroBtn, (juce::Button*) &countBtn, (juce::Button*) &retroBtn, (juce::Button*) &pianoBtn, (juce::Button*) &browserBtn, (juce::Button*) &themeBtn }) addAndMakeVisible (b);
        addChildComponent (duoBtn);   // phone only: the phone top bar shows it

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
        Studio::readout() = [hb = juce::Component::SafePointer<juce::Label> (&hintBar)] (const juce::String& t) { if (hb != nullptr) hb->setText (t, juce::dontSendNotification); };

        hostKeyHandler = [this] (const juce::KeyPress& k) { return keyPressed (k); };
        for (auto& kv : juce::StringArray::fromTokens (studio.settings->getValue ("keymap"), ";", ""))
            if (kv.contains ("=")) userKeys[kv.upToFirstOccurrenceOf ("=", false, false)] = juce::KeyPress::createFromDescription (kv.fromFirstOccurrenceOf ("=", false, false));
        relayoutMain = [sp = juce::Component::SafePointer<MainComponent> (this)] { if (sp != nullptr) { sp->resized(); sp->repaint(); } };
        lowMemoryMain = [sp = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (sp == nullptr) return;
            sp->studio.undo.trimTo (20);
            sp->engine.releaseMemory();
            if (sp->studio.dirty) { sp->engine.storePluginStates(); writeBackupAsync (sp->engine.project()); }
        };
        saveBackupMain = [sp = juce::Component::SafePointer<MainComponent> (this)]
        { if (sp != nullptr && sp->studio.dirty) { sp->engine.storePluginStates(); writeBackup (sp->engine.project()); } };   // finish before iOS may kill us
        applyKeySet = [this] (const juce::String& set)   // Layout: the matching DAW's key commands (none of them clash with another command)
        {
            const int cmd = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier, alt = juce::ModifierKeys::altModifier;
            userKeys.clear();
            auto put = [this] (const char* id, juce::KeyPress k) { userKeys[id] = k; };
            if (set == "Logic")
            {
                put ("metro", juce::KeyPress ('k')); put ("mixer", juce::KeyPress ('x')); put ("piano", juce::KeyPress ('p'));
                put ("browser", juce::KeyPress ('f')); put ("home", juce::KeyPress (juce::KeyPress::returnKey)); put ("export", juce::KeyPress ('b', cmd, 0));
            }
            else if (set == "Pro Tools")
            {
                put ("rec", juce::KeyPress (juce::KeyPress::F12Key)); put ("home", juce::KeyPress (juce::KeyPress::returnKey));
                put ("mixer", juce::KeyPress ('=', cmd, 0)); put ("export", juce::KeyPress ('b', cmd | alt, 0));
            }
            else if (set == "Reaper")
            {
                put ("rec", juce::KeyPress ('r', cmd, 0)); put ("export", juce::KeyPress ('r', cmd | alt, 0)); put ("mixer", juce::KeyPress ('m', cmd, 0));
                put ("metro", juce::KeyPress ('m', cmd | shift, 0)); put ("browser", juce::KeyPress ('x', cmd | alt, 0)); put ("home", juce::KeyPress ('w'));
            }
            else if (set == "Ableton")
            {
                put ("browser", juce::KeyPress ('b', cmd | alt, 0)); put ("mixer", juce::KeyPress ('m', cmd | alt, 0));
            }
            studio.settings->setValue ("layoutKeySet", set);
            saveKeys();
        };
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
            dropBtn.setColour (juce::TextButton::buttonColourId, col::accent);
            dropBtn.onClick = [this] { dropBtn.setVisible (false); phoneCopyPaste ('v'); };
            addChildComponent (dropBtn);
            juce::Component::SafePointer<MainComponent> safeTap (this);
            phoneTapHook = [safeTap] (juce::Component* c, juce::Point<int> p)
            {
                auto* m = safeTap.getComponent();
                if (m == nullptr || ! m->clipArmed || c != (m->clipView == 1 ? m->playlist.get() : m->piano.get()) || m->view != m->clipView) return false;
                const auto q = m->getLocalPoint (c, p);
                m->dropBtn.setBounds (juce::Rectangle<int> (190, 48).withCentre (q.translated (0, -44)).constrainedWithin (m->getLocalBounds()));
                m->dropBtn.setVisible (true); m->dropBtn.toFront (false);
                m->hintBar.setText ("Tap CLIPBOARD to drop it here (or tap another spot). X stops copying.", juce::dontSendNotification);
                return true;
            };
            altABtn.setTooltip ("Alt+A: rolls / arpeggiator on the selected notes (BEAT) or audio clips (REC)");
            altSBtn.setTooltip ("Alt+S: strum the selected notes");
            undoBtn.onClick = [this] { studio.undoStep (-1); };
            redoBtn.onClick = [this] { studio.undoStep (1); };
            altABtn.onClick = [this] { phoneAlt ('a'); };
            dupBtn.setTooltip ("Cmd+B: duplicate the selected notes (BEAT) or clips (REC) right after themselves");
            dupBtn.onClick = [this] { phoneDuplicate(); };
            delBtn.setTooltip ("Delete the selected clips (REC) or notes (BEAT)");
            delBtn.onClick = [this] { phoneDelete(); };
            octUpBtn.setTooltip ("Octave up: the selected notes (all notes if none are selected)");
            octDnBtn.setTooltip ("Octave down: the selected notes (all notes if none are selected)");
            noteUpBtn.setTooltip ("Up one note: the selected notes (all notes if none are selected)");
            noteDnBtn.setTooltip ("Down one note: the selected notes (all notes if none are selected)");
            octUpBtn.onClick  = [this] { phoneTranspose (juce::KeyPress::upKey, juce::ModifierKeys::commandModifier); };
            octDnBtn.onClick  = [this] { phoneTranspose (juce::KeyPress::downKey, juce::ModifierKeys::commandModifier); };
            noteUpBtn.onClick = [this] { phoneTranspose (juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier); };
            noteDnBtn.onClick = [this] { phoneTranspose (juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier); };
            altSBtn.onClick = [this] { phoneAlt ('s'); };
            for (auto* b : { &closeAllBtn, &menuBtn, &importBtn, &copyBtn, &pasteBtn, &undoBtn, &redoBtn, &altABtn, &altSBtn, &dupBtn, &delBtn, &octUpBtn, &octDnBtn, &noteUpBtn, &noteDnBtn }) addAndMakeVisible (b);
            // folders: one tap opens a row of their buttons under the top bar, another tap tucks it away
            browserBtn.setButtonText ("SOUNDS");
            toolsOpen = studio.settings->getBoolValue ("phoneTools", true);
            keysOpen = studio.settings->getBoolValue ("phoneKeys", false);
            moreOpen = studio.settings->getBoolValue ("phoneMore", false);
            const juce::String down (juce::CharPointer_UTF8 (" \xe2\x96\xbe"));
            toolsBtn.setButtonText ("TOOLS" + down); keysBtn.setButtonText ("PIANO ROLL" + down); moreBtn.setButtonText ("MORE" + down);
            toolsBtn.setTooltip ("Copy, paste, undo, redo, delete, duplicate, Alt+A, Alt+S");
            keysBtn.setTooltip ("Octave and one-note up / down for the selected notes");
            moreBtn.setTooltip ("Metronome, count-in, mixer, scope, color, CPU");
            auto folder = [this] (juce::TextButton& b, bool& open, const char* key)
            {
                b.setColour (juce::TextButton::buttonOnColourId, col::accent);
                b.setToggleState (open, juce::dontSendNotification);
                b.onClick = [this, &b, &open, key] { open = ! open; b.setToggleState (open, juce::dontSendNotification); studio.settings->setValue (key, open); resized(); };
                addAndMakeVisible (b);
            };
            folder (toolsBtn, toolsOpen, "phoneTools"); folder (keysBtn, keysOpen, "phoneKeys"); folder (moreBtn, moreOpen, "phoneMore");
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
        if (auto* e = std::getenv ("HHS_PAINTBENCH"))   // dev: after N s, time the paint of every visible view -> paintcost.txt
            juce::Timer::callAfterDelay (juce::jmax (1, std::atoi (e)) * 1000, [] { writePaintCost(); });
        startTimerHz (30);
    }

    ~MainComponent() override
    {
       #if JUCE_IOS
        hhsUnwatchPhoneRoute (routeWatch);
       #endif
        closeDjDecks(); closePatchBay();
        relayoutMain = nullptr; lowMemoryMain = nullptr; saveBackupMain = nullptr;
        miniKb = nullptr;
        kbState.removeListener (&kbListen);
        rackPopOut = nullptr;
        juce::Desktop::getInstance().removeGlobalMouseListener (&previewStopper);
        juce::Desktop::getInstance().removeGlobalMouseListener (&doubleTapMenu);
        openPianoMain = nullptr; patchView = nullptr; videoView = nullptr;
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
        if (bgPicture.isValid())   // your picture behind the UI (fills the window, cropped to fit); the toolbar paints over it
        {
            // No Hitch: scaled once to the window's pixels, then every repaint (clock, meters, buttons) is a plain copy,
            // never a full-size photo resampled 60x a second
            const float sc = g.getInternalContext().getPhysicalPixelScaleFactor();
            const int pw = juce::roundToInt (getWidth() * sc), ph = juce::roundToInt (getHeight() * sc);
            if (pw > 0 && ph > 0 && (bgScaled.getWidth() != pw || bgScaled.getHeight() != ph || bgScaledFrom != bgPicture))
            {
                bgScaled = juce::Image (juce::Image::ARGB, pw, ph, true);   // keeps a PNG / GIF's see-through parts
                juce::Graphics bg (bgScaled);
                bg.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
                bg.drawImageWithin (bgPicture, 0, 0, pw, ph, juce::RectanglePlacement::fillDestination);
                bgScaledFrom = bgPicture;
            }
            if (bgScaled.isValid()) g.drawImage (bgScaled, 0, 0, getWidth(), getHeight(), 0, 0, bgScaled.getWidth(), bgScaled.getHeight());
        }
        if (col::neon && ! col::lightMode) col::metal (g, topBar.toFloat(), 0.0f, 0.02f);   // Neon 3D iron: brushed gunmetal top bar
        else { g.setColour (col::panel); g.fillRect (topBar); }
        if (! logoArea.isEmpty()) drawLogo (g, logoArea.toFloat(), true);
    }

    void paintOverChildren (juce::Graphics& g) override   // Neon 3D: every page panel sits in a thin lit frame (strokes only, clipped to what repaints)
    {
        if (! col::neon || col::lightMode) return;
        if (phoneMenu != nullptr) return;   // phone menu covers the page: no Channel Rack / panel frame drawn over it
        const auto lit = topBar.getBottom();
        {
            juce::ColourGradient line (col::accent.withAlpha (0.0f), (float) topBar.getX(), 0.0f, col::accent.withAlpha (0.0f), (float) topBar.getRight(), 0.0f, false);
            line.addColour (0.5, col::accent.withAlpha (0.8f));
            g.setGradientFill (line); g.fillRect (topBar.getX(), lit - 1, topBar.getWidth(), 1);
        }
        for (auto* c : { browser.get(), playlist.get(), mixer.get(), rack.get(), piano.get(), lanes.get(), patchView.get(), videoView.get() })
        {
            if (c == nullptr || ! c->isVisible() || c->getParentComponent() != this || c->getWidth() < 40 || c->getHeight() < 40) continue;
            const auto b = c->getBounds().toFloat().reduced (0.5f);
            const auto clip = g.getClipBounds().toFloat();
            if (! clip.intersects (b.expanded (2.0f)) || b.reduced (4.0f).contains (clip)) continue;   // a repaint inside the panel never touches its frame
            g.setColour (col::accent.withAlpha (0.16f)); g.drawRoundedRectangle (b.reduced (1.0f), 6.0f, 3.0f);
            g.setColour (col::accent.withAlpha (0.55f)); g.drawRoundedRectangle (b, 6.0f, 1.0f);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (phoneMenu != nullptr) phoneMenu->setBounds (phoneSafe (r));   // phone turned while the menu is open
        if (welcome != nullptr) { welcome->setBounds (r); welcome->toFront (false); }
if (phoneUI)
        {
            if (auto* d = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds()))
                r = d->safeAreaInsets.subtractedFrom (r);   // below the notch / status bar
            r.removeFromTop (6);   // phone: the same controls, wrapped into as many rows as the width needs (portrait or landscape)
            videoTab.setVisible (false);   // phone: no VIDEO page
            for (auto* t : { &beatTab, &recordTab, &mixTab, &patchTab }) t->setVisible (false);   // they live in the PAGE folder
            // top: menu, logo, transport / pages, import, save / BPM, position, folders; open folders add a row each
            constexpr int brk = -1;
            std::vector<std::pair<juce::Component*, int>> items {
                { &menuBtn, 70 }, { &beatPg, 58 }, { &recPg, 52 }, { &mixPg, 52 }, { &pagesBtn, 64 }, { &songBtn, 56 }, { &saveBtn, 80 }, { &browserBtn, 96 }, { nullptr, brk },
                { &toolsBtn, 92 }, { &keysBtn, 100 }, { &moreBtn, 86 } };   // 2 rows: everything else sits in the folders, transport at the bottom
            if (toolsOpen) items.insert (items.end(), { { &retroBtn, 80 }, { &copyBtn, 92 }, { &pasteBtn, 76 }, { &undoBtn, 76 }, { &redoBtn, 76 }, { &delBtn, 86 }, { &dupBtn, 80 }, { &altABtn, 74 }, { &altSBtn, 74 } });
            if (keysOpen) items.insert (items.end(), { { &duoBtn, 64 }, { &pianoBtn, 80 }, { &octDnBtn, 70 }, { &octUpBtn, 70 }, { &noteDnBtn, 52 }, { &noteUpBtn, 52 } });
            if (moreOpen) items.insert (items.end(), { { &closeAllBtn, 46 }, { &rackPopBtn, 66 }, { &importBtn, 96 }, { &bpm, 110 }, { &posLabel, 110 }, { &metroBtn, 72 }, { &countBtn, 92 }, { &mixPopBtn, 50 }, { &scopeBtn, 76 }, { &themeBtn, 80 }, { &cpuLabel, 92 } });
            if (phoneLite) items.erase (std::remove_if (items.begin(), items.end(), [this] (const auto& it) { return it.first == &scopeBtn; }), items.end());   // stripped phone app: no spectrum
            for (auto* c : std::initializer_list<juce::Component*> { &retroBtn, &copyBtn, &pasteBtn, &undoBtn, &redoBtn, &delBtn, &dupBtn, &altABtn, &altSBtn, &duoBtn, &pianoBtn, &octDnBtn, &octUpBtn, &noteDnBtn, &noteUpBtn,
                                                                    &metroBtn, &countBtn, &mixPopBtn, &scopeBtn, &themeBtn, &cpuLabel, &closeAllBtn, &rackPopBtn, &importBtn, &bpm, &posLabel })
                c->setVisible (std::any_of (items.begin(), items.end(), [c] (const auto& it) { return it.first == c; }));
            for (auto& it : items) if (it.second > 0) it.second = it.second * 85 / 100;   // crunched: more room for the page
            const int rowH = 38, avail = juce::jmax (200, r.getWidth() - 16);
            int x = 0, rows = 1;
            for (auto& it : items) { if (it.second == brk) { if (x > 0) { ++rows; x = 0; } continue; } if (x > 0 && x + it.second > avail) { ++rows; x = 0; } x += it.second + 4; }
            topBar = r.removeFromTop (rows * rowH + 8);
            {
                // clip bar: STAMP / PAINT only (copy / paste); Select and Delete are the Playlist's icon tools, no word duplicates
                auto cb = juce::Rectangle<int> (topBar.getX() + 8, topBar.getBottom() + 2, juce::jmin (190, topBar.getWidth() - 16), 40);
                const int bw = cb.getWidth() / 2;
                for (auto* b : { &cbStamp, &cbPaint }) b->setBounds (cb.removeFromLeft (bw).reduced (2));
            }
            logoArea = {};
            int y = topBar.getY() + 4; x = 0;
            for (auto& it : items)
            {
                if (it.second == brk) { if (x > 0) { y += rowH; x = 0; } continue; }
                if (x > 0 && x + it.second > avail) { y += rowH; x = 0; }
                const juce::Rectangle<int> cell (topBar.getX() + 8 + x, y + 2, it.second, rowH - 4);
                if (it.first != nullptr) it.first->setBounds (cell); else logoArea = cell;
                x += it.second + 4;
            }
            mixPopBtn.setToggleState (view == 2, juce::dontSendNotification);
            rackPopBtn.setToggleState (view == 0 && ! rackHidden, juce::dontSendNotification);
        }
        else
        {
#if ! JUCE_MAC
        menuBar.setBounds (r.removeFromTop (22));
#endif
        topBar = build::toolbarBottom ? r.removeFromBottom (build::toolbarH) : r.removeFromTop (build::toolbarH);
        auto t = topBar.reduced (8, juce::jmax (4, (build::toolbarH - 32) / 2));
        mixPopBtn.setToggleState (popouts.count (mixer.get()) > 0, juce::dontSendNotification);
        rackPopBtn.setToggleState (popouts.count (rack.get()) > 0, juce::dontSendNotification);
        const std::pair<const char*, std::pair<juce::Component*, int>> tbItems[] {   // Layout > BUILD decides which show, on which side, in what order
            { "logo", { nullptr, 150 } }, { "beat", { &beatTab, 84 } }, { "record", { &recordTab, 84 } }, { "mix", { &mixTab, 84 } }, { "patch", { &patchTab, 84 } }, { "video", { &videoTab, 84 } }, { "song", { &songBtn, 48 } },
            { "start", { &startBtn, 40 } }, { "play", { &playBtn, 40 } }, { "stop", { &stopBtn, 40 } }, { "rec", { &recBtn, 40 } }, { "bpm", { &bpm, 110 } },
            { "pos", { &posLabel, 110 } }, { "metro", { &metroBtn, 62 } }, { "count", { &countBtn, 80 } }, { "retro", { &retroBtn, 62 } }, { "piano", { &pianoBtn, 62 } },
            { "cpu", { &cpuLabel, 86 } }, { "rackpop", { &rackPopBtn, 52 } }, { "mixpop", { &mixPopBtn, 40 } }, { "vidpop", { &vidPopBtn, 70 } }, { "scope", { &scopeBtn, 64 } },
            { "theme", { &themeBtn, 64 } }, { "browser", { &browserBtn, 120 } }, { "save", { &saveBtn, 70 } } };
        auto find = [&tbItems] (const juce::String& id) -> const std::pair<juce::Component*, int>* { for (auto& it : tbItems) if (id == it.first) return &it.second; return nullptr; };
        for (auto& it : tbItems) if (it.second.first != nullptr) it.second.first->setVisible (false);
        logoArea = {};
        const auto halves = juce::StringArray::fromTokens (build::toolbar, "|", "");
        std::set<juce::String> placed;
        std::vector<std::pair<const std::pair<juce::Component*, int>*, juce::String>> leftItems, rightItems;
        for (int h = 0; h < juce::jmin (2, halves.size()); ++h)
            for (auto& id : juce::StringArray::fromTokens (halves[h], ",", ""))
                if (auto* it = find (id); it != nullptr && placed.insert (id).second) (h == 0 ? leftItems : rightItems).push_back ({ it, id });
        if (placed.count ("mixpop") == 0) rightItems.insert (rightItems.begin(), { find ("mixpop"), "mixpop" });   // the Mixer pop-out never disappears
        if (placed.count ("vidpop") == 0) rightItems.insert (rightItems.begin() + std::min<std::ptrdiff_t> (1, (std::ptrdiff_t) rightItems.size()), { find ("vidpop"), "vidpop" });   // VID POP too (older saved layouts)
        auto place = [this] (const std::pair<juce::Component*, int>& it, juce::Rectangle<int> cell)
        {
            if (it.first == nullptr) logoArea = cell; else { it.first->setBounds (cell); it.first->setVisible (true); }
        };
        int need = 0;   // a narrow window squeezes every button a little before any of them drops off
        for (auto& [it, id] : leftItems) need += it->second + 6;
        for (auto& [it, id] : rightItems) need += it->second + 6;
        const float f = need > 0 ? juce::jlimit (0.45f, 1.0f, (float) t.getWidth() / (float) need) : 1.0f;
        auto w = [f] (int px) { return juce::roundToInt ((float) px * f); };
        int rightW = 0;
        for (auto& [it, id] : rightItems) rightW += w (it->second) + 6;
        auto rt = t.removeFromRight (juce::jmin (rightW, t.getWidth()));
        for (auto& [it, id] : leftItems)
        {
            const int gap = (id == "song" || id == "bpm") ? 10 : 4;
            if (t.getWidth() < w (it->second)) break;   // no room left: it stays hidden instead of overlapping
            place (*it, t.removeFromLeft (w (it->second))); t.removeFromLeft (gap);
        }
        for (auto& [it, id] : rightItems)
        {
            if (rt.getWidth() < w (it->second) && id != "mixpop") continue;
            place (*it, rt.removeFromLeft (w (it->second))); rt.removeFromLeft (6);
        }
        }
        {
            const int tp = juce::jlimit (60, 96, col::laneMinH), tw = 74;   // phone transport: as tall as a track, easy to hit
            auto foot = r.removeFromBottom (phoneUI ? tp : 22);
            const auto fullFoot = foot;
            laneBtn.setBounds (foot.removeFromLeft (phoneUI ? 64 : 52).reduced (1));   // LANE stays on the left
            lanePop.setVisible (showLanes);
            if (showLanes) lanePop.setBounds (foot.removeFromLeft (phoneUI ? 52 : 44).reduced (1));
            if (phoneUI)   // phone: start / play / stop / record in the bottom middle, a bit bigger, under your thumb
            {
                auto mid = fullFoot.withSizeKeepingCentre (4 * tw, fullFoot.getHeight());
                if (mid.getX() < foot.getX()) mid.setX (foot.getX());
                for (auto* b : std::initializer_list<juce::Component*> { &startBtn, &playBtn, &stopBtn, &recBtn }) { b->setVisible (true); b->setBounds (mid.removeFromLeft (tw).reduced (2)); }
                foot = foot.withLeft (juce::jmin (foot.getRight(), startBtn.getX() + 4 * tw + 4));
            }
            hintBar.setVisible (build::hint || phoneUI);
            if (build::hint || phoneUI) hintBar.setBounds (foot);
        }
        if (duo != nullptr)
        {
            const bool on = phoneUI && duoBtn.getToggleState();
            duo->setVisible (on);
            // DUO strip: a third of the screen, never cramped on a small phone or a waste on a big / unfolded one
            // and the page keeps at least 200 pt (landscape on a small phone: the strip shrinks, then steps aside)
            const int h = juce::jmin (juce::jlimit (150, 420, r.getHeight() * 32 / 100), r.getHeight() - 200);
            duo->setVisible (on && h >= 100);
            if (on && h >= 100) duo->setBounds (r.removeFromBottom (h));
        }
        if (kbInset != nullptr)
        {
            kbInset->setVisible (pianoBtn.getToggleState());
            if (pianoBtn.getToggleState()) kbInset->setBounds (r.removeFromBottom (phoneUI ? 110 : 76));
        }
        auto here = [this] (const std::unique_ptr<juce::Component>& c) { return popouts.count (c.get()) == 0; };
        if (here (lanes))
        {
            lanes->setVisible (showLanes);
            if (showLanes) lanes->setBounds (r.removeFromBottom (juce::jlimit (120, juce::jmax (120, r.getHeight() / 2), build::lanesH)));
        }
        if (phoneUI)
        {   // side tab: tap to bring Samples / Patterns / Plugin House out, tap again to tuck it back in
            auto tab = r.removeFromLeft (34);
            browserTab.setButtonText (showBrowser ? "<" : ">");
            browserTab.setBounds (tab.withSizeKeepingCentre (32, juce::jmin (140, tab.getHeight())));
        }
        if (here (browser))
        {
            browser->setVisible (showBrowser);
            if (showBrowser && phoneUI) browser->setBounds (r.removeFromLeft (juce::jmin (260, r.getWidth() * 36 / 100)));
            else if (showBrowser) { const int bw = juce::jlimit (160, juce::jmax (160, r.getWidth() / 2), build::browserW); browser->setBounds (build::browserRight ? r.removeFromRight (bw) : r.removeFromLeft (bw)); }
        }
        const bool dockMix = build::mixDock && ! phoneUI && view != 2 && here (mixer) && r.getHeight() > 300;
        const bool rackHere = here (rack), pianoHere = here (piano);
        if (rackHere) rack->setVisible (view == 0 && ! (phoneUI && rackHidden && pianoHere));
        if (pianoHere) piano->setVisible (view == 0);
        if (here (playlist)) playlist->setVisible (view == 1);
        if (here (mixer)) mixer->setVisible (view == 2 || dockMix);
        if (dockMix) mixer->setBounds (r.removeFromBottom (juce::jmin (build::mixH, r.getHeight() / 2)));
        pianoSplit.setVisible (false);
        if (view == 0)
        {
            if (rackHere && pianoHere && phoneUI && rackHidden) piano->setBounds (r);
            else if (rackHere && pianoHere)
            {
                splitTop = r.getY(); splitTotal = r.getHeight();
                const int splitH = phoneUI ? 34 : 22;
                const int rh = pianoMin ? splitTotal - splitH : juce::jlimit (80, juce::jmax (80, splitTotal - 140), (int) (splitTotal * rackFrac));
                if (build::rackBelow && ! phoneUI)
                {
                    rack->setBounds (r.removeFromBottom (rh));
                    pianoSplit.minimized = pianoMin; pianoSplit.setVisible (true); pianoSplit.setBounds (r.removeFromBottom (splitH)); pianoSplit.repaint();
                }
                else
                {
                    rack->setBounds (r.removeFromTop (rh));
                    pianoSplit.minimized = pianoMin; pianoSplit.setVisible (true); pianoSplit.setBounds (r.removeFromTop (splitH)); pianoSplit.repaint();
                }
                piano->setVisible (! pianoMin); piano->setBounds (r);
            }
            else if (rackHere) rack->setBounds (r);
            else if (pianoHere) piano->setBounds (r);
        }
        if (patchView != nullptr) patchView->setVisible (view == 3);
        if (videoView != nullptr) videoView->setVisible (view == 4);
        if (view >= 3) { if (here (mixer) && ! dockMix) mixer->setVisible (false); if (auto* pv = view == 3 ? patchView.get() : videoView.get()) pv->setBounds (r); }
        else if (view == 1) { if (here (playlist)) playlist->setBounds (r); }
        else if (here (mixer)) mixer->setBounds (r);
    }

    std::map<juce::String, juce::KeyPress> userKeys;   // Options > Key commands: action id -> your key
    bool remapping = false;
    void saveKeys()
    {
        juce::StringArray s; for (auto& [id, k] : userKeys) s.add (id + "=" + k.getTextDescription());
        studio.settings->setValue ("keymap", s.joinIntoString (";")); studio.settings->saveIfNeeded();
    }
   #if HHS_STORE
    static void relayoutAll (juce::Component& c) { c.resized(); c.repaint(); for (auto* k : c.getChildren()) relayoutAll (*k); }
    static juce::String proTerms (const juce::String& price)
    {
        return "HumHouse Pro: " + price + " per month, auto-renewing. Payment is charged to your Apple ID when you confirm. "
               "It renews every month unless you cancel at least 24 hours before the end of the current period. "
               "Manage or cancel in your Apple ID's Subscriptions settings.";
    }
    // HumHouse Pro: the $7.99/month unlock, with what it adds and the terms Apple requires next to the buy button
    struct UnlockPanel : public juce::Component
    {
        juce::Label title, body;
        juce::TextButton buy, restore { "Restore purchases" }, later { "Not now" };
        juce::HyperlinkButton terms { "Terms of Use", juce::URL ("https://www.apple.com/legal/internet-services/itunes/dev/stdeula/") },
                              privacy { "Privacy Policy", juce::URL ("https://github.com/elijahjfrierson-prog/humhouse-studio/blob/main/PRIVACY.md") };
        UnlockPanel()
        {
            const juce::String price (hhsStorePrice());
            title.setText (phoneLite ? "Unlock HumHouse Pro" : "HumHouse Pro is active", juce::dontSendNotification);
            title.setFont (juce::Font (juce::FontOptions (24.0f, juce::Font::bold)));
            body.setText ("Everything in HumHouse, on your phone:\n" + proList() + "\n" + proTerms (price), juce::dontSendNotification);
            body.setJustificationType (juce::Justification::topLeft);
            body.setColour (juce::Label::textColourId, col::text);
            buy.setButtonText (phoneLite ? "Subscribe - " + price + " / month" : "Manage subscription");
            buy.onClick = [this] { if (phoneLite) hhsStoreBuy(); else hhsStoreManage(); close(); };
            restore.onClick = [this] { hhsStoreRestore(); close(); };
            later.onClick = [this] { close(); };
            for (auto* c : std::initializer_list<juce::Component*> { &title, &body, &buy, &restore, &later, &terms, &privacy }) addAndMakeVisible (c);
            setSize (520, 520);
        }
        void close() { if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (0); }
        void resized() override
        {
            auto r = getLocalBounds().reduced (18);
            title.setBounds (r.removeFromTop (36)); r.removeFromTop (6);
            auto links = r.removeFromBottom (28);
            terms.setBounds (links.removeFromLeft (links.getWidth() / 2)); privacy.setBounds (links);
            r.removeFromBottom (8);
            auto row = r.removeFromBottom (44);
            later.setBounds (row.removeFromRight (110)); row.removeFromRight (8);
            restore.setBounds (row.removeFromRight (170)); row.removeFromRight (8);
            buy.setBounds (row);
            r.removeFromBottom (10);
            body.setBounds (r);
        }
    };
    void showUnlock()
    {
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (fitToScreen (new UnlockPanel()));
        o.dialogTitle = "HumHouse Pro"; o.dialogBackgroundColour = col::panel; o.useNativeTitleBar = true; o.resizable = false;
        o.launchAsync();
    }
   #endif
    void showKeyCommands()
    {
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (fitToScreen (new KeyCmdEditor (userKeys, [this] { saveKeys(); })));
        o.dialogTitle = "Key commands"; o.dialogBackgroundColour = col::panel; o.useNativeTitleBar = true; o.resizable = false;
        o.launchAsync();
    }
    // quick-added shortcuts: "menu:<menu text>" -> key, run like clicking that menu item
    static int findMenuId (const juce::PopupMenu& m, const juce::String& text)
    {
        for (juce::PopupMenu::MenuItemIterator it (m); it.next();)
        {
            const auto& i = it.getItem();
            if (i.subMenu != nullptr) { if (const int r = findMenuId (*i.subMenu, text)) return r; continue; }
            if (i.itemID != 0 && i.isEnabled && i.text == text) return i.itemID;
        }
        return 0;
    }
    static juce::String findMenuText (const juce::PopupMenu& m, int id)
    {
        for (juce::PopupMenu::MenuItemIterator it (m); it.next();)
        {
            const auto& i = it.getItem();
            if (i.subMenu != nullptr) { const auto r = findMenuText (*i.subMenu, id); if (r.isNotEmpty()) return r; continue; }
            if (i.itemID == id) return i.text;
        }
        return {};
    }
    juce::String menuTextFor (int id)
    {
        const auto names = getMenuBarNames();
        for (int i = 0; i < names.size(); ++i) { const auto t = findMenuText (getMenuForIndex (i, names[i]), id); if (t.isNotEmpty()) return t; }
        return {};
    }
    bool runMenuText (const juce::String& text)
    {
        const auto names = getMenuBarNames();
        for (int i = 0; i < names.size(); ++i)
            if (const int id = findMenuId (getMenuForIndex (i, names[i]), text)) { menuItemSelected (id, 0); return true; }
        return false;
    }
    juce::String commandUsing (const juce::KeyPress& k, const juce::String& except)   // which command already has this key ("" = free)
    {
        for (auto& [id, key] : userKeys) if (id != except && id.startsWith ("menu:") && key == k) return "\"" + id.fromFirstOccurrenceOf ("menu:", false, false) + "\"";
        for (auto& a : keyActions()) { const auto it = userKeys.find (a.id); if ((it != userKeys.end() ? it->second : a.def) == k) return a.name; }
        return {};
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (! remapping)
            for (auto& [id, key] : userKeys)
                if (id.startsWith ("menu:") && key.isValid() && k == key)
                {
                    const auto text = id.fromFirstOccurrenceOf ("menu:", false, false);
                    juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<MainComponent> (this), text] { if (sp != nullptr) sp->runMenuText (text); });
                    return true;
                }
        if (! remapping && ! userKeys.empty())
        {
            for (auto& a : keyActions())
                if (auto it = userKeys.find (a.id); it != userKeys.end() && k == it->second)
                {
                    const juce::ScopedValueSetter<bool> sv (remapping, true);
                    return keyPressed (a.def);
                }
            for (auto& a : keyActions())
                if (userKeys.count (a.id) > 0 && k == a.def) return true;   // moved to your own key
        }
        const int cmd = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier, alt = juce::ModifierKeys::altModifier;
        auto is = [&k] (int key, int mods) { return k == juce::KeyPress (key, mods, 0); };
        if (is (juce::KeyPress::spaceKey, 0)) { engine.togglePlay(); return true; }
        if (is (juce::KeyPress::homeKey, 0)) { engine.setPositionBeats (0.0); return true; }
        if (is ('u', cmd)) { showShortcuts(); return true; }   // Cmd/Ctrl+U: Shortcuts window on / off
        if (! remapping && isTypingNoteKey (k))   // a key moved onto R (Set to Pro Tools / Ableton) still records
        {
            const int key = 48 + typingKeyOffset (k.getKeyCode());
            if (heldTyping.count (k.getKeyCode()) == 0) { heldTyping[k.getKeyCode()] = key; liveNote (key, 0.8f, true); }
            return true;
        }
        if (is ('r', 0)) { engine.setRecording (! engine.isRecording()); return true; }
        if (is ('l', 0)) { songBtn.setToggleState (! songBtn.getToggleState(), juce::sendNotificationSync); return true; }
        if (k == juce::KeyPress ('w', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) { closeEverything(); return true; }   // close every open window (OBS: one key, from any window)
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
    juce::StringArray getMenuBarNames() override { return { TRANS ("File"), TRANS ("Edit"), TRANS ("View"), TRANS ("Options"), TRANS ("Help") }; }

    juce::PopupMenu getMenuForIndex (int index, const juce::String&) override
    {
        juce::PopupMenu m;
        auto add = [&m] (int id, const juce::String& text, const juce::String& shortcut = {}, bool enabled = true, bool ticked = false)
        {
            juce::PopupMenu::Item it (juce::translate (text));
            it.itemID = id; it.shortcutKeyDescription = shortcut; it.isEnabled = enabled; it.isTicked = ticked;
            m.addItem (it);
        };
        if (index == 0)
        {
           #if HHS_STORE
            m.addItem (4896, phoneLite ? "Unlock HumHouse Pro (" + juce::String (hhsStorePrice()) + " / month)..." : "HumHouse Pro (active)...");
            m.addItem (4897, "Restore purchases");
            m.addSeparator();
           #endif
            add (mNew, "New project", "Ctrl+N"); add (mOpen, "Open...", "Ctrl+O"); add (mOpenBackup, "Open backup...");
            m.addItem (4715, "Convert stems folder (Logic / Pro Tools)...");
            m.addItem (4716, "Shrink .hhs files to KB (pick a folder)...");
            if (! phoneUI) m.addItem (4718, "EXPORT ALL: video + Record timeline mix (one video file)...");
            m.addItem (4717, "Pack audio inside songs (MB, opens on any computer)", true, packAudioInSong().load());
            m.addItem (4724, "Frame meter (frame time, dropped frames)", true, frameMeterOn().load());
            m.addItem (4725, "Zoom around the playhead (else the pointer)", true, zoomAtPlayhead().load());
            m.addItem (4726, "Lock playhead (clicks in the Playlist don't move it; the ruler still does)", true, playheadLock().load());
            if (! phoneUI)
            {
                m.addItem (4722, "Background picture...");   // Background picture (behind the UI, from Downloads)
                m.addItem (4723, "Remove background picture", bgPicture.isValid());
            }
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
            if (! phoneUI) add (mDJ, "Hum Verse (DJ: 2 decks, waveforms, hot cues, library)");
            if (! phoneLite) add (mPatchBay, "Patch Bay (nodes: sequencers, sounds, FX, LFOs)");
            m.addSeparator();
            add (mSamplesUsed, "Samples used in this project...");
            m.addItem (4721, "Tune Finder: key / BPM / tune of the selected clip");
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
            if (! phoneUI) add (mPlugins, "Plugin House: scan / manage plugins...");
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
            m.addItem (4790, "Neon 3D look (HumHouse future)", ! col::lightMode, col::neon);
            m.addItem (4712, "Layout...");
            m.addItem (4713, "Mini keyboard (pop-out)", true, miniKb != nullptr);
            m.addItem (4714, "Dual outputs: hear 1-2 and 3-4 together (monitor controllers)", true, engine.dualOut.load());
            {
                juce::PopupMenu sc;
                const float cur = juce::Desktop::getInstance().getGlobalScaleFactor();
                const int pcts[] { 75, 90, 100, 110, 125, 150, 175, 200 };
                for (int i = 0; i < (phoneUI ? 6 : 8); ++i) sc.addItem (200 + i, juce::String (pcts[i]) + "%", true, std::abs (cur * 100.0f - (float) pcts[i]) < 1.0f);
                m.addSubMenu ("UI scale", sc);
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
               #if JUCE_WINDOWS
                pm.addItem (476, "HumHouse No Latency Driver (built-in fast Windows audio when no ASIO driver is used)", true, engine.noLatencyDriver.load());
               #endif
                pm.addItem (473, "Low Latency Mode (armed track goes straight to the master, FX stay on; takes still land on time)", true, engine.lowLatency.load());
                {
                    juce::PopupMenu om;
                    om.addSectionHeader ("Takes land late? Pick a + value. Early? Pick a - value.");
                    for (int i = 0; i < 9; ++i)
                    {
                        const float v = Engine::recordOffsetChoices[i];
                        om.addItem (490 + i, v == 0.0f ? juce::String ("0 ms (driver latency only)") : (v > 0 ? "+" : "") + juce::String ((int) v) + " ms", true, std::abs (engine.recordOffsetMs.load() - v) < 0.01f);
                    }
                    pm.addSubMenu ("Recording offset (" + juce::String (juce::roundToInt (engine.recordOffsetMs.load())) + " ms)", om);
                }
                pm.addItem (4720, "Recording offset slider (-5000 to +5000 ms, like Logic)...");
                pm.addItem (4721, "Close all windows (plug-ins, Mixer, settings)  Cmd/Ctrl+Shift+W");
                pm.addItem (474, "RAM disk cache (clips play from memory, like Pro Tools)", true, engine.ramCache.load());
                m.addSubMenu ("Performance", pm);
               #if JUCE_WINDOWS
                m.addItem (480, "OBS / Discord capture-friendly drawing (untick = faster GPU drawing)", true, captureFriendlyDrawing());
               #endif
                m.addItem (4726, "HumHouse window title bars: red X + blue minimize (stream-safe; untick = native)", true, humhouseTitleBars());
            }
            add (mClearBlacklist, "Re-enable crashed plugins", {}, engine.knownPlugins.getBlacklistedFiles().size() > 0);
            m.addItem (4711, "Key commands...");
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
        if (id >= mHistory && id < mHistory + 1000) { studio.jumpHistory (id - mHistory); return; }   // 4711+ are Options items
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
        if (id == 397) { templatesDir().createDirectory(); if (phoneUI) phoneTemplates(); else templatesDir().startAsProcess(); return; }
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
        if (id >= 250 && id < 254) { setFontStyle (3); return; }
        if (id == 4726)   // windows opened from now on
        {
            humhouseTitleBars() = ! humhouseTitleBars();
            appDataFile ("titlebars.txt").getParentDirectory().createDirectory();
            appDataFile ("titlebars.txt").replaceWithText (humhouseTitleBars() ? "1" : "0");
            return;
        }
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
        if (id == 472)
        {
            engine.pluginSandbox = ! engine.pluginSandbox.load();
            studio.settings->setValue ("pluginSandbox", engine.pluginSandbox.load());
            flash (engine.pluginSandbox.load() ? "Crash protection on for plug-ins loaded from now" : "Plug-ins load directly (fastest) from now");
            return;
        }
        if (id >= 490 && id <= 498)
        {
            engine.recordOffsetMs = Engine::recordOffsetChoices[id - 490];
            studio.settings->setValue ("recordOffsetMs", (double) engine.recordOffsetMs.load());
            flash ("Recording offset " + juce::String (juce::roundToInt (engine.recordOffsetMs.load())) + " ms: new takes move by this much");
            return;
        }
        if (id == 4721) { closeEverything(); return; }
        if (id == 4720)
        {
            auto* sl = new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
            sl->setRange (-5000.0, 5000.0, 1.0); sl->setTextValueSuffix (" ms"); sl->setDoubleClickReturnValue (true, 0.0);
            sl->setValue (engine.recordOffsetMs.load(), juce::dontSendNotification);
            sl->setTooltip ("Recording offset: + moves new takes earlier, - later. Double-click = 0");
            sl->onValueChange = [this, sl] { engine.recordOffsetMs = (float) sl->getValue(); studio.settings->setValue ("recordOffsetMs", sl->getValue()); };
            sl->setSize (phoneUI ? 340 : 520, 48);
            juce::CallOutBox::launchAsynchronously (std::unique_ptr<juce::Component> (sl), getLocalBounds().withSizeKeepingCentre (20, 20), this);
            return;
        }
        if (id == 4721) { if (tuneFinderHook) tuneFinderHook(); return; }
        if (id == 476)
        {
            if (engine.isRecording()) { flash ("Stop recording first, then switch the No Latency Driver"); return; }
            engine.noLatencyDriver = ! engine.noLatencyDriver.load();
            studio.settings->setValue ("noLatencyDriver", engine.noLatencyDriver.load());
            const bool sw = engine.applyNoLatencyDriver();
            flash (engine.noLatencyDriver.load() ? (sw ? "No Latency Driver on: switched to fast Windows audio" : "No Latency Driver on")
                                                 : "No Latency Driver off: pick any driver in Audio settings");
            return;
        }
        if (id == 473 || id == 474)
        {
            auto& flag = id == 473 ? engine.lowLatency : engine.ramCache;
            flag = ! flag.load();
            studio.settings->setValue (id == 473 ? "lowLatency" : "ramCache", flag.load());
            if (id == 474) engine.sync();
            flash (id == 473 ? (flag.load() ? "Low Latency Mode on: the armed track skips delay compensation, FX stay on" : "Low Latency Mode off")
                             : (flag.load() ? "RAM disk cache on: clips play from memory" : "RAM disk cache off: clips stream from disk"));
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
            case mDJ: showDjDecks (studio); break;
            case mPatchBay: setView (3); break;
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
            case 4790:
                col::neon = ! col::neon; studio.settings->setValue ("themeNeon", col::neon); studio.settings->saveIfNeeded();
                if (col::neon) col::lightMode = false;
                applyTheme (studio.settings->containsKey ("themeColour") ? juce::Colour::fromString (studio.settings->getValue ("themeColour")) : juce::Colour (0xff9b5cff),
                            (float) studio.settings->getDoubleValue ("themeTint", 0.15), studio.settings->getBoolValue ("themeBlack", false));
                resaveLayout (studio);
                repaint(); break;
            case 4711: showKeyCommands(); break;
            case 4712: showLayoutDialog (studio); break;
            case 4713: toggleMiniKeyboard(); break;
            case 4714:
            {
                const bool on = ! engine.dualOut.load();
                engine.setDualOutput (on);
                if (studio.settings) { studio.settings->setValue ("dualOut", on); studio.settings->saveIfNeeded(); }
                flash (on ? "Dual outputs on: 1-2 and 3-4 play the same mix, sample-locked" : "Dual outputs off");
                break;
            }
            case 4715: convertStems(); break;
            case 4716:
                chooser = std::make_unique<juce::FileChooser> ("Folder with your .hhs songs (subfolders too)", hhsHome());
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc) {
                    const auto dir = fc.getResult();
                    if (! dir.isDirectory()) return;
                    const int n = shrinkSongs (dir);
                    flash (juce::String (n) + " song(s) shrunk to KB: their audio stays next to them on disk");
                });
                break;
            case 4718:
                if (videoView == nullptr) { videoView = createVideoView (studio); addChildComponent (*videoView); }
                if (videoExportAllHook()) videoExportAllHook()(); else flash ("EXPORT ALL needs the VIDEO editor (desktop build with video)");
                break;
            case 4722:   // a picture behind the UI: shows wherever a page is popped out (View > Detach ...)
                chooser = std::make_unique<juce::FileChooser> ("Background picture", juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads"), "*.png;*.jpg;*.jpeg;*.gif");
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
                    const auto f = fc.getResult();
                    if (! f.existsAsFile()) return;
                    auto img = juce::ImageFileFormat::loadFrom (f);
                    if (! img.isValid()) { flash ("Couldn't read that picture (PNG, JPG or GIF work)"); return; }
                    bgPicture = img; studio.settings->setValue ("bgPicture", f.getFullPathName());
                    flash ("Background picture set: pop pages out (View > Detach) to see it"); repaint();
                });
                break;
            case 4723: bgPicture = bgScaled = bgScaledFrom = {}; studio.settings->removeValue ("bgPicture"); repaint(); break;
            case 4724: frameMeterOn() = ! frameMeterOn().load(); if (frameMeterOn().load()) engine.resetCallbackMeter(); if (playlist != nullptr) playlist->repaint(); break;
           #if HHS_STORE
            case 4896: showUnlock(); break;
            case 4897: hhsStoreRestore(); break;
           #endif
            case 4725: zoomAtPlayhead() = ! zoomAtPlayhead().load(); break;
            case 4726: playheadLock() = ! playheadLock().load(); studio.settings->setValue ("playheadLock", playheadLock().load()); break;
            case 4717:
                packAudioInSong() = ! packAudioInSong().load();
                studio.settings->setValue ("packAudioInSong", packAudioInSong().load());
                flash (packAudioInSong().load() ? "Songs pack their audio inside (MB)" : "Songs stay KB and point at their audio files");
                break;
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
    std::unique_ptr<juce::Component> rack, piano, playlist, mixer, browser, lanes;
    struct SplitBar : juce::Component   // drag to resize Channel Rack vs Piano Roll; click to minimize / show the Piano Roll
    {
        SplitBar() { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); }
        std::function<void (int)> onDrag; std::function<void()> onToggle; bool minimized = false; int startY = 0;
        void paint (juce::Graphics& g) override
        {
            g.fillAll (col::lightMode ? col::panel2 : juce::Colour (0xff15181d));
            g.setColour (col::accent.withAlpha (0.8f)); g.fillRoundedRectangle (getLocalBounds().toFloat().withSizeKeepingCentre (40, 4).withX (8), 2);
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
    int coreTick = 0;
    juce::VBlankAttachment paintPump { this, [] { pumpPendingPaints(); } };   // visuals never wait behind mouse moves
    juce::TextButton pagesBtn, beatPg, recPg, mixPg, cbSelect, cbTrash, cbStamp, cbPaint;
    bool clipBarShown = false;
    juce::TextButton beatTab { "BEAT" }, recordTab { "RECORD" }, mixTab { "MIX" }, patchTab { "PATCH" }, videoTab { "VIDEO" };
    std::unique_ptr<juce::Component> patchView, videoView;
    // phone: double-tap = right-click on every control (FX slots, clips, nodes, knobs...)
    // phone: a double tap or a ~0.5 s hold acts as ONE right-click (plug-in windows excluded: holding works their controls)
    struct DoubleTapMenu : juce::MouseListener, private juce::Timer
    {
        bool busy = false;
        static bool marked (juce::Component* c, const char* prop) { for (; c != nullptr; c = c->getParentComponent()) if (c->getProperties().contains (prop)) return true; return false; }
        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            stopTimer();
            if (! phoneUI || busy || e.eventComponent == nullptr || e.mods.isPopupMenu() || marked (e.eventComponent, "ownDoubleTap")) return;
            fire (e.eventComponent, e.position, e.source, e.mods);
        }
        juce::Component::SafePointer<juce::Component> holdT; juce::Point<float> holdPos; juce::Point<float> holdScreen;
        std::optional<juce::MouseInputSource> holdSrc; juce::ModifierKeys holdMods; bool held = false;
        void mouseDown (const juce::MouseEvent& e) override
        {
            stopTimer(); held = false;
            if (! phoneUI || busy || e.eventComponent == nullptr || e.mods.isPopupMenu() || e.source.getIndex() > 0 || marked (e.eventComponent, "noHold")) return;
            holdT = e.eventComponent; holdPos = e.position; holdScreen = e.position + e.eventComponent->getScreenPosition().toFloat(); holdSrc = e.source; holdMods = e.mods;
            startTimer (550);
        }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (isTimerRunning() && e.eventComponent != nullptr && (e.position + e.eventComponent->getScreenPosition().toFloat()).getDistanceFrom (holdScreen) > 10.0f) stopTimer();
        }
        void mouseUp (const juce::MouseEvent&) override { stopTimer(); }
        void timerCallback() override
        {
            stopTimer();
            if (holdT != nullptr && holdSrc && holdSrc->isDragging()) { held = true; fire (holdT.getComponent(), holdPos, *holdSrc, holdMods); }
        }
        void fire (juce::Component* comp, juce::Point<float> pos, juce::MouseInputSource src, juce::ModifierKeys m)
        {
            for (auto* up = comp->getParentComponent(); up != nullptr; up = up->getParentComponent())   // a button on a track bar: the bar's own menu
                if (up->getProperties().contains ("holdMenu")) { pos = up->getLocalPoint (comp, pos); comp = up; break; }
            juce::Component::SafePointer<juce::Component> t (comp);
            const auto mods = m.withoutMouseButtons().withFlags (juce::ModifierKeys::rightButtonModifier | juce::ModifierKeys::popupMenuClickModifier);
            juce::MessageManager::callAsync ([this, t, mods, pos, src] {
                if (t == nullptr) return;
                busy = true;
                const auto now = juce::Time::getCurrentTime();
                juce::MouseEvent down (src, pos, mods, 1.0f, 0, 0, 0, 0, t.getComponent(), t.getComponent(), now, pos, now, 1, false);
                t->mouseDown (down);
                if (t != nullptr) t->mouseUp (down.withNewPosition (pos));
                busy = false;
            });
        }
    } doubleTapMenu;
    TransportButton startBtn, playBtn, stopBtn, recBtn, mixPopBtn;
    juce::TextButton rackPopBtn;
    juce::TextButton retroBtn, pianoBtn, duoBtn;
    juce::MidiKeyboardState kbState;
    std::unique_ptr<juce::MidiKeyboardComponent> kbInset;
    // phone DUO: the page on top, below it KEYS (mini piano), PADS (one per Channel Rack sound) or MIX (mini mixer faders).
    // Every size comes from the strip's own bounds, so it fits any phone: folded, unfolded, portrait or landscape.
    struct DuoStrip : juce::Component, private juce::Timer
    {
        struct Pad : juce::Component
        {
            Pad (MainComponent& o, int ch) : m (o), id (ch) {}
            void paint (juce::Graphics& g) override
            {
                const auto* c = m.studio.channel (id);
                auto r = getLocalBounds().toFloat().reduced (3.0f);
                g.setColour ((down ? col::accent : col::panel2).withAlpha (down ? 0.9f : 1.0f)); g.fillRoundedRectangle (r, 8.0f);
                g.setColour (col::accent.withAlpha (0.6f)); g.drawRoundedRectangle (r, 8.0f, 1.5f);
                g.setColour (col::text); g.setFont (juce::FontOptions (juce::jlimit (11.0f, 18.0f, r.getHeight() * 0.22f)));
                g.drawFittedText (c != nullptr ? c->name : juce::String(), r.reduced (6.0f).toNearestInt(), juce::Justification::centred, 2);
            }
            void mouseDown (const juce::MouseEvent&) override { if (const auto* c = m.studio.channel (id)) { key = c->rootKey; m.engine.previewNote (id, key, 0.9f, true); } down = true; repaint(); }
            void mouseUp (const juce::MouseEvent&) override { if (down && key >= 0) m.engine.previewNote (id, key, 0.0f, false); down = false; key = -1; repaint(); }
            MainComponent& m; int id; int key = -1; bool down = false;
        };
        struct Fader : juce::Slider   // a sideways swipe scrolls the strip, the volume stays put
        {
            SwipeLock swipe;
            Fader() : juce::Slider (juce::Slider::LinearVertical, juce::Slider::NoTextBox) { setSliderSnapsToMousePosition (false); }
            void mouseDown (const juce::MouseEvent& e) override { swipe.down(); juce::Slider::mouseDown (e); }
            void mouseDrag (const juce::MouseEvent& e) override { if (swipe.allows (e)) juce::Slider::mouseDrag (e); }
            double startV = 0.0;
        };
        struct Strip : juce::Component
        {
            Fader fader;
            juce::Label name;
            Strip() { addAndMakeVisible (fader); addAndMakeVisible (name); fader.setRange (0.0, 1.25, 0.0); name.setJustificationType (juce::Justification::centred); name.setFont (juce::FontOptions (12.0f)); name.setColour (juce::Label::textColourId, col::text); }
            void resized() override { auto r = getLocalBounds(); name.setBounds (r.removeFromBottom (18)); fader.setBounds (r.reduced (2)); fader.setMouseDragSensitivity (juce::jmax (40, fader.getHeight() - 20)); }   // full travel = the fader's own length
        };
        explicit DuoStrip (MainComponent& o) : m (o), keys (o.kbState, juce::MidiKeyboardComponent::horizontalKeyboard)
        {
            setOpaque (true);
            const char* names[] { "KEYS", "PADS", "MIX" };
            for (int i = 0; i < 3; ++i)
            {
                auto* b = tabs.add (new juce::TextButton (names[i]));
                b->setClickingTogglesState (true); b->setRadioGroupId (7301); b->setWantsKeyboardFocus (false);
                b->onClick = [this, i] { tab = i; m.studio.settings->setValue ("duoTab", i); show(); };
                addAndMakeVisible (b);
            }
            keys.setAvailableRange (24, 108); keys.setLowestVisibleKey (48); keys.setWantsKeyboardFocus (false); keys.clearKeyMappings();
            keys.setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, col::accent.withAlpha (0.7f));
            addChildComponent (keys); addChildComponent (padView); addChildComponent (mixView);
            padArea.getProperties().set ("noHold", true); padArea.getProperties().set ("ownDoubleTap", true);   // pads play: a hold is a held note, never a right-click
            padView.setViewedComponent (&padArea, false); padView.setScrollBarsShown (true, false);
            padView.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::never); padView.setScrollBarThickness (18);   // a finger on a pad plays it at once; the bar scrolls
            mixView.setViewedComponent (&mixBody, false); mixView.setScrollBarsShown (false, true);
            mixView.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::nonHover);
            tab = juce::jlimit (0, 2, m.studio.settings->getIntValue ("duoTab", 0));
            tabs[tab]->setToggleState (true, juce::dontSendNotification);
            show();
            startTimerHz (8);
        }
        ~DuoStrip() override { m.kbState.allNotesOff (1); }
        void paint (juce::Graphics& g) override { g.fillAll (col::panel); g.setColour (col::accent.withAlpha (0.5f)); g.fillRect (0, 0, getWidth(), 1); }
        void show()
        {
            keys.setVisible (tab == 0); padView.setVisible (tab == 1); mixView.setVisible (tab == 2);
            if (tab != 0) m.kbState.allNotesOff (1);
            rebuild(); resized();
        }
        // pads / faders follow the song: new sounds or mixer tracks show up without reopening
        juce::String layoutKey() const
        {
            juce::String k;
            if (tab == 1) for (auto& c : m.studio.p().channels) k << c.id << ",";
            if (tab == 2) { const auto& mx = m.studio.p().mixer; for (size_t t = 0; t < mx.size(); ++t) if (t == 0 || insertUsedHere ((int) t)) k << (int) t << mx[t].name << ","; }
            return k;
        }
        bool insertUsedHere (int t) const { return MainComponent::insertUsed (m.studio.p(), t); }
        void rebuild()
        {
            const auto k = layoutKey();
            if (k == builtFor) return;
            builtFor = k;
            pads.clear(); strips.clear();
            if (tab == 1)
                for (auto& c : m.studio.p().channels) padArea.addAndMakeVisible (pads.add (new Pad (m, c.id)));
            if (tab == 2)
            {
                const auto& mx = m.studio.p().mixer;
                for (size_t t = 0; t < mx.size(); ++t)
                {
                    if (t != 0 && ! insertUsedHere ((int) t)) continue;
                    auto* st = strips.add (new Strip());
                    st->name.setText (t == 0 ? juce::String ("Master") : mx[t].name.isEmpty() ? "Insert " + juce::String ((int) t) : mx[t].name, juce::dontSendNotification);
                    st->fader.setValue (mx[t].volume, juce::dontSendNotification);
                    const int idx = (int) t;
                    st->fader.onValueChange = [this, st, idx] { m.studio.live ([&] (Project& p) { if (idx < (int) p.mixer.size()) p.mixer[(size_t) idx].volume = (float) st->fader.getValue(); }); };
                    st->fader.onDragStart = [st] { st->fader.startV = st->fader.getValue(); };   // also fires for VoiceOver edits
                    st->fader.onDragEnd = [this, st] { if (st->fader.getValue() != st->fader.startV) m.studio.commit ("Volume"); };   // a scroll swipe isn't an edit
                    mixBody.addAndMakeVisible (st);
                }
            }
        }
        void timerCallback() override
        {
            if (! isShowing()) return;
            if (layoutKey() != builtFor) { rebuild(); resized(); return; }
            if (tab == 2 && ! touchHeld())
            {
                const auto& mx = m.studio.p().mixer; int i = 0;
                for (size_t t = 0; t < mx.size() && i < strips.size(); ++t)
                    if (t == 0 || insertUsedHere ((int) t)) strips[i++]->fader.setValue (mx[t].volume, juce::dontSendNotification);
            }
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (4);
            auto bar = r.removeFromTop (juce::jlimit (30, 44, getHeight() / 6));
            const int tw = juce::jmin (120, bar.getWidth() / 3);
            for (auto* b : tabs) b->setBounds (bar.removeFromLeft (tw).reduced (2));
            r.removeFromTop (4);
            keys.setBounds (r);
            // about two octaves across the strip; keys never thinner than a fingertip (~30 pt)
            keys.setKeyWidth (juce::jmax (30.0f, (float) r.getWidth() / 15.0f));
            padView.setBounds (r);
            if (pads.isEmpty()) padArea.setSize (r.getWidth(), r.getHeight());
            else
            {
                const int cols = juce::jlimit (2, 8, r.getWidth() / juce::jmax (1, juce::jmin (r.getHeight() / 2, 110)));
                const int rows = juce::jmax (1, (pads.size() + cols - 1) / cols);
                const int pw = (r.getWidth() - (rows * juce::jmax (44, r.getHeight() / juce::jmin (rows, 4)) > r.getHeight() ? 18 : 0)) / cols;   // room for the scroll bar
                const int ph = juce::jmax (44, r.getHeight() / juce::jmin (rows, 4));
                for (int i = 0; i < pads.size(); ++i) pads[i]->setBounds ((i % cols) * pw, (i / cols) * ph, pw, ph);
                padArea.setSize (r.getWidth(), juce::jmax (r.getHeight(), rows * ph));
            }
            mixView.setBounds (r);
            const int sw = juce::jmax (56, juce::jmin (90, r.getWidth() / juce::jmax (1, juce::jmin (strips.size(), 8))));
            mixBody.setSize (juce::jmax (r.getWidth(), sw * strips.size()), r.getHeight() - 8);
            for (int i = 0; i < strips.size(); ++i) strips[i]->setBounds (i * sw, 0, sw, mixBody.getHeight());
        }
        MainComponent& m;
        juce::OwnedArray<juce::TextButton> tabs;
        juce::MidiKeyboardComponent keys;
        juce::Component padArea, mixBody;
        juce::Viewport mixView, padView;
        juce::OwnedArray<Pad> pads;
        juce::OwnedArray<Strip> strips;
        juce::String builtFor { "-" };
        int tab = 0;
    };
    static juce::String proList()   // what the free phone app leaves out (phoneLite)
    {
        return "- Glitch Gate stutters, repeats and tape effects on clip fades\n"
               "- Mastering tools, Maximizer, spectrum and scope views\n"
               "- Automation and LFOs on any knob\n"
               "- Patch Bay, HumWave synth and the extra sound packs\n"
               "- Vocal Room, Studio Vocal and Chroma Verb\n"
               "- Curved fades and automation\n";
    }
    // phone: the Welcome page the desktop apps open with (what's in it; App Store app: what HumHouse Pro adds + subscribe)
    struct PhoneWelcome : public juce::Component
    {
        juce::Label title, sub, body;
        juce::Viewport view;
        juce::ToggleButton dontShow { "Don't show this again" };
        juce::TextButton start { "Start making music" }, buy, restore { "Restore purchases" };
        juce::HyperlinkButton terms { "Terms of Use", juce::URL ("https://www.apple.com/legal/internet-services/itunes/dev/stdeula/") },
                              privacy { "Privacy Policy", juce::URL ("https://github.com/elijahjfrierson-prog/humhouse-studio/blob/main/PRIVACY.md") };
        bool store = false;
        std::function<void (bool off)> onDone;
        PhoneWelcome()
        {
           #if HHS_STORE
            store = phoneLite;
           #endif
            title.setText ("Welcome to HumHouse Studio", juce::dontSendNotification);
            title.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
            sub.setText ("Beats and vocals in one place - full studio workflow, phone-fast.", juce::dontSendNotification);
            sub.setColour (juce::Label::textColourId, col::dim);
            fill();
           #if HHS_STORE
            buy.onClick = [this] { hhsStoreBuy(); finish(); };
            restore.onClick = [this] { hhsStoreRestore(); finish(); };
           #endif
            body.setJustificationType (juce::Justification::topLeft);
            body.setColour (juce::Label::textColourId, col::text);
            view.setViewedComponent (&body, false);
            view.setScrollBarsShown (true, false);
            view.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::all);
            start.onClick = [this] { finish(); };
            for (auto* c : std::initializer_list<juce::Component*> { &title, &sub, &view, &dontShow, &start }) addAndMakeVisible (c);
            if (store) for (auto* c : std::initializer_list<juce::Component*> { &buy, &restore, &terms, &privacy }) addAndMakeVisible (c);
        }
        void fill()   // again once the App Store answers with the buyer's local price
        {
            juce::String t ("FREE IN HUMHOUSE\n"
                            "- Channel Rack: step sequencer, built-in kits, sampler and synth\n"
                            "- Piano Roll: scale highlight, chord stamp, hi-hat rolls, 808 slides\n"
                            "- Playlist: patterns and audio clips, select, paint, slice, mute\n"
                            "- DUO strip: mini keyboard, drum pads or mini mixer under any page\n"
                            "- Record vocals: punch-in, loop takes, AutoPitch\n"
                            "- Mixer: EQ, compression, reverb, delay, sends\n"
                            "- Export WAV / MP3 / M4A; songs open on HumHouse for Mac and Windows\n");
            if (phoneLite) t << "\n" << (store ? "HUMHOUSE PRO ADDS\n" : "NOT IN THIS VERSION\n") << proList();
           #if HHS_STORE
            if (store) t << "\n" << proTerms (hhsStorePrice());
            buy.setButtonText ("Subscribe - " + juce::String (hhsStorePrice()) + " / month");
           #endif
            if (t == body.getText()) return;
            body.setText (t, juce::dontSendNotification);
            if (! getBounds().isEmpty()) resized();
        }
        void finish() { if (onDone) onDone (dontShow.getToggleState()); }
        void paint (juce::Graphics& g) override { g.fillAll (col::bg); }
        void resized() override
        {
            auto r = getLocalBounds();
            if (auto* d = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds())) r = d->safeAreaInsets.subtractedFrom (r);   // below the notch
            r = r.reduced (16, 10);
            title.setBounds (r.removeFromTop (32)); sub.setBounds (r.removeFromTop (22)); r.removeFromTop (6);
            const bool wide = r.getWidth() >= 560;
            if (store)
            {
                auto links = r.removeFromBottom (26);
                terms.setBounds (links.removeFromLeft (links.getWidth() / 2)); privacy.setBounds (links);
            }
            auto row = r.removeFromBottom (44);
            if (store && ! wide)
            {
                start.setBounds (row); r.removeFromBottom (6);
                auto row2 = r.removeFromBottom (44);
                restore.setBounds (row2.removeFromRight (row2.getWidth() / 3)); row2.removeFromRight (6); buy.setBounds (row2);
            }
            else
            {
                start.setBounds (row.removeFromRight (190));
                if (store) { row.removeFromRight (6); restore.setBounds (row.removeFromRight (160)); row.removeFromRight (6); buy.setBounds (row); }
            }
            dontShow.setBounds (r.removeFromBottom (32)); r.removeFromBottom (4);
            view.setBounds (r);
            juce::AttributedString a; a.append (body.getText(), body.getFont(), col::text); a.setWordWrap (juce::AttributedString::byWord);
            juce::TextLayout tl; tl.createLayout (a, (float) juce::jmax (10, r.getWidth() - 24));
            body.setSize (r.getWidth() - 12, juce::jmax (r.getHeight(), (int) std::ceil (tl.getHeight()) + 24));
        }
    };
    std::unique_ptr<PhoneWelcome> welcome;
    std::unique_ptr<DuoStrip> duo;
    struct MiniKb : juce::DocumentWindow   // Options > Mini keyboard: a small floating keyboard; click it, then play with the mouse or your computer keys
    {
        MiniKb (juce::MidiKeyboardState& st) : juce::DocumentWindow ("Mini keyboard", col::panel, juce::DocumentWindow::closeButton)
        {
            auto* k = new juce::MidiKeyboardComponent (st, juce::MidiKeyboardComponent::horizontalKeyboard);
            k->setAvailableRange (24, 108); k->setLowestVisibleKey (48); k->setKeyWidth (24.0f); k->setOctaveForMiddleC (4);
            k->setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, col::accent.withAlpha (0.7f));
            k->setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, col::accent.withAlpha (0.25f));
            k->clearKeyMappings(); k->setWantsKeyboardFocus (false);   // computer keys go to the typing keyboard, so E / C / P and the other shortcuts keep working
            setContentOwned (k, false);
            setUsingNativeTitleBar (true); useHumHouseTitleBar (*this); setResizable (true, false); setResizeLimits (300, 80, 2000, 300);
            setWantsKeyboardFocus (false);
            centreWithSize (640, 120); addToDesktop (getDesktopWindowStyleFlags()); setVisible (true); markFloating (*this);   // floats over the app only, never glued over other apps
            miniKeyboardOpen() = true;
        }
        ~MiniKb() override { miniKeyboardOpen() = false; }
        void closeButtonPressed() override { if (onClose) onClose(); }
        std::function<void()> onClose;
    };
    std::unique_ptr<MiniKb> miniKb;
    void toggleMiniKeyboard()
    {
        if (miniKb != nullptr) { miniKb = nullptr; return; }
        miniKb = std::make_unique<MiniKb> (kbState);
        miniKb->onClose = [sp = juce::Component::SafePointer<MainComponent> (this)] { juce::MessageManager::callAsync ([sp] { if (sp != nullptr) sp->miniKb = nullptr; }); };
    }
    struct KbListen : juce::MidiKeyboardState::Listener
    {
        explicit KbListen (MainComponent& o) : m (o) {}
        void handleNoteOn (juce::MidiKeyboardState*, int, int note, float vel) override { m.liveNote (note, juce::jmax (0.05f, vel), true); }
        void handleNoteOff (juce::MidiKeyboardState*, int, int note, float) override { m.liveNote (note, 0.0f, false); }
        MainComponent& m;
    } kbListen { *this };
    juce::TextButton songBtn, metroBtn, countBtn, browserBtn, themeBtn, scopeBtn, vidPopBtn, saveBtn;
    int savedFlash = 0;
    void* routeWatch = nullptr;   // iOS headphone route observer
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
    juce::Image bgPicture;   // Background picture (desktop)
    juce::Image bgScaled, bgScaledFrom;   // bgPicture pre-scaled to the window (No Hitch)
    juce::TextButton closeAllBtn { "X" }, menuBtn { "MENU" }, importBtn { "+ IMPORT" }, copyBtn { "COPY" }, pasteBtn { "PASTE" }, altABtn { "ALT+A" }, altSBtn { "ALT+S" }, dupBtn { "CMD+B" }, delBtn { "DELETE" }, octUpBtn { "OCT+" }, octDnBtn { "OCT-" }, noteUpBtn { juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb2")) }, noteDnBtn { juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbc")) }, undoBtn { "UNDO" }, redoBtn { "REDO" }, dropBtn { "CLIPBOARD: DROP HERE" }, toolsBtn, keysBtn, moreBtn;
    bool toolsOpen = true, keysOpen = false, moreOpen = false;
    bool clipArmed = false; int clipView = -1;
    void phoneAlt (int key)
    {
        juce::Component* t = view == 1 ? playlist.get() : view == 0 ? piano.get() : nullptr;
        if (t == nullptr) { hintBar.setText ("ALT+A / ALT+S work on the BEAT and REC pages", juce::dontSendNotification); return; }
        if (! t->keyPressed (juce::KeyPress (key, juce::ModifierKeys::altModifier, 0)))
            hintBar.setText (key == 's' && view == 1 ? "Strum is for notes: open the pattern in BEAT, select notes, then ALT+S"
                                                     : "Select notes or audio clips first", juce::dontSendNotification);
    }
    void clearClipboardMode()
    {
        clipArmed = false; phoneClipTarget = nullptr; dropBtn.setVisible (false);
        copyBtn.setToggleState (false, juce::dontSendNotification); copyBtn.setButtonText ("COPY");
    }
    void phoneTranspose (int key, int mods)
    {
        if (view != 0) setView (0);   // notes live in the BEAT page's Piano Roll
        piano->keyPressed (juce::KeyPress (key, mods, 0));
    }
    void phoneDelete()
    {
        juce::Component* target = view == 1 ? playlist.get() : view == 0 ? piano.get() : nullptr;
        const auto& clips = engine.project().clips;
        if (view == 1 && std::none_of (clips.begin(), clips.end(), [] (const Clip& c) { return c.selected; }))
            target = nullptr;   // DELETE only removes clips, never a whole selected track
        if (target == nullptr || ! target->keyPressed (juce::KeyPress (juce::KeyPress::deleteKey)))
            hintBar.setText ("Select notes or clips first, then DELETE", juce::dontSendNotification);
    }
    void phoneDuplicate()
    {
        juce::Component* target = view == 1 ? playlist.get() : view == 0 ? piano.get() : nullptr;
        if (target == nullptr || ! target->keyPressed (juce::KeyPress ('b', juce::ModifierKeys::commandModifier, 0)))
            hintBar.setText ("Select notes or clips first, then CMD+B", juce::dontSendNotification);
    }
    void phoneRackToggle()   // phone: the Rack stays docked on BEAT; RACK shows / hides it (no floating window)
    {
        if (view != 0) { rackHidden = false; setView (0); }
        else { rackHidden = ! rackHidden; resized(); }
    }
    void phoneMixToggle() { setView (view == 2 ? lastView : 2); }
    void phoneCopyPaste (int key)
    {
        juce::Component* target = view == 1 ? playlist.get() : view == 0 ? piano.get() : nullptr;
        if (target == nullptr) return;
        int n = 0;
        if (view == 1) for (auto& c : studio.p().clips) n += c.selected ? 1 : 0;
        if (key == 'c' && clipArmed) { clearClipboardMode(); hintBar.setText ("Copy off", juce::dontSendNotification); return; }   // tap COPY again to turn it off
        if (key == 'c' && view == 1 && n == 0) { hintBar.setText ("Nothing selected: tap clips or patterns first, then COPY", juce::dontSendNotification); return; }
        const bool handled = target->keyPressed (juce::KeyPress (key, juce::ModifierKeys::commandModifier, 0));
        if (key == 'c' && ! handled) { hintBar.setText ("Nothing selected: select notes first, then COPY", juce::dontSendNotification); return; }
        if (key == 'c')
        {
            copyBtn.setToggleState (true, juce::dontSendNotification);
            copyBtn.setButtonText (view == 1 ? "COPIED " + juce::String (n) : "COPIED");
            clipArmed = true; phoneClipTarget = view == 1 ? (juce::Component*) playlist.get() : (juce::Component*) piano.get(); clipView = view; dropBtn.setVisible (false);
            hintBar.setText (view == 1 ? "Copied " + juce::String (n) + (n == 1 ? " clip" : " clips") + " - scroll, tap where they go, then tap CLIPBOARD"
                                       : "Copied the notes - scroll, tap where they go, then tap CLIPBOARD", juce::dontSendNotification);
        }
        else hintBar.setText ("Pasted at the playhead", juce::dontSendNotification);
    }
    std::unique_ptr<PhoneMenuList> phoneMenu;
    // below the notch / status bar and above the home bar, so the menu's X CLOSE can be tapped
    juce::Rectangle<int> phoneSafe (juce::Rectangle<int> r) const
    {
        if (auto* d = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds())) r = d->safeAreaInsets.subtractedFrom (r);
        return r;
    }
    void showMenuOrList (juce::PopupMenu m, const juce::PopupMenu::Options& o, std::function<void (int)> cb)
    {
        if (! phoneUI) { m.showMenuAsync (o, std::move (cb)); return; }
        phoneMenu = std::make_unique<PhoneMenuList> (m);
        juce::Component::SafePointer<MainComponent> safe (this);
        phoneMenu->done = [safe, cb] (int r) {
            juce::MessageManager::callAsync ([safe, cb, r] { if (safe == nullptr) return; safe->phoneMenu.reset(); if (cb) cb (r); });
        };
        addAndMakeVisible (*phoneMenu); phoneMenu->setBounds (phoneSafe (getLocalBounds())); phoneMenu->toFront (true);
    }
    void showAllMenus()
    {
        juce::PopupMenu m;
        const auto names = getMenuBarNames();
        for (int i = 0; i < names.size(); ++i)
        {
            if (! phoneUI) { m.addSubMenu (names[i], getMenuForIndex (i, names[i])); continue; }
            m.addSectionHeader (names[i]);   // phone: every item is one tap away (no sub-folders to open first)
            const auto sub = getMenuForIndex (i, names[i]);
            for (juce::PopupMenu::MenuItemIterator it (sub); it.next();) m.addItem (it.getItem());
        }
        juce::Component::SafePointer<MainComponent> safe (this);
        showMenuOrList (m, juce::PopupMenu::Options().withTargetComponent (&menuBtn).withStandardItemHeight (36),
                         [safe] (int r) { if (safe != nullptr && r != 0) safe->menuItemSelected (r, 0); });
        templateX (300, templateFiles());   // File > New from template: X deletes
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
                juce::StringArray share, media;
                juce::Array<juce::File> picked;
               #if JUCE_IOS
                for (auto& u : fc.getURLResults()) if (auto f = copyIntoApp (u); f.existsAsFile()) picked.add (f);
               #else
                picked = fc.getResults();
               #endif
                if (picked.isEmpty()) { safe->hintBar.setText ("Couldn't read that file. If it's in iCloud, open it once in Files to download it, then import again.", juce::dontSendNotification); return; }
                for (auto& f : picked)
                    if (f.existsAsFile()) (allHumHouseShareFiles ({ f.getFullPathName() }) ? share : media).add (f.getFullPathName());
                if (! share.isEmpty()) safe->filesDropped (share, 0, 0);
                if (auto* t = dynamic_cast<juce::FileDragAndDropTarget*> (safe->playlist.get()); t != nullptr && ! media.isEmpty())
                {
                    safe->setView (1);
                    t->filesDropped (media, -1, -1);   // at the playhead, on new tracks
                }
            });
    }
    void closeEverything()
    {
        juce::PopupMenu::dismissAllActiveMenus();
        if (phoneUI) clearClipboardMode();
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
    int view = 0, ticks = 0, lastView = 0; bool rackHidden = false;
    bool showBrowser = ! phoneUI;
    juce::TextButton browserTab { ">" };
    juce::TextButton laneBtn, lanePop;
    bool showLanes = false;
    juce::String flashText;
    juce::uint32 flashUntil = 0;

    static juce::File docsDir() { return hhsHome(); }   // iPhone: Files > On My iPhone > HumHouse Studio
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
                if (const bool want = juce::Process::isForegroundProcess(); want != isAlwaysOnTop()) setOnTopSafely (*this, want);
            fsBtn.setButtonText (isFull() ? "EXIT FULL" : "FULL SCREEN");
        }
    };
    std::map<juce::Component*, std::unique_ptr<PopOut>> popouts;

    void toggleDetach (juce::Component* c, const juce::String& title)
    {
        if (popouts.count (c) > 0) { reattach (c); return; }
        if (phoneUI)   // phone: panels always dock into the page (no floating windows to lose behind it)
        {
            if (c == mixer.get()) setView (2);
            else if (c == lanes.get()) { showLanes = true; laneBtn.setToggleState (true, juce::dontSendNotification); resized(); }
            return;
        }
        auto w = std::make_unique<PopOut> (title);
        w->setUsingNativeTitleBar (! phoneUI);
        useHumHouseTitleBar (*w, juce::DocumentWindow::allButtons);
        w->setContentNonOwned (c, false);
        c->setVisible (true);
        w->setResizable (true, true);
        w->setResizeLimits (360, 220, 10000, 8000);
        w->centreWithSize (juce::jmax (900, c->getWidth()), juce::jmax (520, c->getHeight()));
        joinFullScreen (*w);   // Mac full screen: the pop-out opens inside it (pop Mixer), not on its own Space like the MIX page
        w->setVisible (true);
        joinFullScreen (*w); w->toFront (true);
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
        if (phoneUI && v == 4) return;   // phone: no VIDEO page
        if (phoneLite && v == 3) return;   // stripped phone app: no Patch Bay
        if (phoneUI)
        {
            pagesBtn.setButtonText (v == 3 ? "PATCH" : "PAGES");
            beatPg.setToggleState (v == 0, juce::dontSendNotification); recPg.setToggleState (v == 1, juce::dontSendNotification); mixPg.setToggleState (v == 2, juce::dontSendNotification);
        }
        if (view != 2) lastView = view;
        view = v;
        juce::TextButton* tabs[] { &beatTab, &recordTab, &mixTab, &patchTab, &videoTab };
        if (v == 3 && patchView == nullptr) { patchView = createPatchBayView (studio); addChildComponent (*patchView); }
        if (v == 4 && videoView == nullptr) { videoView = createVideoView (studio); addChildComponent (*videoView); }
        for (int i = 0; i < 5; ++i) tabs[i]->setToggleState (i == v, juce::dontSendNotification);
        if (phoneUI)   // phone: never leave a floating Rack / Mixer window over the page after switching
        {
            std::vector<juce::Component*> pops;
            for (auto& p : popouts) pops.push_back (p.first);
            for (auto* c : pops) reattach (c);
        }
        resized();
        repaint();
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
        const int cpu = juce::jlimit (0, 100, juce::roundToInt (engine.cpuLoad() * 100.0));
        cpuLabel.setText ("CPU " + juce::String (cpu) + "%", juce::dontSendNotification);   // just the %, no late count
        const auto cc = cpu > 80 ? col::rec : cpu > 50 ? col::accent : col::good;
        if (cpuLabel.findColour (juce::Label::textColourId) != cc) cpuLabel.setColour (juce::Label::textColourId, cc);
        if (++coreTick >= 20)   // per-core + per-insert CPU in the CPU tooltip
        {
            coreTick = 0;
            juce::String tip ("CPU load of the audio engine. Freeze heavy tracks in the Mixer to save CPU");
            const auto cores = engine.coreLoads();
            if (! cores.empty())
            {
                tip << "\nPer thread:";
                for (size_t i = 0; i < cores.size(); ++i)
                    tip << (i % 4 == 0 ? "\n" : "   ") << (i + 1 == cores.size() ? juce::String ("Audio") : "Core " + juce::String ((int) i + 1))
                        << " " << juce::roundToInt (cores[i] * 100.0f) << "%";
            }
            std::vector<std::pair<float, int>> hv;
            const auto& mx = engine.project().mixer;
            for (int t = 0; t < (int) mx.size(); ++t) if (const float l = engine.trackLoad (t); l > 0.005f) hv.push_back ({ l, t });
            std::sort (hv.rbegin(), hv.rend());
            if (! hv.empty())
            {
                tip << "\nHeaviest inserts:";
                for (size_t i = 0; i < hv.size() && i < 4; ++i)
                    tip << "\n" << (hv[i].second == 0 ? juce::String ("Master") : juce::String (hv[i].second) + " " + mx[(size_t) hv[i].second].name)
                        << "  " << juce::roundToInt (hv[i].first * 100.0f) << "%";
            }
            if (engine.lowLatency.load()) tip << "\nLow Latency Mode on";
            cpuLabel.setTooltip (tip);
        }
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

        if (++ticks >= 30 * 120 && ! engine.isPlaying())   // overdue backups wait for Stop, like Logic / Reaper
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
        bool ok = saveProject (p, p.projectFile);
       #if JUCE_IOS
        if (! ok)   // never lose a song: fall back to a fresh file in Files > On My iPhone > HumHouse Studio
        {
            docsDir().createDirectory();
            p.projectFile = docsDir().getChildFile (juce::File::createLegalFileName (p.title.isEmpty() ? juce::String ("Untitled") : p.title) + ".hhs").getNonexistentSibling (true);
            collectAudio();
            ok = saveProject (p, p.projectFile);
        }
       #endif
        if (ok)
        {
            writeBackup (p);
            studio.dirty = false;
            syncControls();
            flash ("Saved " + p.projectFile.getFileName() + " + backup");
            saveBtn.setButtonText ("SAVED"); saveBtn.setToggleState (true, juce::dontSendNotification);   // opposite color = confirmed
            savedFlash = 45;
        }
        else juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Save",
                                                     "Could not save the project to\n" + p.projectFile.getFullPathName()
                                                         + "\n\nA backup was kept. Try Save As with another name, or free up space.");
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
        bool inPlace = ! as && p.projectFile.existsAsFile();
       #if JUCE_IOS
        inPlace = inPlace && p.projectFile.isAChildOf (docsDir());   // opened from elsewhere: save a copy into the app's folder
       #endif
        if (inPlace) { writeProject(); if (after && ! studio.dirty) after(); return; }
        docsDir().createDirectory();
       #if JUCE_IOS
        // iPhone: the Files save sheet hands back a location the app can't write to (false "Could not save"),
        // so projects save straight into the app's folder, which the Files app shows
        auto* w = new juce::AlertWindow ("Save project", "Name your song (it saves to Files > On My iPhone > HumHouse Studio)", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", p.title);
        w->addButton ("SAVE", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<MainComponent> safe (this);
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, after] (int r) {
            if (safe == nullptr || r != 1) return;
            auto name = juce::File::createLegalFileName (w->getTextEditorContents ("name").trim());
            if (name.isEmpty()) name = "Untitled";
            auto f = docsDir().getChildFile (name + ".hhs");
            if (f.existsAsFile() && f != safe->engine.project().projectFile) f = f.getNonexistentSibling (true);   // never overwrite another song
            safe->engine.project().projectFile = f;
            safe->engine.project().title = f.getFileNameWithoutExtension();
            safe->writeProject();
            if (after && ! safe->studio.dirty) after();
        }), true);
        return;
       #endif
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

    // Ableton / Reaper / FL plugins: the installed plugin with that name (effects never match an instrument)
    juce::String findInstalledPlugin (const juce::String& name, bool instrument)
    {
        const auto want = name.trim().toLowerCase();
        juce::String loose;
        for (auto& d : engine.knownPlugins.getTypes())
        {
            if (! instrument && d.isInstrument) continue;
            const auto n = d.name.trim().toLowerCase();
            if (n == want) return d.createIdentifierString();
            if (loose.isEmpty() && want.isNotEmpty() && (n.startsWith (want) || want.startsWith (n))) loose = d.createIdentifierString();
        }
        return loose;
    }
    void convertStems()
    {
        chooser = std::make_unique<juce::FileChooser> ("Pick the stems (audio files) or their folder (Logic / Pro Tools / any DAW)", docsDir(), "*");
        juce::Component::SafePointer<MainComponent> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories
                                | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                              [safe] (const juce::FileChooser& fc) {
                                  juce::Array<juce::File> picked;
                                 #if JUCE_IOS
                                  for (auto& u : fc.getURLResults()) if (auto f = copyIntoApp (u); f.exists()) picked.add (f);
                                 #else
                                  picked = fc.getResults();
                                 #endif
                                  if (safe == nullptr || picked.isEmpty()) return;
                                  if (picked.size() == 1 && picked[0].isDirectory()) { const auto d = picked[0]; safe->askToSave ([safe, d] { if (safe != nullptr) safe->load (d); }); return; }
                                  safe->askToSave ([safe, picked] { if (safe != nullptr) safe->load (picked[0], picked); });
                              });
    }

    void load (const juce::File& f, const juce::Array<juce::File>& stems = {})
    {
        Project q;
        juce::StringArray converted;
        if (! stems.isEmpty() || f.isDirectory() || isForeignProject (f))
        {
            const bool ok = ! stems.isEmpty() ? importStemFiles (stems, q, converted)
                          : f.isDirectory() ? importStemFolder (f, q, converted)
                                            : importForeignProject (f, q, converted, [this] (const juce::String& n, bool inst) { return findInstalledPlugin (n, inst); });
            if (! ok)
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Convert", f.isDirectory() ? "No audio files in that folder." : "Couldn't read that project file.");
                return;
            }
            q.projectFile = juce::File();   // Save as... writes the converted song as a .hhs with its audio inside
        }
        else if (! loadProject (q, f))
        {
            const auto why = unreadableReason (f);
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Open", why.isNotEmpty() ? why : "Not a HumHouse project, or it's damaged: " + f.getFileName() + "\nTry File > Open backup.");
            return;
        }
        if (f.getParentDirectory().getFileName() == "Backup") q.projectFile = juce::File();
        closeAllEditorWindows();
        engine.newProject (std::move (q));
        studio.selectedTrack = 1; studio.selectedLanes.clear(); studio.laneAnchor = -1;
        studio.undo.clear();
        studio.undo.push (engine.project(), "Open");
        studio.selectedChannel = engine.project().channels.empty() ? -1 : engine.project().channels.front().id;
        studio.dirty = ! converted.isEmpty();
        if (! converted.isEmpty())
        {
            hintBar.setText ("Converted " + f.getFileName() + ": " + converted.joinIntoString ("  |  "), juce::dontSendNotification);
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Converted " + f.getFileName(), converted.joinIntoString ("\n") + "\n\nSave as... to keep it as a HumHouse song.");
        }
        studio.sendChangeMessage();
        juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<MainComponent> (this)] { if (sp) { sp->collectAudio(); sp->locateMissingAudio(); } });   // glue: copy outside audio into the song folder
    }

    void open (bool backups) { askToSave ([this, backups] { openChooser (backups); }); }
   #if JUCE_IOS
    // Files hands back a temporary security-scoped link: copy the file into the app's folder while it's readable
    static juce::File copyIntoApp (const juce::URL& u)
    {
        const auto local = u.getLocalFile();
        if (local.isAChildOf (docsDir())) return local;
        auto name = juce::URL::removeEscapeChars (u.getFileName());
        if (name.isEmpty()) name = local.getFileName();
        auto dir = docsDir().getChildFile ("Imported");
        dir.createDirectory();
        auto dest = dir.getChildFile (juce::File::createLegalFileName (name));
        if (auto in = u.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)))
        {
            const auto tmp = dir.getChildFile (dest.getFileName() + ".part");   // streamed, so big recordings never sit in memory
            {
                juce::FileOutputStream out (tmp);
                if (out.openedOk()) { out.setPosition (0); out.truncate(); }
                const auto total = in->getTotalLength();
                const auto copied = out.openedOk() ? out.writeFromInputStream (*in, -1) : (juce::int64) -1;
                out.flush();
                if (copied < 0 || ! out.getStatus().wasOk() || (total >= 0 && copied != total))   // phone full mid-copy: never keep half a file
                { tmp.deleteFile(); return local.existsAsFile() ? local : juce::File(); }
            }
            // same file again (under its name or an earlier "name (2)"): reuse it; a different file with that name: keep both
            for (int i = 1; i <= 100; ++i)
            {
                const auto c = i == 1 ? dest : dir.getChildFile (dest.getFileNameWithoutExtension() + " (" + juce::String (i) + ")" + dest.getFileExtension());
                if (c.existsAsFile() && c.hasIdenticalContentTo (tmp)) { tmp.deleteFile(); return c; }
            }
            if (dest.existsAsFile()) dest = dest.getNonexistentSibling (true);
            if (tmp.moveFileTo (dest)) return dest;
            tmp.deleteFile();
        }
        return local.existsAsFile() ? local : juce::File();
    }
    void openChooser (bool backups)
    {
        juce::Array<juce::File> found;
        auto scan = [&found] (const juce::File& d, bool deep) { if (d.isDirectory()) for (auto& f : d.findChildFiles (juce::File::findFiles, deep, "*.hhs")) if (! found.contains (f)) found.add (f); };
        const auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
        scan (docsDir(), true); scan (docs, false); scan (docs.getChildFile ("Inbox"), true);
        scan (juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("HumHouse Studio"), true);   // songs from older builds
        juce::Array<juce::File> songs;
        for (auto& f : found)
            if (! f.isAChildOf (templatesDir()) && ! f.hasFileExtension ("tmp") && (f.getParentDirectory().getFileName() == "Backup") == backups) songs.add (f);
        std::sort (songs.begin(), songs.end(), [] (const juce::File& a, const juce::File& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
        juce::PopupMenu m;
        m.addSectionHeader (backups ? "Backups - newest first" : "Your songs - newest first");
        for (int i = 0; i < juce::jmin (300, songs.size()); ++i)
            m.addItem (i + 1, songs[i].getFileNameWithoutExtension() + "    " + songs[i].getLastModificationTime().toString (true, true, false));
        if (songs.isEmpty()) m.addItem (-1, backups ? "No backups yet" : "No songs saved yet", false);
        m.addSeparator();
        m.addItem (100000, "Browse Files (iCloud, Downloads, other apps)...");
        juce::Component::SafePointer<MainComponent> safe (this);
        showMenuOrList (m, juce::PopupMenu::Options().withTargetComponent (&menuBtn).withStandardItemHeight (40), [safe, songs] (int r) {
            if (safe == nullptr || r == 0) return;
            if (r >= 1 && r <= songs.size()) { safe->load (songs[r - 1]); return; }
            if (r != 100000) return;
            safe->chooser = std::make_unique<juce::FileChooser> ("Open project", docsDir(), "*.hhs;*.rpp;*.als;*.flp");
            safe->chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc) {
                if (safe == nullptr || fc.getURLResults().isEmpty()) return;
                if (auto f = copyIntoApp (fc.getURLResults()[0]); f.existsAsFile()) safe->load (f);
            });
        });
        if (phoneMenu != nullptr)   // phone: X on each song deletes it (asks first)
        {
            const int n = juce::jmin (300, songs.size());
            phoneMenu->canDelete = [n] (int id) { return id >= 1 && id <= n; };
            phoneMenu->onDelete = [safe, songs, backups] (int id, std::function<void()> removeRow)
            {
                const auto f = songs[id - 1];
                juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon).withTitle ("Delete \"" + f.getFileNameWithoutExtension() + "\"?")
                                                  .withMessage (backups ? "This backup is deleted from the phone." : "Are you sure? This song is deleted from the phone.").withButton ("Delete").withButton ("Cancel"),
                                              [safe, f, removeRow] (int b) {
                                                  if (b != 1 || safe == nullptr) return;
                                                  if (f.moveToTrash() || f.deleteFile() || ! f.existsAsFile()) { removeRow(); safe->flash ("Deleted: " + f.getFileNameWithoutExtension()); }
                                                  else safe->flash ("Couldn't delete " + f.getFileNameWithoutExtension());
                                              });
            };
            phoneMenu->repaint();
        }
    }
   #else
    void openChooser (bool backups)
    {
        auto dir = backups ? backupDir() : docsDir();
        chooser = std::make_unique<juce::FileChooser> (backups ? "Open backup" : "Open project (or Reaper .rpp, Ableton .als, FL .flp)", dir, backups ? "*.hhs" : "*.hhs;*.rpp;*.als;*.flp");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) load (fc.getResult()); });
    }
   #endif

    static juce::File templatesDir() { return docsDir().getChildFile ("Templates"); }
    // phone: templates live in the app (Files > On My iPhone > HumHouse Studio > Templates); open, delete or clear them here
    void phoneTemplates()
    {
        juce::PopupMenu m;
        m.addSectionHeader ("TEMPLATES ON THIS PHONE");
        auto ts = templateFiles();
        ts.removeIf ([] (const juce::File& f) { return ! f.isAChildOf (templatesDir()); });   // only the app's own Templates folder: a song folder added as a source is never deleted here
        for (int i = 0; i < ts.size() && i < 90; ++i) m.addItem (1000 + i, ts[i].getFileNameWithoutExtension());   // tap = new song from it, X = delete
        if (ts.isEmpty()) m.addItem (-1, "(no templates yet)", false);
        m.addSeparator(); m.addItem (3000, "Delete all templates", ! ts.isEmpty());
        showMenuOrList (m, {}, [this, ts] (int r) {
            if (r >= 1000 && r < 1000 + ts.size()) newProject (ts[r - 1000]);
            else if (r >= 2000 && r < 2000 + ts.size())
            {
                const auto f = ts[r - 2000];
                juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon).withTitle ("Delete \"" + f.getFileNameWithoutExtension() + "\"?")
                                                  .withMessage ("Are you sure? This template is deleted from the phone. Your songs stay.").withButton ("Delete").withButton ("Cancel"),
                                              [this, f] (int b) { if (b == 1 && f.deleteFile()) flash ("Template deleted: " + f.getFileNameWithoutExtension()); });
            }
            else if (r == 3000)
                juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon).withTitle ("Delete all templates?")
                                                  .withMessage (juce::String (ts.size()) + " templates on this phone will be deleted. Your songs stay.").withButton ("Delete").withButton ("Cancel"),
                                              [this, ts] (int b) { if (b != 1) return; for (auto& f : ts) f.deleteFile(); flash ("Templates cleared"); });
        });
        templateX (1000, ts);
    }
    // phone: an X next to each template row deletes it (asks first); base = the rows' first menu id
    void templateX (int base, const juce::Array<juce::File>& ts)
    {
        if (phoneMenu == nullptr) return;
        phoneMenu->canDelete = [base, ts] (int id) { return id >= base && id < base + juce::jmin (90, ts.size()) && ts[id - base].isAChildOf (templatesDir()); };
        juce::Component::SafePointer<MainComponent> safe (this);
        phoneMenu->onDelete = [safe, base, ts] (int id, std::function<void()> removeRow)
        {
            const auto f = ts[id - base];
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon).withTitle ("Delete \"" + f.getFileNameWithoutExtension() + "\"?")
                                              .withMessage ("Are you sure? This template is deleted from the phone. Your songs stay.").withButton ("Delete").withButton ("Cancel"),
                                          [safe, f, removeRow, base] (int b) {
                                              if (b != 1 || safe == nullptr) return;
                                              if (f.deleteFile() || ! f.existsAsFile())
                                              {   // File menu rows are looked up by position again, so that menu closes instead of shifting
                                                  if (base == 300 && safe->phoneMenu != nullptr && safe->phoneMenu->done) safe->phoneMenu->done (0); else removeRow();
                                                  safe->flash ("Template deleted: " + f.getFileNameWithoutExtension());
                                              }
                                              else safe->flash ("Couldn't delete " + f.getFileNameWithoutExtension());
                                          });
        };
    }
    static juce::File defaultTemplate() { return templatesDir().getChildFile ("_default.hhs"); }
    static bool inBackupFolder (const juce::File& f) { for (auto d = f.getParentDirectory(); d != d.getParentDirectory(); d = d.getParentDirectory()) if (d.getFileName() == "Backup") return true; return false; }
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
                if (f.getFileName() != "_default.hhs" && ! inBackupFolder (f) && ! fs.contains (f)) fs.add (f);   // "My Backup Beat" is still a template
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
        if (saveProject (engine.project(), tmp) && tmp.moveFileTo (f)) { lastTemplate = name; flash ("Saved template " + name); }   // same name = replace, never half-written
        else { tmp.deleteFile(); juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Template", "Could not save the template to\n" + f.getFullPathName()); }
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
        auto fail = [f] (const juce::String& what) {
            const auto why = unreadableReason (f);
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Open", why.isNotEmpty() ? why : "That file isn't a HumHouse " + what + ".");
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
        else if (f.hasFileExtension ("hhs") || isForeignProject (f))
            askToSave ([this, f] { load (f); });
        else if (f.hasFileExtension ("ptx;ptf;logicx;aaf;omf"))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Convert " + f.getFileName(),
                "Pro Tools and Logic sessions are closed formats. Export every track as audio from that DAW "
                "(Logic: File > Export > All Tracks as Audio Files, Pro Tools: File > Export > Clips as Files), "
                "then drop the stems folder or File > Convert stems: each file becomes its own track + mixer insert.");
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
       #if JUCE_IOS
        {   // iPhone can't pick a folder: exports go to Files > On My iPhone > HumHouse Studio > Exports
            auto dir = hhsHome().getChildFile ("Exports");
            if (! dir.createDirectory().wasOk() || ! dir.isDirectory()) dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
            if (! dir.isDirectory())
            { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Export", "Can't open the Exports folder in Files."); return; }
            hintBar.setText ("Exporting to Files > On My iPhone > HumHouse Studio > Exports", juce::dontSendNotification);
            exportInto (dir, name, what, track);
            return;
        }
       #endif
        juce::File start (studio.settings ? studio.settings->getValue ("exportDir") : juce::String());
        if (! start.isDirectory()) start = docsDir();
        chooser = std::make_unique<juce::FileChooser> ("Choose the export folder", start);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this, name, what, track] (const juce::FileChooser& fc) {
                                  const auto dir = fc.getResult();
                                  if (! dir.isDirectory()) return;
                                  if (studio.settings) { studio.settings->setValue ("exportDir", dir.getFullPathName()); studio.settings->saveIfNeeded(); }
                                  exportInto (dir, name, what, track);
                              });
    }

    void exportInto (const juce::File& dir, const juce::String& name, int what, int track)
    {
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
    }

    void showAudioSettings()
    {
        if (phoneUI) { showPhoneLatency(); return; }
        auto* sel = new juce::AudioDeviceSelectorComponent (engine.deviceManager, 0, 32, 0, 2, true, false, true, false);
        sel->setSize (560, 480);
        static juce::Component::SafePointer<juce::DocumentWindow> open;
        if (open != nullptr) { open->toFront (true); delete sel; return; }
        open = openToolWindow ("Audio settings - device, inputs, buffer size, sample rate", fitToScreen (sel), true);
    }

    // phone: pick the buffer from a list (the desktop device panel crashed on iPhone); lower = less delay
    void showPhoneLatency()
    {
        auto* dev = engine.deviceManager.getCurrentAudioDevice();
        const int cur = dev != nullptr ? dev->getCurrentBufferSizeSamples() : 0;
        const double sr = dev != nullptr && dev->getCurrentSampleRate() > 0 ? dev->getCurrentSampleRate() : 48000.0;
        juce::Array<int> sizes;
        if (dev != nullptr) for (int b : dev->getAvailableBufferSizes()) if (b >= 32 && b <= 2048) sizes.addIfNotAlreadyThere (b);
        if (sizes.isEmpty()) sizes = { 64, 128, 256, 512, 1024 };
        juce::PopupMenu m;
        m.addSectionHeader ("Latency: lower = faster, higher = no crackles");
        for (int b : sizes)
            m.addItem (b, juce::String (b) + " samples  (" + juce::String (1000.0 * b / sr, 1) + " ms)" + (b <= 128 ? "  fastest" : b == 256 ? "  recommended" : ""), true, b == cur);
        juce::Component::SafePointer<MainComponent> safe (this);
        showMenuOrList (m, juce::PopupMenu::Options().withTargetComponent (&menuBtn).withStandardItemHeight (40), [safe] (int b) {
            if (safe == nullptr || b <= 0) return;
            juce::MessageManager::callAsync ([safe, b] {   // after the menu is gone: the device restarts cleanly
                if (safe == nullptr) return;
                auto& dm = safe->engine.deviceManager;
                auto setup = dm.getAudioDeviceSetup();
                if (setup.bufferSize == b) return;
                setup.bufferSize = b;
                const auto err = dm.setAudioDeviceSetup (setup, true);
                if (auto x = dm.createStateXml()) safe->studio.settings->setValue ("audio", x.get());
                safe->hintBar.setText (err.isEmpty() ? "Buffer " + juce::String (b) + " samples" : "Couldn't change the buffer: " + err, juce::dontSendNotification);
            });
        });
    }

    struct ShortcutsWindow : juce::DocumentWindow
    {
        // top bar: + ADD SHORTCUT (any menu command, press a key) and EDIT KEYS (change / clear / reset any key)
        struct Pane : juce::Component
        {
            Pane (juce::TextEditor* t, MainComponent& m) : text (t), main (m)
            {
                setWantsKeyboardFocus (true);
                for (auto* b : { &add, &edit }) { b->setWantsKeyboardFocus (false); b->setColour (juce::TextButton::buttonColourId, col::accent.withAlpha (0.85f)); addAndMakeVisible (b); }
                add.setTooltip ("Pick any menu command, then press the key you want for it");
                edit.setTooltip ("Change, clear or reset the key of any command");
                add.onClick = [this] { pickCommand(); };
                edit.onClick = [this] { main.showKeyCommands(); };
                status.setColour (juce::Label::textColourId, col::accent2); addAndMakeVisible (status);
                addAndMakeVisible (text.get());
                showMine();
            }
            void resized() override
            {
                auto r = getLocalBounds();
                auto bar = r.removeFromTop (38).reduced (6, 4);
                add.setBounds (bar.removeFromLeft (150)); bar.removeFromLeft (6); edit.setBounds (bar.removeFromLeft (110)); bar.removeFromLeft (8);
                status.setBounds (bar);
                text->setBounds (r);
            }
            void showMine()
            {
                juce::String mine;
                for (auto& [id, k] : main.userKeys)
                    if (id.startsWith ("menu:") && k.isValid()) mine << k.getTextDescriptionWithIcons() << " = " << id.fromFirstOccurrenceOf ("menu:", false, false) << "\n";
                status.setText (waitingFor.isNotEmpty() ? "Press the key for \"" + waitingFor + "\"  (Esc = cancel)"
                                : mine.isEmpty() ? juce::String ("+ ADD SHORTCUT puts a key on any menu command") : juce::String ("Your added shortcuts are listed at the top"), juce::dontSendNotification);
                auto t = base.isEmpty() ? (base = text->getText()) : base;
                text->setText (mine.isEmpty() ? t : "YOUR SHORTCUTS (Edit keys to change or clear)\n" + mine + "\n" + t, false);
            }
            void pickCommand()
            {
                juce::PopupMenu m;
                const auto names = main.getMenuBarNames();
                for (int i = 0; i < names.size(); ++i) m.addSubMenu (names[i], main.getMenuForIndex (i, names[i]));
                juce::Component::SafePointer<Pane> safe (this);
                main.showMenuOrList (m, juce::PopupMenu::Options().withTargetComponent (&add).withStandardItemHeight (30), [safe] (int r)
                {
                    if (safe == nullptr || r == 0) return;
                    safe->waitingFor = safe->main.menuTextFor (r);
                    safe->showMine(); safe->grabKeyboardFocus();
                });
            }
            bool keyPressed (const juce::KeyPress& k) override
            {
                if (waitingFor.isEmpty()) return false;
                if (k != juce::KeyPress (juce::KeyPress::escapeKey))
                {
                    const auto clash = main.commandUsing (k, "menu:" + waitingFor);
                    if (clash.isNotEmpty()) { status.setText (k.getTextDescriptionWithIcons() + " is already " + clash + ". Pick another key or change that one in Edit keys.", juce::dontSendNotification); return true; }
                    if (waitingFor.containsAnyOf ("=;")) { status.setText ("That command can't take a shortcut.", juce::dontSendNotification); waitingFor = {}; return true; }
                    main.userKeys["menu:" + waitingFor] = k; main.saveKeys();
                }
                waitingFor = {}; showMine(); return true;
            }
            std::unique_ptr<juce::TextEditor> text; MainComponent& main;
            juce::TextButton add { "+ ADD SHORTCUT" }, edit { "EDIT KEYS" }; juce::Label status; juce::String waitingFor, base;
        };
        ShortcutsWindow (const juce::String& text, std::function<void()> close, MainComponent& main)
            : juce::DocumentWindow ("Shortcuts  (Cmd/Ctrl+U to close)", col::panel, juce::DocumentWindow::closeButton), onClose (std::move (close))
        {
            auto* ed = new juce::TextEditor();
            ed->setMultiLine (true); ed->setReadOnly (true); ed->setCaretVisible (false); ed->setScrollbarsShown (true);
            ed->setFont (juce::FontOptions (14.0f)); ed->setText (text, false);
            ed->setColour (juce::TextEditor::backgroundColourId, col::bg); ed->setColour (juce::TextEditor::textColourId, col::text);
            ed->setColour (juce::TextEditor::outlineColourId, col::panel2);
            setContentOwned (new Pane (ed, main), false);
            setUsingNativeTitleBar (! phoneUI); useHumHouseTitleBar (*this); setResizable (true, false); centreWithSize (860, 620); setVisible (true);
        }
        void closeButtonPressed() override { if (onClose) onClose(); }
        bool keyPressed (const juce::KeyPress& k) override
        {
            if (auto* p = dynamic_cast<Pane*> (getContentComponent()); p != nullptr && p->waitingFor.isNotEmpty()) return p->keyPressed (k);
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
        shortcutsWin = std::make_unique<ShortcutsWindow> (t, [this] { juce::MessageManager::callAsync ([this] { shortcutsWin.reset(); }); }, *this);
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
