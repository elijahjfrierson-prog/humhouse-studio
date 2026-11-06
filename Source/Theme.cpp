#include "UI.h"

namespace hhs
{
namespace
{
struct Palette { juce::Colour bg, panel, panel2, grid, gridBeat, gridBar, accent, scale, root; };
const Palette defaults { juce::Colour (0xff0e1013), juce::Colour (0xff16191e), juce::Colour (0xff1d2127), juce::Colour (0xff22262d),
                         juce::Colour (0xff2c3139), juce::Colour (0xff3c434e), juce::Colour (0xff9b5cff), juce::Colour (0xff1a2721),
                         juce::Colour (0xff21392d) };

void refreshAll()
{
    if (auto* l = dynamic_cast<HHLookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel())) l->refreshColours();
    auto& d = juce::Desktop::getInstance();
    for (int i = 0; i < d.getNumComponents(); ++i)
        if (auto* c = d.getComponent (i))
        {
            if (auto* w = dynamic_cast<juce::DocumentWindow*> (c)) w->setBackgroundColour (col::bg);
            c->sendLookAndFeelChange();
            c->repaint();
        }
}

class ColourWheel : public juce::Component
{
public:
    std::function<void()> onChange;
    float hue = 0.07f, sat = 0.75f;

    void paint (juce::Graphics& g) override
    {
        auto r = wheel();
        if (img.getWidth() != (int) r.getWidth()) build ((int) r.getWidth());
        g.drawImageAt (img, (int) r.getX(), (int) r.getY());
        const float R = r.getWidth() * 0.5f, a = hue * juce::MathConstants<float>::twoPi;
        const auto p = r.getCentre() + juce::Point<float> (std::cos (a), std::sin (a)) * (sat * R);
        g.setColour (juce::Colours::black); g.drawEllipse (p.x - 8, p.y - 8, 16, 16, 3.0f);
        g.setColour (juce::Colours::white); g.drawEllipse (p.x - 8, p.y - 8, 16, 16, 1.5f);
    }
    void mouseDown (const juce::MouseEvent& e) override { pick (e.position); }
    void mouseDrag (const juce::MouseEvent& e) override { pick (e.position); }

private:
    juce::Image img;
    juce::Rectangle<float> wheel() const
    {
        const float s = (float) std::min (getWidth(), getHeight());
        return getLocalBounds().toFloat().withSizeKeepingCentre (s, s);
    }
    void pick (juce::Point<float> pos)
    {
        auto r = wheel();
        const auto d = pos - r.getCentre();
        sat = juce::jlimit (0.0f, 1.0f, d.getDistanceFromOrigin() / (r.getWidth() * 0.5f));
        hue = std::fmod (std::atan2 (d.y, d.x) / juce::MathConstants<float>::twoPi + 1.0f, 1.0f);
        repaint();
        if (onChange) onChange();
    }
    void build (int size)
    {
        img = juce::Image (juce::Image::ARGB, size, size, true);
        const float R = size * 0.5f;
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                const float dx = x + 0.5f - R, dy = y + 0.5f - R, d = std::sqrt (dx * dx + dy * dy);
                if (d > R) continue;
                const float h = std::fmod (std::atan2 (dy, dx) / juce::MathConstants<float>::twoPi + 1.0f, 1.0f);
                bd.setPixelColour (x, y, juce::Colour::fromHSV (h, d / R, 1.0f, juce::jlimit (0.0f, 1.0f, R - d)));
            }
    }
};

class ThemePanel : public juce::Component
{
public:
    explicit ThemePanel (Studio& st) : s (st)
    {
        wheel.hue = col::accent.getHue(); wheel.sat = col::accent.getSaturation();
        wheel.onChange = [this] { apply(); };
        addAndMakeVisible (wheel);
        for (auto* sl : { &bright, &tint })
        {
            sl->setSliderStyle (juce::Slider::LinearHorizontal);
            sl->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            sl->onValueChange = [this] { apply(); };
            addAndMakeVisible (sl);
        }
        bright.setRange (0.35, 1.0); bright.setValue (col::accent.getBrightness(), juce::dontSendNotification);
        bright.setTooltip ("Brightness of the theme color");
        tint.setRange (0.0, 1.0); tint.setValue (s.settings ? s.settings->getDoubleValue ("themeTint", 0.5) : 0.5, juce::dontSendNotification);
        tint.setTooltip ("How much the color tints the whole DAW background (0 = neutral black)");
        for (auto* l : { &brightLabel, &tintLabel }) { l->setFont (juce::FontOptions (12.0f)); addAndMakeVisible (l); }
        const juce::uint32 presets[] { 0xff9b5cff, 0xffff8a3d, 0xffff3b57, 0xffb15cff, 0xff4d7cff, 0xff38d1ff, 0xff3ddc84, 0xffff5db1, 0xffffc93d, 0xffe6e8eb, 0xff8c929b, 0xff000000 };
        for (auto p : presets)
        {
            auto* b = swatches.add (new juce::TextButton());
            b->setColour (juce::TextButton::buttonColourId, juce::Colour (p));
            b->setTooltip (p == 0xffe6e8eb ? "White (light theme: white backgrounds, dark text)" : p == 0xff8c929b ? "Grey" : p == 0xff000000 ? "Black (true-black background, silver highlights)" : "Preset color");
            b->onClick = [this, p] {
                black = p == 0xff000000; light = p == 0xffe6e8eb;
                const juce::Colour c (black ? 0xffc9ced6 : light ? 0xff30343b : p);
                wheel.hue = c.getHue(); wheel.sat = c.getSaturation(); wheel.repaint();
                bright.setValue (c.getBrightness(), juce::dontSendNotification);
                apply();
            };
            addAndMakeVisible (b);
        }
        // meters + scopes gradient presets (quiet, middle, loud)
        const juce::uint32 grads[][3] { { 0xff3ddc84, 0xffffd23d, 0xffff3b57 }, { 0xff38d1ff, 0xff4d7cff, 0xffb15cff },
                                        { 0xffffc93d, 0xffff8a3d, 0xffff3b57 }, { 0xff3ddc84, 0xff38d1ff, 0xffffffff },
                                        { 0xffff5db1, 0xffb15cff, 0xff38d1ff }, { 0xff7f8896, 0xffc9ced6, 0xffffffff } };
        for (auto& gc : grads)
        {
            const juce::String v = juce::Colour (gc[0]).toString() + " " + juce::Colour (gc[1]).toString() + " " + juce::Colour (gc[2]).toString();
            auto* b = gradButtons.add (new GradSwatch (juce::Colour (gc[0]), juce::Colour (gc[1]), juce::Colour (gc[2])));
            b->onClick = [this, v] { applyGradient (v); if (s.settings) { s.settings->setValue ("gradColours", v); s.settings->saveIfNeeded(); } };
            addAndMakeVisible (b);
        }
        gradLabel.setFont (juce::FontOptions (12.0f)); addAndMakeVisible (gradLabel);
        fontLabel.setFont (juce::FontOptions (12.0f)); addAndMakeVisible (fontLabel);
        const juce::uint32 fonts[] { 0x00000000, 0xffffffff, 0xffc9ced6, 0xff8c929b, 0xff15171a, 0xffff8a3d, 0xff38d1ff, 0xff3ddc84, 0xffff5db1, 0xffffc93d, 0xffb15cff };
        for (auto fc : fonts)
        {
            auto* b = fontSw.add (new juce::TextButton (fc == 0 ? "Auto" : ""));
            b->setColour (juce::TextButton::buttonColourId, fc == 0 ? col::panel2 : juce::Colour (fc));
            b->setTooltip (fc == 0 ? "Automatic text color for the theme" : "Text color across the whole app");
            b->onClick = [this, fc] { col::fontColour = juce::Colour (fc); if (s.settings) s.settings->setValue ("fontColour", juce::Colour (fc).toString()); setFontGradient ({}); apply(); };
            addAndMakeVisible (b);
        }
        // font: Solid (the color row) or Gradient (headline text: tabs, titles, track / channel / insert names)
        setupButton (fontSolid, "Solid", "One text color (the row below)");
        setupButton (fontGrad, "Gradient", "Headline text (tabs, panel titles, track, channel and insert names) gets a color gradient");
        fontSolid.onClick = [this] { setFontGradient ({}); };
        fontGrad.onClick = [this] { setFontGradient (col::fontLo.toString() + " " + col::fontMid.toString() + " " + col::fontHi.toString()); };
        for (auto* b : { &fontSolid, &fontGrad }) { b->setColour (juce::TextButton::buttonOnColourId, col::accent); addAndMakeVisible (b); }
        for (auto& gc : grads)
        {
            const juce::String v = juce::Colour (gc[0]).toString() + " " + juce::Colour (gc[1]).toString() + " " + juce::Colour (gc[2]).toString();
            auto* b = fontGradButtons.add (new GradSwatch (juce::Colour (gc[0]), juce::Colour (gc[1]), juce::Colour (gc[2]), true));
            b->onClick = [this, v] { setFontGradient (v); };
            addAndMakeVisible (b);
        }
        syncFontMode();
        setupButton (reset, "Reset", "Back to the default HumHouse purple");
        reset.onClick = [this] {
            resetTheme();
            applyGradient ({}); applyFontGradient ({}); syncFontMode(); black = false;
            if (s.settings) { s.settings->removeValue ("fontGrad"); s.settings->removeValue ("themeColour"); s.settings->removeValue ("themeTint"); s.settings->removeValue ("gradColours"); s.settings->removeValue ("themeBlack"); s.settings->saveIfNeeded(); }
            wheel.hue = col::accent.getHue(); wheel.sat = col::accent.getSaturation(); wheel.repaint();
            bright.setValue (col::accent.getBrightness(), juce::dontSendNotification);
        };
        addAndMakeVisible (reset);
        // track colors: pick a Playlist track, click a swatch (its audio clips follow)
        trackLabel.setFont (juce::FontOptions (12.0f)); addAndMakeVisible (trackLabel);
        auto& lanes = s.p().lanes;
        for (size_t i = 0; i < lanes.size(); ++i) trackBox.addItem (juce::String ((int) i + 1) + "  " + lanes[i].name, (int) i + 1);
        trackBox.setSelectedId (juce::jlimit (1, juce::jmax (1, (int) lanes.size()), s.laneAnchor + 1), juce::dontSendNotification);
        addAndMakeVisible (trackBox);
        for (int i = 0; i <= 12; ++i)
        {
            auto* b = trackSwatches.add (new juce::TextButton (i == 12 ? "x" : ""));
            if (i < 12) b->setColour (juce::TextButton::buttonColourId, pickedColour (i));
            b->setTooltip (i == 12 ? "Clear the track color" : "Color this track and its clips");
            b->onClick = [this, i] {
                const int li = trackBox.getSelectedId() - 1;
                const auto c = i == 12 ? juce::Colour() : pickedColour (i);
                s.edit ("Track color", [li, c] (Project& p) { if (li >= 0 && li < (int) p.lanes.size()) p.lanes[(size_t) li].colour = c; });
            };
            addAndMakeVisible (b);
        }
        setSize (340, 700);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::panel);
        drawLogo (g, juce::Rectangle<float> (14, 10, 200, 40), true);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        r.removeFromTop (46);
        wheel.setBounds (r.removeFromTop (220));
        r.removeFromTop (10);
        brightLabel.setBounds (r.removeFromTop (18)); bright.setBounds (r.removeFromTop (24));
        tintLabel.setBounds (r.removeFromTop (18)); tint.setBounds (r.removeFromTop (24));
        r.removeFromTop (10);
        auto row = r.removeFromTop (30);
        const int w = row.getWidth() / swatches.size();
        for (auto* b : swatches) b->setBounds (row.removeFromLeft (w).reduced (3));
        r.removeFromTop (8);
        gradLabel.setBounds (r.removeFromTop (18));
        auto grow = r.removeFromTop (28);
        const int gw = grow.getWidth() / gradButtons.size();
        for (auto* b : gradButtons) b->setBounds (grow.removeFromLeft (gw).reduced (3));
        auto flab = r.removeFromTop (24);
        fontGrad.setBounds (flab.removeFromRight (76).reduced (2, 1)); fontSolid.setBounds (flab.removeFromRight (60).reduced (2, 1));
        fontLabel.setBounds (flab);
        auto frow = r.removeFromTop (30);
        const int fw = frow.getWidth() / fontSw.size();
        for (auto* b : fontSw) b->setBounds (frow.removeFromLeft (fw).reduced (3));
        auto fgrow = r.removeFromTop (28);
        const int fgw = fgrow.getWidth() / fontGradButtons.size();
        for (auto* b : fontGradButtons) b->setBounds (fgrow.removeFromLeft (fgw).reduced (3));
        r.removeFromTop (10);
        trackLabel.setBounds (r.removeFromTop (18));
        trackBox.setBounds (r.removeFromTop (24));
        auto trow = r.removeFromTop (28);
        const int tw = trow.getWidth() / trackSwatches.size();
        for (auto* b : trackSwatches) b->setBounds (trow.removeFromLeft (tw).reduced (2));
        r.removeFromTop (10);
        reset.setBounds (r.removeFromTop (28).removeFromRight (90));
    }

private:
    Studio& s;
    ColourWheel wheel;
    juce::Slider bright, tint;
    juce::Label brightLabel { {}, "Brightness" }, tintLabel { {}, "Background tint" };
    juce::OwnedArray<juce::TextButton> swatches;
    bool black = s.settings != nullptr && s.settings->getBoolValue ("themeBlack", false);
    bool light = s.settings != nullptr && s.settings->getBoolValue ("themeLight", false);
    juce::OwnedArray<juce::TextButton> fontSw;
    juce::Label fontLabel { {}, "Font color" };
    juce::TextButton fontSolid, fontGrad;
    struct GradSwatch : public juce::Button
    {
        GradSwatch (juce::Colour a, juce::Colour b, juce::Colour c, bool forFont = false) : juce::Button ("gradient"), lo (a), mid (b), hi (c), font (forFont)
        {
            setTooltip (font ? "Font gradient (headline text)" : "Meter + scope gradient");
        }
        juce::Colour lo, mid, hi;
        bool font;
        void paintButton (juce::Graphics& g, bool over, bool) override
        {
            auto r = getLocalBounds().toFloat().reduced (1);
            juce::ColourGradient gr (lo, r.getX(), 0, hi, r.getRight(), 0, false); gr.addColour (0.5, mid);
            g.setGradientFill (gr); g.fillRoundedRectangle (r, 3);
            const bool on = font ? col::fontGradient && col::fontLo == lo && col::fontMid == mid && col::fontHi == hi
                                 : col::gradLo == lo && col::gradMid == mid && col::gradHi == hi;
            g.setColour (on ? col::text : over ? col::dim : col::grid); g.drawRoundedRectangle (r, 3, on ? 2.0f : 1.0f);
        }
    };
    juce::OwnedArray<GradSwatch> gradButtons, fontGradButtons;
    juce::Label gradLabel { {}, "Meters + scopes gradient" };
    juce::TextButton reset;
    juce::Label trackLabel { {}, "Track color (Playlist)" };
    juce::ComboBox trackBox;
    juce::OwnedArray<juce::TextButton> trackSwatches;

    void syncFontMode()
    {
        fontSolid.setToggleState (! col::fontGradient, juce::dontSendNotification);
        fontGrad.setToggleState (col::fontGradient, juce::dontSendNotification);
    }
    void setFontGradient (const juce::String& v)   // "" = solid
    {
        applyFontGradient (v); syncFontMode();
        if (s.settings) { s.settings->setValue ("fontGrad", v); s.settings->saveIfNeeded(); }
    }
    void apply()
    {
        const auto c = juce::Colour::fromHSV (wheel.hue, wheel.sat, (float) bright.getValue(), 1.0f);
        col::lightMode = light;
        applyTheme (c, (float) tint.getValue(), black);
        if (s.settings)
        {
            s.settings->setValue ("themeLight", light);
            s.settings->setValue ("themeBlack", black);
            s.settings->setValue ("themeColour", c.toString());
            s.settings->setValue ("themeTint", tint.getValue());
            s.settings->removeValue ("layout");   // the color wheel takes over from a Layout
        }
    }
};
}

static void applyTextColours()
{
    col::text = col::fontColour.getAlpha() > 0 ? col::fontColour : (col::lightMode ? juce::Colour (0xff15171a) : juce::Colour (0xffdfe4ec));
    col::dim = col::lightMode ? juce::Colour (0xff5d6573) : juce::Colour (0xff7f8896);
}

void applyTheme (juce::Colour accent, float tint, bool black)
{
    applyTextColours();
    if (col::lightMode)   // the opposite of Black: white backgrounds, dark text, the chosen color tints it
    {
        const float h = accent.getHue(), sat = juce::jlimit (0.0f, 0.12f, accent.getSaturation() * 0.15f * juce::jlimit (0.0f, 1.0f, tint));
        auto shade = [h, sat] (float b) { return juce::Colour::fromHSV (h, sat, b, 1.0f); };
        col::accent = accent;
        col::bg = shade (0.97f); col::panel = shade (0.93f); col::panel2 = shade (0.89f);
        col::grid = shade (0.86f); col::gridBeat = shade (0.80f); col::gridBar = shade (0.68f);
        col::scale = juce::Colour::fromHSV (h, 0.12f, 0.90f, 1.0f); col::root = juce::Colour::fromHSV (h, 0.2f, 0.84f, 1.0f);
        refreshAll();
        return;
    }
    if (black)
    {
        col::accent = accent;
        col::bg = juce::Colour (0xff050506); col::panel = juce::Colour (0xff0c0d0f); col::panel2 = juce::Colour (0xff131417);
        col::grid = juce::Colour (0xff18191c); col::gridBeat = juce::Colour (0xff212327); col::gridBar = juce::Colour (0xff2e3136);
        col::scale = juce::Colour (0xff15171a); col::root = juce::Colour (0xff1e2126);
        refreshAll();
        return;
    }
    const float h = accent.getHue(), sat = juce::jlimit (0.0f, 0.4f, accent.getSaturation() * 0.4f * juce::jlimit (0.0f, 1.0f, tint));
    auto shade = [h, sat] (float b) { return juce::Colour::fromHSV (h, sat, b, 1.0f); };
    col::accent = accent;
    col::bg = shade (0.075f); col::panel = shade (0.118f); col::panel2 = shade (0.153f);
    col::grid = shade (0.176f); col::gridBeat = shade (0.224f); col::gridBar = shade (0.306f);
    col::scale = juce::Colour::fromHSV (h, 0.3f, 0.16f, 1.0f);
    col::root = juce::Colour::fromHSV (h, 0.4f, 0.23f, 1.0f);
    refreshAll();
}

void applyGradient (const juce::String& colours)
{
    auto t = juce::StringArray::fromTokens (colours, " ", "");
    if (t.size() == 3) { col::gradLo = juce::Colour::fromString (t[0]); col::gradMid = juce::Colour::fromString (t[1]); col::gradHi = juce::Colour::fromString (t[2]); }
    else { col::gradLo = juce::Colour (0xff3ddc84); col::gradMid = juce::Colour (0xffffd23d); col::gradHi = juce::Colour (0xffff3b57); }
    refreshAll();
}

void applyFontGradient (const juce::String& colours)
{
    auto t = juce::StringArray::fromTokens (colours, " ", "");
    col::fontGradient = t.size() == 3;
    if (col::fontGradient) { col::fontLo = juce::Colour::fromString (t[0]); col::fontMid = juce::Colour::fromString (t[1]); col::fontHi = juce::Colour::fromString (t[2]); }
    refreshAll();
}

void resetTheme()
{
    col::lightMode = false; col::fontColour = juce::Colour (0x00000000); applyTextColours();
    col::bg = defaults.bg; col::panel = defaults.panel; col::panel2 = defaults.panel2; col::grid = defaults.grid;
    col::gridBeat = defaults.gridBeat; col::gridBar = defaults.gridBar; col::accent = defaults.accent;
    col::scale = defaults.scale; col::root = defaults.root;
    refreshAll();
}

namespace
{
struct LayoutLook { const char* name; juce::uint32 bg, panel, panel2, gridBar, text, accent; bool light; int laneH; const char* keys; };
const LayoutLook layoutLooks[] {
    { "HumHouse (default)",     0xff0e1013, 0xff16191e, 0xff1d2127, 0xff3c434e, 0xffdfe4ec, 0xff9b5cff, false, 60, "" },
    { "Logic Dark",             0xff1e1e1e, 0xff2a2a2a, 0xff333333, 0xff4a4a4a, 0xffe6e6e6, 0xff3fa9f5, false, 56, "Logic" },
    { "Logic Light",            0xffececec, 0xffdcdcdc, 0xffcfcfcf, 0xffa0a0a0, 0xff1c1c1c, 0xff2f80ed, true,  56, "Logic" },
    { "Pro Tools Classic",      0xff3c4048, 0xff4b505a, 0xff565c67, 0xff727985, 0xfff0f0f0, 0xff5fb3e6, false, 72, "Pro Tools" },
    { "Pro Tools Dark",         0xff202327, 0xff2b2f35, 0xff343941, 0xff4c535d, 0xffe8eaed, 0xff4fb3ff, false, 72, "Pro Tools" },
    { "Reaper Default",         0xff333333, 0xff3d3d3d, 0xff464646, 0xff5e5e5e, 0xffdcdcdc, 0xff39c0a8, false, 64, "Reaper" },
    { "Reaper Classic",         0xffc0c4c8, 0xffb0b4b8, 0xffa4a8ac, 0xff7a7e82, 0xff101010, 0xff2c6fbb, true,  64, "Reaper" },
    { "Studio One Blue",        0xff1b2129, 0xff232b35, 0xff2b3540, 0xff44515f, 0xffe3e9f0, 0xff3e8ed0, false, 60, "" },
    { "Cubase Slate",           0xff2b2f33, 0xff34393e, 0xff3d4349, 0xff5a6168, 0xffe0e3e6, 0xffe2a33c, false, 60, "" },
    { "Live Gray",              0xff3a3a3a, 0xff444444, 0xff4e4e4e, 0xff666666, 0xffe8e8e8, 0xfff0d43a, false, 52, "" },
    { "BandLab Night",          0xff121218, 0xff1b1b24, 0xff24242f, 0xff3a3a4a, 0xfff2f2f7, 0xfff2295b, false, 64, "" },
    { "Midnight Purple",        0xff0d0a14, 0xff161021, 0xff1f172d, 0xff3a2c55, 0xffece6f7, 0xffb06cff, false, 60, "" },
    { "Studio White",           0xfff6f6f6, 0xffebebeb, 0xffe0e0e0, 0xffb5b5b5, 0xff151515, 0xff9b5cff, true,  60, "" },
    { "Neon",                   0xff07070a, 0xff0f0f14, 0xff16161e, 0xff2a2a3a, 0xfff0f0ff, 0xff00f0c8, false, 60, "" },
    { "Vintage Tape",           0xff2a241d, 0xff352d24, 0xff40372c, 0xff645645, 0xfff1e6d2, 0xffe0b060, false, 68, "" },
    { "High Contrast",          0xff000000, 0xff0a0a0a, 0xff141414, 0xff6a6a6a, 0xffffffff, 0xffffd400, false, 70, "" },
    { "Ocean",                  0xff0b1a22, 0xff10242f, 0xff162e3b, 0xff2b4d60, 0xffe2f3fa, 0xff2ec4e6, false, 60, "" },
    { "Forest",                 0xff0f1a13, 0xff16241b, 0xff1d2e23, 0xff35503e, 0xffe4f2e8, 0xff4cd07a, false, 60, "" },
};
constexpr int numLooks = (int) (sizeof (layoutLooks) / sizeof (layoutLooks[0]));

std::map<juce::String, juce::String> parseLayout (const juce::String& d)
{
    std::map<juce::String, juce::String> v;
    for (auto& kv : juce::StringArray::fromTokens (d, ";", ""))
        if (kv.contains ("=")) v[kv.upToFirstOccurrenceOf ("=", false, false)] = kv.fromFirstOccurrenceOf ("=", false, false);
    return v;
}
juce::String joinLayout (const std::map<juce::String, juce::String>& v)
{
    juce::StringArray a; for (auto& [k, x] : v) a.add (k + "=" + x);
    return a.joinIntoString (";");
}
juce::String lookDesc (const LayoutLook& l)
{
    auto c = [] (juce::uint32 x) { return juce::Colour (x).toString(); };
    return joinLayout ({ { "bg", c (l.bg) }, { "panel", c (l.panel) }, { "panel2", c (l.panel2) }, { "gridBar", c (l.gridBar) },
                         { "text", c (l.text) }, { "accent", c (l.accent) }, { "light", l.light ? "1" : "0" }, { "laneH", juce::String (l.laneH) } });
}
juce::String currentLayout()
{
    return joinLayout ({ { "bg", col::bg.toString() }, { "panel", col::panel.toString() }, { "panel2", col::panel2.toString() }, { "gridBar", col::gridBar.toString() },
                         { "text", col::text.toString() }, { "accent", col::accent.toString() }, { "light", col::lightMode ? "1" : "0" }, { "laneH", juce::String (col::laneMinH) } });
}
void applyLayoutDesc (const juce::String& d)
{
    auto v = parseLayout (d);
    auto c = [&v] (const char* k, juce::Colour def) { auto it = v.find (k); const auto x = it != v.end() ? juce::Colour::fromString (it->second) : def; return x.getAlpha() == 0 ? def : x.withAlpha (1.0f); };
    col::lightMode = v["light"] == "1";
    applyTextColours();
    col::bg = c ("bg", col::bg); col::panel = c ("panel", col::panel); col::panel2 = c ("panel2", col::panel2);
    col::gridBar = c ("gridBar", col::gridBar); col::accent = c ("accent", col::accent); col::text = c ("text", col::text);
    col::grid = col::panel2.interpolatedWith (col::gridBar, 0.2f); col::gridBeat = col::panel2.interpolatedWith (col::gridBar, 0.5f);
    col::dim = col::text.interpolatedWith (col::bg, 0.45f);
    col::scale = col::bg.interpolatedWith (col::accent, 0.10f); col::root = col::bg.interpolatedWith (col::accent, 0.2f);
    const int lh = v["laneH"].getIntValue();
    col::laneMinH = juce::jlimit (36, 220, lh > 0 ? lh : 60);
    refreshAll();
}

struct ColourPick : juce::Component, juce::ChangeListener
{
    ColourPick (juce::Colour c, std::function<void (juce::Colour)> f) : fn (std::move (f))
    {
        sel.setCurrentColour (c, juce::dontSendNotification); sel.addChangeListener (this);
        addAndMakeVisible (sel); setSize (300, 280);
    }
    ~ColourPick() override { sel.removeChangeListener (this); }
    void resized() override { sel.setBounds (getLocalBounds()); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { if (fn) fn (sel.getCurrentColour()); }
    juce::ColourSelector sel { juce::ColourSelector::showColourAtTop | juce::ColourSelector::showSliders | juce::ColourSelector::showColourspace };
    std::function<void (juce::Colour)> fn;
};

struct LayoutPanel : juce::Component
{
    explicit LayoutPanel (Studio& st) : s (st)
    {
        addAndMakeVisible (title); title.setText ("LAYOUT: pick a look or make your own", juce::dontSendNotification); title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        addAndMakeVisible (pick); loadMine(); fillPick();
        pick.onChange = [this] { choose (pick.getSelectedId()); };
        addAndMakeVisible (withKeys); withKeys.setButtonText ("Use that DAW's key commands too (Logic, Pro Tools, Reaper)");
        withKeys.setToggleState (s.settings == nullptr || s.settings->getBoolValue ("layoutKeys", true), juce::dontSendNotification);
        withKeys.onClick = [this] { if (s.settings) s.settings->setValue ("layoutKeys", withKeys.getToggleState()); };
        addAndMakeVisible (customLbl); customLbl.setText ("CUSTOM: tap a color to change it", juce::dontSendNotification);
        static const char* names[6] { "Background", "Panels", "Buttons", "Grid lines", "Text", "Accent" };
        static const char* keys[6] { "bg", "panel", "panel2", "gridBar", "text", "accent" };
        for (int i = 0; i < 6; ++i)
        {
            auto* b = swatches.add (new juce::TextButton (names[i])); addAndMakeVisible (b);
            b->onClick = [this, i]
            {
                juce::Component::SafePointer<LayoutPanel> sp (this);
                const juce::String key (keys[i]);
                auto now = juce::Colour::fromString (parseLayout (currentLayout())[key]);
                juce::CallOutBox::launchAsynchronously (std::make_unique<ColourPick> (now, [sp, key] (juce::Colour c) { if (sp != nullptr) sp->setCustom (key, c.toString()); }),
                                                        swatches[i]->getScreenBounds(), nullptr);
            };
        }
        addAndMakeVisible (laneLbl); laneLbl.setText ("Track height", juce::dontSendNotification);
        addAndMakeVisible (laneH); laneH.setRange (36, 220, 1); laneH.setSliderStyle (juce::Slider::LinearHorizontal);
        laneH.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 22);
        laneH.onDragEnd = [this] { setCustom ("laneH", juce::String ((int) laneH.getValue())); };
        addAndMakeVisible (name); name.setTextToShowWhenEmpty ("Name your layout", col::dim);
        for (auto* b : { &saveBtn, &delBtn, &resetBtn }) addAndMakeVisible (b);
        saveBtn.onClick = [this] { saveMine(); };
        delBtn.onClick = [this] { deleteMine(); };
        resetBtn.onClick = [this] { pick.setSelectedId (1, juce::sendNotificationSync); };
        sync();
        setSize (520, 360);
    }
    void fillPick()
    {
        pick.clear (juce::dontSendNotification);
        for (int i = 0; i < numLooks; ++i) pick.addItem (layoutLooks[i].name, i + 1);
        if (! mine.empty()) { pick.addSeparator(); pick.addSectionHeading ("MY LAYOUTS"); }
        for (size_t i = 0; i < mine.size(); ++i) pick.addItem (mine[i].first, 100 + (int) i);
        const auto cur = s.settings ? s.settings->getValue ("layoutName") : juce::String();
        for (int i = 0; i < pick.getNumItems(); ++i) if (pick.getItemText (i) == cur) pick.setSelectedItemIndex (i, juce::dontSendNotification);
        if (pick.getSelectedId() == 0) pick.setText (cur.isNotEmpty() ? cur : juce::String ("Custom"), juce::dontSendNotification);
    }
    void choose (int id)
    {
        if (id <= 0) return;
        juce::String desc, keySet, label;
        if (id <= numLooks) { desc = lookDesc (layoutLooks[id - 1]); keySet = layoutLooks[id - 1].keys; label = layoutLooks[id - 1].name; }
        else if (id - 100 < (int) mine.size()) { desc = mine[(size_t) (id - 100)].second; label = mine[(size_t) (id - 100)].first; keySet = parseLayout (desc)["keys"]; }
        else return;
        if (id == 1)   // HumHouse default: the color wheel's look
        {
            col::laneMinH = 60;
            if (s.settings) { s.settings->removeValue ("layout"); s.settings->setValue ("layoutName", label); }
            col::lightMode = false; applyTheme (juce::Colour (0xff9b5cff), 0.35f, false);
        }
        else
        {
            applyLayoutDesc (desc);
            if (s.settings) { s.settings->setValue ("layout", desc); s.settings->setValue ("layoutName", label); }
        }
        if (withKeys.getToggleState() && applyKeySet) applyKeySet (keySet);
        if (s.settings) s.settings->saveIfNeeded();
        sync();
    }
    void setCustom (const juce::String& key, const juce::String& value)
    {
        auto v = parseLayout (currentLayout()); v[key] = value;
        const auto desc = joinLayout (v);
        applyLayoutDesc (desc);
        if (s.settings) { s.settings->setValue ("layout", desc); s.settings->setValue ("layoutName", "Custom"); s.settings->saveIfNeeded(); }
        pick.setText ("Custom (save it below)", juce::dontSendNotification);
        sync();
    }
    void loadMine()
    {
        mine.clear();
        if (s.settings == nullptr) return;
        for (auto& line : juce::StringArray::fromLines (s.settings->getValue ("myLayouts")))
            if (line.contains ("|")) mine.push_back ({ line.upToFirstOccurrenceOf ("|", false, false), line.fromFirstOccurrenceOf ("|", false, false) });
    }
    void storeMine()
    {
        juce::StringArray a; for (auto& [n, d] : mine) a.add (n + "|" + d);
        if (s.settings) { s.settings->setValue ("myLayouts", a.joinIntoString ("\n")); s.settings->saveIfNeeded(); }
    }
    void saveMine()
    {
        auto n = name.getText().removeCharacters ("|;\r\n").trim();
        if (n.isEmpty()) n = "My layout " + juce::String ((int) mine.size() + 1);
        auto v = parseLayout (currentLayout());
        if (s.settings) v["keys"] = s.settings->getValue ("layoutKeySet");
        bool replaced = false;
        for (auto& m : mine) if (m.first == n) { m.second = joinLayout (v); replaced = true; }
        if (! replaced) mine.push_back ({ n, joinLayout (v) });
        storeMine();
        if (s.settings) s.settings->setValue ("layoutName", n);
        fillPick();
    }
    void deleteMine()
    {
        const int id = pick.getSelectedId();
        if (id < 100 || id - 100 >= (int) mine.size()) return;
        mine.erase (mine.begin() + (id - 100));
        storeMine(); fillPick();
    }
    void sync()
    {
        static const char* keys[6] { "bg", "panel", "panel2", "gridBar", "text", "accent" };
        auto v = parseLayout (currentLayout());
        for (int i = 0; i < swatches.size(); ++i)
        {
            const auto c = juce::Colour::fromString (v[keys[i]]);
            swatches[i]->setColour (juce::TextButton::buttonColourId, c);
            swatches[i]->setColour (juce::TextButton::textColourOffId, col::contrastOn (c));
        }
        laneH.setValue (col::laneMinH, juce::dontSendNotification);
        delBtn.setEnabled (pick.getSelectedId() >= 100);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        title.setBounds (r.removeFromTop (26)); r.removeFromTop (4);
        pick.setBounds (r.removeFromTop (34)); r.removeFromTop (4);
        withKeys.setBounds (r.removeFromTop (28)); r.removeFromTop (8);
        customLbl.setBounds (r.removeFromTop (22));
        for (int row = 0; row < 2; ++row)
        {
            auto line = r.removeFromTop (36); const int w = line.getWidth() / 3;
            for (int i = 0; i < 3; ++i) swatches[row * 3 + i]->setBounds (line.removeFromLeft (w).reduced (3));
        }
        r.removeFromTop (6);
        auto lr = r.removeFromTop (30); laneLbl.setBounds (lr.removeFromLeft (100)); laneH.setBounds (lr);
        r.removeFromTop (8);
        auto sr = r.removeFromTop (34); name.setBounds (sr.removeFromLeft (sr.getWidth() / 2).reduced (2)); saveBtn.setBounds (sr.reduced (2));
        r.removeFromTop (4);
        auto br = r.removeFromTop (34); delBtn.setBounds (br.removeFromLeft (br.getWidth() / 2).reduced (2)); resetBtn.setBounds (br.reduced (2));
    }
    Studio& s;
    juce::Label title, customLbl, laneLbl;
    juce::ComboBox pick; juce::ToggleButton withKeys;
    juce::OwnedArray<juce::TextButton> swatches;
    juce::Slider laneH; juce::TextEditor name;
    juce::TextButton saveBtn { "Save as my layout" }, delBtn { "Delete my layout" }, resetBtn { "Reset to HumHouse default" };
    std::vector<std::pair<juce::String, juce::String>> mine;
};
}

void applySavedLayout (Studio& s)
{
    if (s.settings != nullptr && s.settings->containsKey ("layout")) applyLayoutDesc (s.settings->getValue ("layout"));
}

void showLayoutDialog (Studio& s)
{
    static juce::Component::SafePointer<juce::DialogWindow> open;
    if (open != nullptr) { delete open.getComponent(); return; }   // tap LAYOUT again to close it
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new LayoutPanel (s));
    o.dialogTitle = "Layout";
    o.dialogBackgroundColour = col::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = ! phoneUI;
    o.resizable = false;
    open = o.launchAsync();
    if (phoneUI && open != nullptr) { open->setTitleBarHeight (44); open->setTitleBarButtonsRequired (juce::DocumentWindow::closeButton, true); }
}

void showThemeDialog (Studio& s)
{
    static juce::Component::SafePointer<juce::DialogWindow> open;
    if (open != nullptr) { delete open.getComponent(); return; }   // tap COLOR again to close it
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new ThemePanel (s));
    o.dialogTitle = "Theme color";
    o.dialogBackgroundColour = col::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = ! phoneUI;   // phone: our title bar with an X to leave
    o.resizable = false;
    open = o.launchAsync();
    if (phoneUI && open != nullptr) { open->setTitleBarHeight (44); open->setTitleBarButtonsRequired (juce::DocumentWindow::closeButton, true); }
}

void drawLogo (juce::Graphics& g, juce::Rectangle<float> area, bool withText)
{
    auto icon = area.removeFromLeft (area.getHeight()).reduced (1.0f);
    const float k = icon.getWidth() / 1024.0f;
    auto X = [&] (float v) { return icon.getX() + v * k; };
    auto Y = [&] (float v) { return icon.getY() + v * k; };
    g.setGradientFill (juce::ColourGradient (col::panel2.brighter (0.25f), icon.getX(), icon.getY(),
                                             col::bg.darker (0.4f), icon.getRight(), icon.getBottom(), false));
    g.fillRoundedRectangle (icon, 220.0f * k);
    g.setGradientFill (juce::ColourGradient (col::accent.brighter (0.45f), icon.getX(), icon.getY(),
                                             col::accent.darker (0.1f), icon.getRight(), icon.getBottom(), false));
    juce::Path roof;
    roof.startNewSubPath (X (232), Y (470)); roof.lineTo (X (512), Y (230)); roof.lineTo (X (792), Y (470));
    g.strokePath (roof, juce::PathStrokeType (64.0f * k, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.fillRoundedRectangle (X (300), Y (470), 72 * k, 330 * k, 36 * k);
    g.fillRoundedRectangle (X (652), Y (470), 72 * k, 330 * k, 36 * k);
    g.fillRoundedRectangle (X (120), Y (520), 68 * k, 250 * k, 34 * k);   // headphone cups either side of the house
    g.fillRoundedRectangle (X (836), Y (520), 68 * k, 250 * k, 34 * k);
    g.setColour (juce::Colours::white);
    const float bx[] { 402, 462, 522, 582 }, bh[] { 120, 240, 170, 80 };
    for (int i = 0; i < 4; ++i) g.fillRoundedRectangle (X (bx[i]), Y (635 - bh[i] / 2), 40 * k, bh[i] * k, 20 * k);
    if (! withText) return;
    area.removeFromLeft (8.0f);
    const float h = area.getHeight();
    float fit = 1.0f;   // shrink the words to the space they get, never cut them off
    {
        juce::GlyphArrangement ga; ga.addLineOfText (juce::Font (juce::FontOptions (h * 0.52f, juce::Font::bold)), "HUMHOUSE", 0.0f, 0.0f);
        const float tw = ga.getBoundingBox (0, -1, true).getWidth() + 2.0f;
        if (tw > area.getWidth() && area.getWidth() > 1.0f) fit = area.getWidth() / tw;
    }
    g.setColour (col::accent);
    g.setFont (juce::FontOptions (h * 0.52f * fit, juce::Font::bold));
    g.drawText ("HUMHOUSE", area.removeFromTop (h * 0.62f), juce::Justification::bottomLeft, false);
    g.setColour (col::dim);
    g.setFont (juce::FontOptions (h * 0.3f * fit, juce::Font::bold));
    g.drawText ("S T U D I O", area, juce::Justification::topLeft, false);
}
}
