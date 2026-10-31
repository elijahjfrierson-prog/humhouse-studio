#pragma once
#include <map>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "Engine.h"
#include "ProjectIO.h"
#include "NoteTools.h"

namespace hhs
{
#if JUCE_IOS
inline constexpr bool phoneUI = true;    // touch layout: wrapping top bar, one MENU folder, X close-all, finger panning
#else
inline constexpr bool phoneUI = false;
#endif

namespace col
{
    // mutable so the theme color wheel can recolor the whole DAW
    inline juce::Colour bg (0xff0e1013), panel (0xff16191e), panel2 (0xff1d2127), grid (0xff22262d), gridBeat (0xff2c3139),
        gridBar (0xff3c434e), text (0xffdfe4ec), dim (0xff7f8896), accent (0xffff8a3d), accent2 (0xff38d1ff),
        rec (0xffff3b57), good (0xff3ddc84), scale (0xff1a2721), root (0xff21392d);
    inline bool lightMode = false;                          // White theme: light backgrounds, dark text
    inline juce::Colour fontColour (0x00000000);            // custom text color (transparent = automatic)
    // meters + scopes gradient (quiet -> loud), set in Theme
    inline juce::Colour gradLo (0xff3ddc84), gradMid (0xffffd23d), gradHi (0xffff3b57);
    // bottom (quiet) to top (loud), like the mixer volume meters
    inline juce::ColourGradient levelGradient (float x, float yTop, float yBottom, float alpha = 1.0f)
    {
        juce::ColourGradient gr (gradHi.withMultipliedAlpha (alpha), x, yTop, gradLo.withMultipliedAlpha (alpha), x, yBottom, false);
        gr.addColour (0.45, gradMid.withMultipliedAlpha (alpha));
        return gr;
    }
    // mirrored around the centre line for waveforms: quiet near the middle, hot at the peaks
    inline juce::ColourGradient mirrorGradient (float x, float yTop, float yBottom, float alpha = 1.0f)
    {
        juce::ColourGradient gr (gradHi.withMultipliedAlpha (alpha), x, yTop, gradHi.withMultipliedAlpha (alpha), x, yBottom, false);
        gr.addColour (0.22, gradMid.withMultipliedAlpha (alpha)); gr.addColour (0.5, gradLo.withMultipliedAlpha (alpha)); gr.addColour (0.78, gradMid.withMultipliedAlpha (alpha));
        return gr;
    }
    // font gradient (Theme > Font: Gradient): headline text gets a left-to-right lo -> mid -> hi fill
    inline bool fontGradient = false;
    inline juce::Colour fontLo (0xff3ddc84), fontMid (0xffffd23d), fontHi (0xffff3b57);
    // big / bold text only; small plain text stays solid so it reads
    inline bool isHeadline (const juce::Font& f) { return f.getHeight() >= 13.0f || (f.isBold() && f.getHeight() >= 11.0f); }
    // White theme: darken the stops so they read on white backgrounds
    inline juce::Colour fontStop (juce::Colour c)
    {
        // every stop stays far from the background: deep on the white theme, bright on the dark one
        return lightMode ? juce::Colour::fromHSV (c.getHue(), juce::jmin (1.0f, c.getSaturation() * 1.2f), 0.08f + c.getBrightness() * 0.2f, c.getFloatAlpha())
                         : juce::Colour::fromHSV (c.getHue(), c.getSaturation(), juce::jmax (0.8f, c.getBrightness()), c.getFloatAlpha());
    }
    // keep `text` unless it would blend into `bg` (similar brightness); then switch to a contrasting color
    inline juce::Colour readableOn (juce::Colour text, juce::Colour bg)
    {
        if (bg.getAlpha() < 40 || std::abs (text.getPerceivedBrightness() - bg.getPerceivedBrightness()) >= 0.33f) return text;
        return bg.getPerceivedBrightness() > 0.55f ? juce::Colour (0xff0d0f12).withAlpha (text.getFloatAlpha()) : juce::Colour (0xfff4f6fa).withAlpha (text.getFloatAlpha());
    }
    // text sitting on a color: the opposite hue, dark on light colors and bright on dark ones
    inline juce::Colour contrastOn (juce::Colour bg)
    {
        const bool light = bg.getPerceivedBrightness() > 0.6f;
        return juce::Colour::fromHSV (std::fmod (bg.getHue() + 0.5f, 1.0f), juce::jmin (0.45f, bg.getSaturation()), light ? 0.1f : 0.98f, 1.0f);
    }

    inline juce::Font laneNameFont (const juce::String& f, float h)
    {
        if (f == "Bold")   return juce::FontOptions (h, juce::Font::bold);
        if (f == "Italic") return juce::FontOptions (h, juce::Font::italic);
        if (f == "Mono")   return juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), h, juce::Font::plain);
        if (f == "Serif")  return juce::FontOptions (juce::Font::getDefaultSerifFontName(), h, juce::Font::plain);
        if (f == "Sans")   return juce::FontOptions (juce::Font::getDefaultSansSerifFontName(), h, juce::Font::bold);
        return juce::FontOptions (h);
    }

    inline juce::ColourGradient fontGradientFill (float x0, float x1, float alpha = 1.0f)
    {
        juce::ColourGradient gr (fontStop (fontLo).withMultipliedAlpha (alpha), x0, 0, fontStop (fontHi).withMultipliedAlpha (alpha), juce::jmax (x0 + 1.0f, x1), 0, false);
        gr.addColour (0.5, fontStop (fontMid).withMultipliedAlpha (alpha));
        return gr;
    }
    // outline behind gradient text in the theme's background tone, so it still reads where it sits on a colored bar
    inline juce::Colour fontHalo (float alpha = 1.0f)
    {
        return (lightMode ? juce::Colours::white.withAlpha (0.85f) : juce::Colours::black.withAlpha (0.85f)).withMultipliedAlpha (alpha);
    }
    template <typename Draw> inline void drawWithHalo (juce::Graphics& g, float alpha, Draw&& draw)
    {
        g.saveState(); g.setColour (fontHalo (alpha));
        for (auto [dx, dy] : { std::pair { -1, 0 }, std::pair { 1, 0 }, std::pair { 0, -1 }, std::pair { 0, 1 } }) draw (dx, dy);
        g.restoreState();
    }
    // fill for one line of text in `area`: the gradient spans just the glyphs, so short words still show all three colours
    inline void setFontGradientFill (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Justification j, float alpha = 1.0f)
    {
        const float w = juce::jmin (area.getWidth(), juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text));
        const float x = j.testFlags (juce::Justification::right) ? area.getRight() - w
                      : j.testFlags (juce::Justification::horizontallyCentred) ? area.getCentreX() - w * 0.5f : area.getX();
        g.setGradientFill (fontGradientFill (x, x + w, alpha));
    }
    // gradient text as one glyph path: macOS fills a gradient glyph-by-glyph and a space (empty outline) leaves the clip open,
    // painting the whole bar in the gradient - so build the outline ourselves and fill only the letters
    inline void fillGradientText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, juce::Justification j,
                                  int lines, float minScale, float alpha = 1.0f)
    {
        juce::GlyphArrangement ga;
        ga.addFittedText (g.getCurrentFont(), text, area.getX(), area.getY(), area.getWidth(), area.getHeight(), j, juce::jmax (1, lines), minScale);
        juce::Path p; ga.createPath (p);
        if (p.isEmpty() || p.getBounds().isEmpty()) return;
        const auto bb = p.getBounds();
        g.saveState();
        g.setGradientFill (fontGradientFill (bb.getX(), bb.getRight(), alpha));
        g.fillPath (p);
        g.restoreState();
    }
}

// Panel titles / names drawn in paint(): gradient when Font = Gradient, otherwise the colour already set on g
inline void drawHeadline (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area, juce::Justification j, bool fitted = false)
{
    if (col::fontGradient && col::isHeadline (g.getCurrentFont()))
    {
        col::drawWithHalo (g, 1.0f, [&] (int dx, int dy) { if (fitted) g.drawFittedText (text, area.translated (dx, dy), j, 1); else g.drawText (text, area.translated (dx, dy), j, true); });
        col::fillGradientText (g, text, area.toFloat(), j, 1, fitted ? 0.0f : 1.0f);
        return;
    }
    if (fitted) g.drawFittedText (text, area, j, 1);
    else g.drawText (text, area, j, true);
}

// FL typing keyboard: Z S X D C V G B H N J M , = lower octave, Q 2 W 3 E R 5 T 6 Y 7 U I = upper
inline bool& typingKeyboard() { static bool on = false; return on; }
inline int typingKeyOffset (int keyCode)
{
    static const juce::String lo ("zsxdcvgbhnjm,"), hi ("q2w3er5t6y7ui");
    const auto c = juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) keyCode);
    int i = lo.indexOfChar (c);
    if (i >= 0) return i;
    i = hi.indexOfChar (c);
    return i >= 0 ? 12 + i : -1;
}
inline bool isTypingNoteKey (const juce::KeyPress& k)
{
    return typingKeyboard() && ! k.getModifiers().isCommandDown() && ! k.getModifiers().isAltDown()
           && ! k.getModifiers().isCtrlDown() && typingKeyOffset (k.getKeyCode()) >= 0;
}

// 0 = Tattoo (Pirata One), 1 = Flash (Rye), 2 = Clean; remembered between launches
int& fontStyle();
void setFontStyle (int);
void showWhatsNewIfUpdated();

inline constexpr int kNumSnaps = 11;
inline const double kSnapValues[kNumSnaps] { 0.0, 1.0 / 24, 1.0 / 16, 1.0 / 12, 0.125, 0.25, 1.0 / 6, 1.0 / 3, 0.5, 1.0, 4.0 };
inline const char* const kSnapNames[kNumSnaps] { "(none)", "1/6 step", "1/4 step", "1/3 step", "1/2 step", "Step", "1/6 beat", "1/3 beat", "1/2 beat", "Beat", "Bar" };

bool& captureFriendlyDrawing();   // Options > OBS / Discord capture-friendly drawing (Windows)

// OBS / Discord capture: JUCE 8 Direct2D windows can capture black on Windows, so use the software renderer
inline void makeCaptureFriendly (juce::Component& c)
{
   #if JUCE_WINDOWS
    if (! captureFriendlyDrawing()) return;
    if (auto* peer = c.getPeer())
    {
        const int i = peer->getAvailableRenderingEngines().indexOf ("Software Renderer");
        if (i >= 0) peer->setCurrentRenderingEngine (i);
    }
   #else
    juce::ignoreUnused (c);
   #endif
}

// Pop-out Mixer / plug-in windows float over the app. On macOS a real "always on top" (floating level) window is hidden
// from OBS / screen-capture window lists, so there they stay normal windows and are raised above the app after each click.
#if JUCE_MAC
inline constexpr bool floatWithTopmost = false;
#else
inline constexpr bool floatWithTopmost = true;
#endif
// horizontal scroll bar for the Playlist / Piano Roll (drag it, or use the wheel)
struct HScroll : juce::ScrollBar, juce::ScrollBar::Listener
{
    std::function<void (double)> onMove;
    explicit HScroll (bool vertical = false) : juce::ScrollBar (vertical) { addListener (this); setAutoHide (false); }
    void scrollBarMoved (juce::ScrollBar*, double start) override { if (onMove) onMove (start); }
    void sync (double total, double start, double visible)
    {
        setRangeLimits (0.0, juce::jmax (total, start + visible), juce::dontSendNotification);
        setCurrentRange (start, visible, juce::dontSendNotification);
    }
};
inline void markFloating (juce::Component& c) { c.getProperties().set ("hhsFloat", true); }
inline bool isFloating (juce::Component* c) { return c != nullptr && (bool) c->getProperties().getWithDefault ("hhsFloat", false); }
inline void raiseFloatingWindows()
{
    auto& d = juce::Desktop::getInstance();
    for (int i = 0; i < d.getNumComponents(); ++i)
        if (auto* c = d.getComponent (i); isFloating (c) && c->isVisible() && ! c->isCurrentlyBlockedByAnotherModalComponent())
            c->toFront (false);
}
inline void updateFloating (juce::Component& c)
{
    if (! floatWithTopmost) return;
    const bool front = juce::Process::isForegroundProcess();
    if (front != c.isAlwaysOnTop()) c.setAlwaysOnTop (front);
}

// Zoomed out, ruler bar numbers thin to every 2 / 4 / 8 (then 16...) bars so they never crowd; the grid itself is unchanged
inline int rulerLabelStep (float barPx)
{
    int step = 1;
    while (step < 64 && step * barPx < 36.0f) step *= 2;
    return step;
}

// FL-style tool cursors (white icon, dark outline so it reads on any background)
enum class CursorIcon { Pencil, Brush, Bin, Mute, Scissors, Zoom, Select, Rack, Hand };
inline juce::Image toolCursorImage (CursorIcon k, int& hx, int& hy)
{
    constexpr int S = 32;
    juce::Image img (juce::Image::ARGB, S, S, true);
    juce::Graphics g (img);
    juce::Path p;
    hx = 2; hy = 2;
    using juce::AffineTransform;
    switch (k)
    {
        case CursorIcon::Pencil:
        {
            juce::Path body; body.addRectangle (-2.5f, 0.0f, 5.0f, 18.0f); body.startNewSubPath (-2.5f, 18.0f);
            body.lineTo (0.0f, 24.0f); body.lineTo (2.5f, 18.0f); body.closeSubPath();
            p.addPath (body, AffineTransform::rotation (juce::MathConstants<float>::pi * 0.75f).translated (19.0f, 19.0f));
            hx = 2; hy = 2; break;
        }
        case CursorIcon::Brush:
        {
            juce::Path b; b.addRoundedRectangle (-2.0f, 0.0f, 4.0f, 14.0f, 1.5f); b.addEllipse (-4.0f, 13.0f, 8.0f, 9.0f);
            p.addPath (b, AffineTransform::rotation (juce::MathConstants<float>::pi * 0.75f).translated (24.0f, 24.0f));
            hx = 4; hy = 4; break;
        }
        case CursorIcon::Bin:
            p.addRectangle (7.0f, 4.0f, 14.0f, 3.0f); p.addRectangle (11.0f, 1.5f, 6.0f, 3.0f);
            p.startNewSubPath (8.0f, 8.0f); p.lineTo (20.0f, 8.0f); p.lineTo (18.5f, 24.0f); p.lineTo (9.5f, 24.0f); p.closeSubPath();
            hx = 14; hy = 14; break;
        case CursorIcon::Mute:
            p.startNewSubPath (4.0f, 11.0f); p.lineTo (9.0f, 11.0f); p.lineTo (15.0f, 5.0f); p.lineTo (15.0f, 23.0f);
            p.lineTo (9.0f, 17.0f); p.lineTo (4.0f, 17.0f); p.closeSubPath();
            p.addLineSegment ({ 18.0f, 9.0f, 26.0f, 19.0f }, 2.5f); p.addLineSegment ({ 26.0f, 9.0f, 18.0f, 19.0f }, 2.5f);
            hx = 9; hy = 14; break;
        case CursorIcon::Scissors:
            p.addEllipse (3.0f, 18.0f, 8.0f, 8.0f); p.addEllipse (17.0f, 18.0f, 8.0f, 8.0f);
            p.addLineSegment ({ 9.5f, 19.0f, 19.0f, 2.0f }, 2.5f); p.addLineSegment ({ 18.5f, 19.0f, 9.0f, 2.0f }, 2.5f);
            hx = 14; hy = 2; break;
        case CursorIcon::Select:   // arrow
            p.startNewSubPath (6.0f, 3.0f); p.lineTo (6.0f, 24.0f); p.lineTo (11.0f, 19.0f); p.lineTo (15.0f, 27.0f); p.lineTo (18.5f, 25.5f);
            p.lineTo (14.5f, 17.5f); p.lineTo (21.0f, 17.5f); p.closeSubPath();
            hx = 6; hy = 3; break;
        case CursorIcon::Rack:     // 3 lines going down (channel rack rows)
            for (int i = 0; i < 3; ++i) p.addRoundedRectangle (5.0f, 6.0f + 7.0f * (float) i, 18.0f, 3.5f, 1.5f);
            hx = 14; hy = 14; break;
        case CursorIcon::Hand:     // grab: palm + four fingers
            p.addRoundedRectangle (8.0f, 14.0f, 15.0f, 13.0f, 4.0f);
            for (int i = 0; i < 4; ++i) p.addRoundedRectangle (8.0f + 4.0f * (float) i, 5.0f + (i == 0 || i == 3 ? 3.0f : 0.0f), 3.2f, 12.0f, 1.5f);
            hx = 15; hy = 15; break;
        case CursorIcon::Zoom:
            p.addEllipse (3.0f, 3.0f, 15.0f, 15.0f); p.addLineSegment ({ 16.0f, 16.0f, 26.0f, 26.0f }, 4.0f);
            hx = 10; hy = 10; break;
    }
    g.setColour (juce::Colours::black.withAlpha (0.9f)); g.strokePath (p, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (juce::Colours::white); g.fillPath (p); g.strokePath (p, juce::PathStrokeType (1.0f));
    if (k == CursorIcon::Scissors || k == CursorIcon::Zoom)
    {
        g.setColour (juce::Colours::black); // keep ring holes see-through-ish
        if (k == CursorIcon::Scissors) { g.fillEllipse (5.5f, 20.5f, 3.0f, 3.0f); g.fillEllipse (19.5f, 20.5f, 3.0f, 3.0f); }
        else g.fillEllipse (6.0f, 6.0f, 9.0f, 9.0f);
    }
    return img;
}
inline juce::MouseCursor toolCursor (CursorIcon k)
{
    static std::map<int, juce::MouseCursor> cache;
    if (auto it = cache.find ((int) k); it != cache.end()) return it->second;
    int hx, hy;
    const auto img = toolCursorImage (k, hx, hy);
    return cache.emplace ((int) k, juce::MouseCursor (img, hx, hy)).first->second;
}

// a toolbar button that shows an icon instead of text (tooltip says what it does)
struct IconTextButton : public juce::TextButton
{
    int icon = 0;
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto& lf = getLookAndFeel();
        const auto bg = findColour (getToggleState() ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId);
        lf.drawButtonBackground (g, *this, bg, over, down);
        int hx, hy;
        const auto img = toolCursorImage ((CursorIcon) icon, hx, hy);
        const float sz = (float) juce::jmin (getWidth(), getHeight()) - 6.0f;
        g.setOpacity (isEnabled() ? 1.0f : 0.4f);
        g.drawImage (img, juce::Rectangle<float> (sz, sz).withCentre (getLocalBounds().toFloat().getCentre()), juce::RectanglePlacement::centred);
    }
};

class HHLookAndFeel : public juce::LookAndFeel_V4
{
public:
    HHLookAndFeel();
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;
    void refreshColours();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;   // Font = Gradient
    void drawLabel (juce::Graphics&, juce::Label&) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawFileBrowserRow (juce::Graphics&, int w, int h, const juce::File&, const juce::String& filename, juce::Image* icon,
                             const juce::String& size, const juce::String& time, bool isDir, bool selected, int index,
                             juce::DirectoryContentsDisplayComponent&) override;   // samples show without .wav / .flac / .mp3
};

// Rotary knob; hovering shows its name, what it does and its value (tooltip + hint bar + popup).
class Knob : public juce::Slider
{
public:
    Knob (const juce::String& name, const juce::String& help, double mn, double mx, double def, const juce::String& suffix = {});
    void mouseDoubleClick (const juce::MouseEvent&) override;   // type an exact value (Alt-click resets)
};

void setupButton (juce::Button&, const juce::String& text, const juce::String& tooltip, bool toggle = false);
void askText (const juce::String& title, const juce::String& initial, std::function<void (juce::String)> done);

// Shared app state: every edit goes through edit() so it is locked against the audio thread,
// pushed to the 999-level undo history and broadcast to all views.
struct Studio : public juce::ChangeBroadcaster
{
    explicit Studio (Engine& e) : engine (e) {}
    Engine& engine;
    UndoHistory undo;
    juce::PropertiesFile* settings = nullptr;
    int selectedChannel = -1, selectedTrack = 1;
    int editsSinceBackup = 0;
    juce::SortedSet<int> selectedLanes;   // highlighted playlist tracks (click / Shift / Ctrl-Cmd)
    int laneAnchor = -1;
    bool dirty = false;
    std::function<void (int view)> showView;   // 0 beat, 1 record, 2 mix

    Project& p() { return engine.project(); }
    template <typename F> void live (F&& f) { const juce::ScopedLock sl (engine.getLock()); f (p()); }
    template <typename F> void edit (const juce::String& what, F&& f) { live (std::forward<F> (f)); commit (what); }
    void commit (const juce::String& what);
    void undoStep (int delta);
    void jumpHistory (int index);
    NoteList& notes (int channelId);
    Channel* channel (int id);
};

std::unique_ptr<juce::Component> createMainComponent();
void requestQuit (juce::Component* main);   // asks Save / Don't Save / Cancel when the project has changes
// a project, template pack, preset or FX chain opened from Finder / Explorer, an email or a drop on the window
void openFileInMain (juce::Component* main, const juce::File&);
inline bool isHumHouseShareFile (const juce::String& path)
{
    const juce::File f (path);
    return f.hasFileExtension ("hhpack") || f.hasFileExtension ("hhpreset") || f.hasFileExtension ("hhchain") || f.hasFileExtension ("hhs");
}
inline bool allHumHouseShareFiles (const juce::StringArray& files)
{
    for (auto& f : files) if (! isHumHouseShareFile (f)) return false;
    return ! files.isEmpty();
}
// shared files received by email, AirDrop, USB...: copied into your presets; returns the installed file, or {} if it isn't one
juce::File installSharedPreset (const juce::File&);
juce::File installSharedChain (const juce::File&);
std::unique_ptr<juce::Component> createChannelRack (Studio&);
std::unique_ptr<juce::Component> createPianoRoll (Studio&);
std::unique_ptr<juce::Component> createPlaylist (Studio&);
std::unique_ptr<juce::Component> createMixer (Studio&);
std::unique_ptr<juce::Component> createBrowser (Studio&);

void addSampleChannels (Studio&, const juce::Array<juce::File>&, const juce::String& insertName);
void importKit (Studio&, const juce::File& folder);
void addPluginChannel (Studio&, const juce::PluginDescription&);

void showPluginHouseMenu (Studio&, int track, int replaceSlotUid = -1);
void addEffect (Studio&, int track, const juce::String& type, const juce::String& pluginId, int replaceSlotUid = -1);
void showPluginScanner (Studio&);

void openEffectEditor (Studio&, int track, int slotUid);
// automation (FL): "Automate" menu; item ids base + index into out. Picking one makes an automation track below belowLane
juce::PopupMenu automateMenu (Studio&, std::vector<AutoTarget>& out, int base, int track, int fxUid, int channelId);
void createAutomationClip (Studio&, const AutoTarget&, int belowLane);
juce::PopupMenu trackPresetMenu (Studio&, int mixerTrack);   // right-click > Presets on tracks / inserts
// right-click > Color: ids base..base+11 = palette, base+12 = random
inline juce::Colour pickedColour (int i)
{
    static const juce::uint32 c[] { 0xffff5d5d, 0xffff8a3d, 0xffffc93d, 0xffb8e04a, 0xff4ad991, 0xff3dd6d6,
                                    0xff3d9bff, 0xff6f6bff, 0xffa86bff, 0xffff6bd6, 0xffc8c8c8, 0xff9a7a5a };
    if (i < 0 || i >= 12) return juce::Colour::fromHSV (juce::Random::getSystemRandom().nextFloat(), 0.6f, 0.95f, 1.0f);
    return juce::Colour (c[i]);
}
inline juce::PopupMenu colourMenu (int base)
{
    static const char* names[] { "Red", "Orange", "Yellow", "Lime", "Green", "Cyan", "Blue", "Indigo", "Purple", "Pink", "Silver", "Brown" };
    juce::PopupMenu m;
    for (int i = 0; i < 12; ++i) m.addColouredItem (base + i, names[i], pickedColour (i));
    m.addSeparator(); m.addItem (base + 12, "Random");
    return m;
}
void openChannelEditor (Studio&, int channelId);
void openClipEditor (Studio&, int clipUid);                         // audio clip MAIN / FUNC / SCOPE
extern std::function<void (int, const juce::String&)> clipAction;
extern std::function<void()> rackPopOut;   // set by the main window: pop the Channel Rack out / dock it   // set by the Playlist: reverse / normalize / humlab / humtime / humtone
// Patch Method (FL Patcher style): the channel gets its own insert and a window to stack effects / chains on it
void openPatchMethod (Studio&, int channelId);
void closeAllEditorWindows();
extern std::function<bool (const juce::KeyPress&)> hostKeyHandler;   // app shortcuts (Cmd+S, Space, undo...) from pop-out / plug-in windows
juce::Component* createScopeView (std::function<ScopeTap*()> source, const juce::String& title);
void openScopeWindow (Studio&);
void openTrackScopeWindow (Studio&, int track);   // master scope, pops out + resizable
void closeEditorsForPlugin (juce::AudioProcessor*);

void applyTheme (juce::Colour accent, float tint, bool black = false);   // tint 0..1 = how much the accent colors the background
void resetTheme();
void applyGradient (const juce::String& colours);   // "lo mid hi" ARGB hex
void applyFontGradient (const juce::String& colours);   // "lo mid hi" ARGB hex, empty = solid font
void showThemeDialog (Studio&);
void drawLogo (juce::Graphics&, juce::Rectangle<float> area, bool withText);

enum class Tool { Arp, Strum, Chop, Flam, Quantize, Randomize, Limit, Articulate, ScaleLevels, Flip, Claw, Lfo };
void showToolDialog (Studio&, Tool, int channelId);
}
