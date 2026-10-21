#include "UI.h"

namespace hhs
{
namespace
{
struct Palette { juce::Colour bg, panel, panel2, grid, gridBeat, gridBar, accent, scale, root; };
const Palette defaults { juce::Colour (0xff0e1013), juce::Colour (0xff16191e), juce::Colour (0xff1d2127), juce::Colour (0xff22262d),
                         juce::Colour (0xff2c3139), juce::Colour (0xff3c434e), juce::Colour (0xffff8a3d), juce::Colour (0xff1a2721),
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
        const juce::uint32 presets[] { 0xffff8a3d, 0xffff3b57, 0xffb15cff, 0xff4d7cff, 0xff38d1ff, 0xff3ddc84, 0xffff5db1, 0xffffc93d, 0xffe6e8eb, 0xff8c929b, 0xff000000 };
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
        setupButton (reset, "Reset", "Back to the original HumHouse orange");
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

void showThemeDialog (Studio& s)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new ThemePanel (s));
    o.dialogTitle = "Theme color";
    o.dialogBackgroundColour = col::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
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
    g.setColour (col::accent);
    g.setFont (juce::FontOptions (h * 0.52f, juce::Font::bold));
    g.drawText ("HUMHOUSE", area.removeFromTop (h * 0.62f), juce::Justification::bottomLeft, false);
    g.setColour (col::dim);
    g.setFont (juce::FontOptions (h * 0.3f, juce::Font::bold));
    g.drawText ("S T U D I O", area, juce::Justification::topLeft, false);
}
}
