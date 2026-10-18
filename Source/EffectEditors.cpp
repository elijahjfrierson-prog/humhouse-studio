#include "UI.h"
#include <complex>

namespace hhs
{
namespace
{
class EditorWindow : public juce::DocumentWindow, private juce::Timer
{
public:
    EditorWindow (const juce::String& title, juce::Component* content, juce::String k, std::function<bool()> stillValid,
                  juce::AudioProcessor* p = nullptr)
        : juce::DocumentWindow (title, col::panel, juce::DocumentWindow::closeButton), key (std::move (k)), proc (p), valid (std::move (stillValid))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content, true);
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        makeCaptureFriendly (*this);
        startTimer (400);
    }
    void closeButtonPressed() override { close (this); }
    static void close (EditorWindow* w) { juce::MessageManager::callAsync ([w] { windows().removeObject (w); }); }
    static juce::OwnedArray<EditorWindow>& windows() { static juce::OwnedArray<EditorWindow> w; return w; }
    juce::String key;
    juce::AudioProcessor* proc = nullptr;

private:
    std::function<bool()> valid;
    void timerCallback() override { if (valid && ! valid()) { stopTimer(); close (this); } }
};

bool focusExisting (const juce::String& key)
{
    for (auto* w : EditorWindow::windows()) if (w->key == key) { w->toFront (true); return true; }
    return false;
}

FxSlot* findSlot (Studio& s, int track, int uid)
{
    auto& p = s.p();
    if (track < 0 || track >= (int) p.mixer.size()) return nullptr;
    for (auto& f : p.mixer[(size_t) track].fx) if (f.uid == uid) return &f;
    return nullptr;
}

class ParamGrid : public juce::Component
{
public:
    struct P { juce::String id, name; float mn, mx, def, value; std::vector<const char*> choices; };
    ParamGrid (const juce::String& title, std::vector<P> ps, std::function<void (const juce::String&, float)> setLive, std::function<void()> commit,
               const juce::String& presetKind = {})
        : params (std::move (ps)), onSet (std::move (setLive)), onCommit (std::move (commit)), kind (presetKind)
    {
        if (kind.isNotEmpty())
        {
            setupButton (presetBtn, "PRESETS", "Save or load a preset for this effect / instrument");
            presetBtn.onClick = [this] { showPresetMenu(); };
            addAndMakeVisible (presetBtn);
        }
        heading.setText (title, juce::dontSendNotification);
        heading.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        heading.setColour (juce::Label::textColourId, col::accent);
        addAndMakeVisible (heading);
        for (auto& p : params)
        {
            auto* l = labels.add (new juce::Label ({}, p.name));
            l->setJustificationType (juce::Justification::centred);
            l->setFont (juce::FontOptions (11.0f));
            addAndMakeVisible (l);
            const auto id = p.id;
            if (! p.choices.empty())
            {
                auto* cb = new juce::ComboBox();
                for (size_t c = 0; c < p.choices.size(); ++c) cb->addItem (p.choices[c], (int) c + 1);
                cb->setSelectedItemIndex ((int) std::round (p.value - p.mn), juce::dontSendNotification);
                cb->setTooltip (p.name);
                const float mn = p.mn;
                cb->onChange = [this, cb, id, mn] { set (id, mn + (float) cb->getSelectedItemIndex()); onCommit(); };
                controls.add (cb);
            }
            else
            {
                auto* k = new Knob (p.name, "drag up / down, double-click resets", p.mn, p.mx, p.def);
                k->setValue (p.value, juce::dontSendNotification);
                if (p.mx - p.mn > 100 && p.mn > 0) k->setSkewFactorFromMidPoint (std::sqrt (p.mn * p.mx));
                k->onValueChange = [this, k, id] { set (id, (float) k->getValue()); if (! k->isMouseButtonDown()) onCommit(); };
                k->onDragEnd = [this] { onCommit(); };
                controls.add (k);
            }
            addAndMakeVisible (controls.getLast());
        }
        visibleCount = (int) params.size();
        layoutSize();
    }
    void layoutSize()
    {
        const int cols = juce::jlimit (1, 6, visibleCount);
        const int rows = juce::jmax (1, (visibleCount + cols - 1) / cols);
        setSize (juce::jmax (300, cols * 86 + 20), rows * 92 + 50);
    }
    std::function<void()> onAnyChange;
    void setExternal (const juce::String& id, float v)
    {
        for (size_t i = 0; i < params.size(); ++i)
        {
            if (params[i].id != id) continue;
            v = juce::jlimit (params[i].mn, params[i].mx, v);
            if (auto* cb = dynamic_cast<juce::ComboBox*> (controls[(int) i])) cb->setSelectedItemIndex ((int) std::round (v - params[i].mn), juce::dontSendNotification);
            else if (auto* k = dynamic_cast<juce::Slider*> (controls[(int) i])) k->setValue (v, juce::dontSendNotification);
            set (id, v);
        }
    }
    float valueOf (const juce::String& id) const { for (auto& p : params) if (p.id == id) return p.value; return 0.0f; }
    void commitNow() { onCommit(); }
    void hideWhere (const std::function<bool (const juce::String&)>& hide)
    {
        visibleCount = 0;
        for (size_t i = 0; i < params.size(); ++i)
        {
            const bool h = hide (params[i].id);
            controls[(int) i]->setVisible (! h); labels[(int) i]->setVisible (! h);
            visibleCount += h ? 0 : 1;
        }
        layoutSize();
    }
    void paint (juce::Graphics& g) override { g.fillAll (col::panel); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto top = r.removeFromTop (28);
        if (kind.isNotEmpty()) presetBtn.setBounds (top.removeFromRight (80).reduced (0, 2));
        heading.setBounds (top);
        const int cols = juce::jlimit (1, 6, visibleCount);
        for (int i = 0, v = 0; i < controls.size(); ++i)
        {
            if (! controls[i]->isVisible()) continue;
            juce::Rectangle<int> cell (r.getX() + (v % cols) * 86, r.getY() + (v / cols) * 92, 82, 88);
            ++v;
            labels[i]->setBounds (cell.removeFromTop (16));
            if (dynamic_cast<juce::ComboBox*> (controls[i]) != nullptr) controls[i]->setBounds (cell.withSizeKeepingCentre (80, 24));
            else controls[i]->setBounds (cell.reduced (12, 2));
        }
    }

private:
    juce::String kind;
    int visibleCount = 0;
    juce::TextButton presetBtn;
    std::unique_ptr<juce::FileChooser> chooser;
    void set (const juce::String& id, float v)
    {
        for (auto& p : params) if (p.id == id) p.value = v;
        onSet (id, v);
        if (onAnyChange) onAnyChange();
    }
    juce::File presetDir() const
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("HumHouse Studio")
            .getChildFile ("Presets").getChildFile (juce::File::createLegalFileName (kind));
    }
    void showPresetMenu()
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Save preset...");
        auto files = presetDir().findChildFiles (juce::File::findFiles, false, "*.hhpreset");
        files.sort();
        if (! files.isEmpty()) menu.addSeparator();
        for (int i = 0; i < files.size() && i < 200; ++i) menu.addItem (100 + i, files[i].getFileNameWithoutExtension());
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetBtn), [this, files] (int r) {
            if (r == 1) savePreset();
            else if (r >= 100 && r - 100 < files.size()) loadPreset (files[r - 100]);
        });
    }
    void savePreset()
    {
        auto* aw = new juce::AlertWindow ("Save preset", "Preset name", juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("name", "My preset");
        aw->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<ParamGrid> safe (this);
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw] (int r) {
            const auto name = juce::File::createLegalFileName (aw->getTextEditorContents ("name").trim());
            if (r != 1 || name.isEmpty() || safe == nullptr) return;
            juce::XmlElement x ("HUMHOUSE_PRESET");
            x.setAttribute ("kind", safe->kind);
            for (auto& p : safe->params) { auto* e = x.createNewChildElement ("P"); e->setAttribute ("id", p.id); e->setAttribute ("v", (double) p.value); }
            safe->presetDir().createDirectory();
            x.writeTo (safe->presetDir().getChildFile (name + ".hhpreset"));
        }), true);
    }
    void loadPreset (const juce::File& f)
    {
        auto x = juce::XmlDocument::parse (f);
        if (x == nullptr) return;
        for (auto* e : x->getChildWithTagNameIterator ("P"))
        {
            const auto id = e->getStringAttribute ("id");
            for (size_t i = 0; i < params.size(); ++i)
            {
                if (params[i].id != id) continue;
                const float v = juce::jlimit (params[i].mn, params[i].mx, (float) e->getDoubleAttribute ("v", params[i].value));
                if (auto* cb = dynamic_cast<juce::ComboBox*> (controls[(int) i])) cb->setSelectedItemIndex ((int) std::round (v - params[i].mn), juce::dontSendNotification);
                else if (auto* k = dynamic_cast<juce::Slider*> (controls[(int) i])) k->setValue (v, juce::dontSendNotification);
                set (id, v);
            }
        }
        onCommit();
    }
    std::vector<P> params;
    std::function<void (const juce::String&, float)> onSet;
    std::function<void()> onCommit;
    juce::Label heading;
    juce::OwnedArray<juce::Label> labels;
    juce::OwnedArray<juce::Component> controls;
};

// ---- visual editors: EQ curve with draggable bands, compressor transfer curve + GR, effect pictures
static void drawPanelBg (juce::Graphics& g, juce::Rectangle<float> a)
{
    g.setColour (col::bg.darker (0.35f)); g.fillRoundedRectangle (a, 6.0f);
    g.setColour (col::grid.withAlpha (0.6f)); g.drawRoundedRectangle (a, 6.0f, 1.0f);
}

class EqGraph : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    EqGraph (ParamGrid& g, std::function<BuiltinEffect*()> liveFx, std::function<ScopeTap*()> src)
        : grid (g), live (std::move (liveFx)), tap (std::move (src))
    {
        model = createBuiltinEffect ("steq7");
        if (model) model->prepare (48000, 512);
        snap.assign (fftN, 0.0f); fftBuf.assign (fftN * 2, 0.0f); spec.assign (fftN / 2, -100.0f);
        setTooltip ("Drag a dot: left/right = frequency, up/down = gain. Wheel = Q. Double-click = reset. Right-click = Dynamic EQ. Click NATURAL/LINEAR to switch phase.");
        startTimerHz (30);
    }
    void paint (juce::Graphics& g) override
    {
        auto a = area();
        drawPanelBg (g, getLocalBounds().toFloat());
        g.setFont (juce::FontOptions (10.0f));
        for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const float x = xOf (f);
            g.setColour (col::grid.withAlpha (0.5f)); g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
            g.setColour (col::dim); g.drawText (f >= 1000 ? juce::String (f / 1000, 0) + "k" : juce::String (f, 0), (int) x + 2, (int) a.getBottom() - 12, 30, 12, juce::Justification::left);
        }
        for (int d : { -18, -12, -6, 0, 6, 12, 18 })
        {
            g.setColour (col::grid.withAlpha (d == 0 ? 0.9f : 0.35f)); g.drawHorizontalLine ((int) yOf ((float) d), a.getX(), a.getRight());
            if (d != 0) { g.setColour (col::dim); g.drawText (juce::String (d), (int) a.getX() + 2, (int) yOf ((float) d) - 6, 24, 12, juce::Justification::left); }
        }
        // live spectrum
        if (rate > 0)
        {
            juce::Path sp; bool started = false;
            for (int i = 1; i < fftN / 2; ++i)
            {
                const double f = i * rate / fftN;
                if (f < 20 || f > 20000) continue;
                const float y = juce::jmap (juce::jlimit (-90.0f, 0.0f, spec[(size_t) i]), -90.0f, 0.0f, a.getBottom(), a.getY() + a.getHeight() * 0.15f);
                if (! started) { sp.startNewSubPath (xOf (f), a.getBottom()); started = true; }
                sp.lineTo (xOf (f), y);
            }
            if (started)
            {
                sp.lineTo (a.getRight(), a.getBottom()); sp.closeSubPath();
                g.setColour (col::text.withAlpha (0.08f)); g.fillPath (sp);
            }
        }
        // EQ curve
        if (model)
        {
            for (auto& d : model->params()) model->setParam (d.id, grid.valueOf (d.id));
            juce::Path c;
            for (float x = a.getX(); x <= a.getRight(); x += 2.0f)
            {
                const float y = yOf (model->responseDb (freqAt (x)));
                if (x == a.getX()) c.startNewSubPath (x, y); else c.lineTo (x, y);
            }
            juce::Path fill (c); fill.lineTo (a.getRight(), yOf (0)); fill.lineTo (a.getX(), yOf (0)); fill.closeSubPath();
            g.setColour (col::accent.withAlpha (0.16f)); g.fillPath (fill);
            g.setColour (col::accent.withAlpha (0.35f)); g.strokePath (c, juce::PathStrokeType (5.0f));
            g.setColour (col::accent); g.strokePath (c, juce::PathStrokeType (2.0f));
        }
        // band dots (+ live dynamic ring)
        auto* fx = live ? live() : nullptr;
        for (int b = 0; b < (int) bands().size(); ++b)
        {
            auto pt = dotOf (b);
            const auto c = juce::Colour (bands()[(size_t) b].colour);
            const int k = bands()[(size_t) b].dyn;
            if (k >= 0 && grid.valueOf (dynId (k, "Dyn")) > 0.5f)
            {
                const float off = fx ? fx->meter (k) : 0.0f;
                const float yr = yOf (gainOf (b) + off);
                g.setColour (c.withAlpha (0.5f)); g.drawLine (pt.x, pt.y, pt.x, yr, 1.5f);
                g.setColour (c); g.drawEllipse (pt.x - 10, yr - 10, 20, 20, 2.0f);
            }
            g.setColour (c.withAlpha (b == hot ? 0.45f : 0.2f)); g.fillEllipse (pt.x - 11, pt.y - 11, 22, 22);
            g.setColour (c); g.fillEllipse (pt.x - 6, pt.y - 6, 12, 12);
            if (b == hot)
            {
                g.setColour (col::text); g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
                const auto& bd = bands()[(size_t) b];
                juce::String t = juce::String (bd.name) + "  " + hzText (grid.valueOf (bd.f));
                if (bd.g) t << "  " << juce::String (grid.valueOf (bd.g), 1) << " dB";
                if (bd.q) t << "  Q " << juce::String (grid.valueOf (bd.q), 2);
                g.drawText (t, (int) pt.x - 90, (int) pt.y - 30, 180, 14, juce::Justification::centred);
            }
        }
        // phase mode chip
        const bool lin = grid.valueOf ("phase") > 0.5f;
        auto chip = modeChip();
        g.setColour (lin ? col::accent2.withAlpha (0.3f) : col::good.withAlpha (0.25f)); g.fillRoundedRectangle (chip, 4.0f);
        g.setColour (col::text); g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (lin ? "LINEAR  ~23 ms" : "NATURAL  0 ms", chip, juce::Justification::centred);
    }
    void mouseMove (const juce::MouseEvent& e) override { const int h = hit (e.position); if (h != hot) { hot = h; repaint(); } }
    void mouseExit (const juce::MouseEvent&) override { hot = -1; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (modeChip().contains (e.position)) { grid.setExternal ("phase", grid.valueOf ("phase") > 0.5f ? 0.0f : 1.0f); grid.commitNow(); repaint(); return; }
        drag = hit (e.position); hot = drag;
        if (drag >= 0 && e.mods.isPopupMenu()) { bandMenu (drag); drag = -1; }
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag < 0) return;
        const auto& bd = bands()[(size_t) drag];
        grid.setExternal (bd.f, (float) freqAt (e.position.x));
        if (bd.g) grid.setExternal (bd.g, juce::jlimit (-18.0f, 18.0f, dbAt (e.position.y)));
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override { if (drag >= 0) grid.commitNow(); drag = -1; }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int b = hit (e.position);
        if (b < 0) return;
        const auto& bd = bands()[(size_t) b];
        if (bd.g) grid.setExternal (bd.g, 0.0f); else grid.setExternal (bd.f, b == 0 ? 20.0f : 22000.0f);
        grid.commitNow(); repaint();
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const int b = hit (e.position);
        if (b < 0 || bands()[(size_t) b].q == nullptr) return;
        const auto* q = bands()[(size_t) b].q;
        grid.setExternal (q, grid.valueOf (q) * (w.deltaY > 0 ? 1.12f : 1.0f / 1.12f));
        grid.commitNow(); repaint();
    }
private:
    struct Band { const char* g; const char* f; const char* q; juce::uint32 colour; int dyn; const char* name; };
    static const std::vector<Band>& bands()
    {
        static const std::vector<Band> b {
            { nullptr, "hpf", nullptr, 0xff9aa0aa, -1, "HPF" }, { "lsG", "lsF", nullptr, 0xffff5d5d, 0, "Low Shelf" },
            { "b1G", "b1F", "b1Q", 0xffffa53d, 1, "LMF" }, { "b2G", "b2F", "b2Q", 0xfff5e04a, 2, "MF" },
            { "b3G", "b3F", "b3Q", 0xff4ade80, 3, "HMF" }, { "b4G", "b4F", "b4Q", 0xff38bdf8, 4, "Presence" },
            { "hsG", "hsF", nullptr, 0xffa78bfa, 5, "High Shelf" }, { nullptr, "lpf", nullptr, 0xff9aa0aa, -1, "LPF" } };
        return b;
    }
    static juce::String dynId (int k, const char* suffix)
    {
        static const char* pre[] { "ls", "b1", "b2", "b3", "b4", "hs" };
        return juce::String (pre[k]) + suffix;
    }
    static juce::String hzText (float f) { return f >= 1000 ? juce::String (f / 1000.0f, 2) + " kHz" : juce::String ((int) f) + " Hz"; }
    juce::Rectangle<float> area() const { return getLocalBounds().toFloat().reduced (6.0f, 8.0f); }
    juce::Rectangle<float> modeChip() const { return { (float) getWidth() - 128.0f, 8.0f, 118.0f, 20.0f }; }
    float xOf (double f) const { auto a = area(); return a.getX() + a.getWidth() * (float) (std::log (juce::jlimit (20.0, 20000.0, f) / 20.0) / std::log (1000.0)); }
    double freqAt (float x) const { auto a = area(); return 20.0 * std::pow (1000.0, juce::jlimit (0.0, 1.0, (double) ((x - a.getX()) / a.getWidth()))); }
    float yOf (float db) const { auto a = area(); return a.getCentreY() - juce::jlimit (-20.0f, 20.0f, db) / 20.0f * a.getHeight() * 0.5f; }
    float dbAt (float y) const { auto a = area(); return (a.getCentreY() - y) / (a.getHeight() * 0.5f) * 20.0f; }
    float gainOf (int b) const { const auto* gid = bands()[(size_t) b].g; return gid ? grid.valueOf (gid) : 0.0f; }
    juce::Point<float> dotOf (int b) const { return { xOf (grid.valueOf (bands()[(size_t) b].f)), yOf (gainOf (b)) }; }
    int hit (juce::Point<float> p) const
    {
        int best = -1; float bd = 14.0f;
        for (int b = 0; b < (int) bands().size(); ++b) { const float d = dotOf (b).getDistanceFrom (p); if (d < bd) { bd = d; best = b; } }
        return best;
    }
    void bandMenu (int b)
    {
        const int k = bands()[(size_t) b].dyn;
        if (k < 0) return;
        const bool on = grid.valueOf (dynId (k, "Dyn")) > 0.5f;
        juce::PopupMenu m, th, rg, at, rl;
        m.addSectionHeader (juce::String (bands()[(size_t) b].name) + " - Dynamic EQ");
        m.addItem (1, "Dynamic", true, on);
        m.addItem (2, "Only this band dynamic");
        m.addItem (3, "Make all bands dynamic");
        m.addItem (4, "Make all bands static");
        m.addSeparator();
        const float curTh = grid.valueOf (dynId (k, "DTh")), curRg = grid.valueOf (dynId (k, "DRg"));
        for (int v : { 12, 24, 36, 48 }) th.addItem (100 + v, "-" + juce::String (v) + " dB", true, (int) std::round (-curTh) == v);
        m.addSubMenu ("Threshold", th);
        for (int v : { -12, -6, -3, 3, 6, 12 }) rg.addItem (200 + v + 50, (v > 0 ? "+" : "") + juce::String (v) + " dB" + (v < 0 ? " (cut when loud)" : " (boost when loud)"), true, (int) std::round (curRg) == v);
        m.addSubMenu ("Range", rg);
        for (int v : { 1, 5, 20, 50 }) at.addItem (400 + v, juce::String (v) + " ms", true, (int) std::round (grid.valueOf (dynId (k, "DAt"))) == v);
        m.addSubMenu ("Attack", at);
        for (int v : { 50, 120, 300, 800 }) rl.addItem (1000 + v, juce::String (v) + " ms", true, (int) std::round (grid.valueOf (dynId (k, "DRl"))) == v);
        m.addSubMenu ("Release", rl);
        juce::Component::SafePointer<EqGraph> safe (this);
        m.showMenuAsync ({}, [safe, k, on] (int r) {
            if (safe == nullptr || r <= 0) return;
            auto& gr = safe->grid;
            if (r == 1) gr.setExternal (dynId (k, "Dyn"), on ? 0.0f : 1.0f);
            else if (r == 2) for (int i = 0; i < 6; ++i) gr.setExternal (dynId (i, "Dyn"), i == k ? 1.0f : 0.0f);
            else if (r == 3 || r == 4) for (int i = 0; i < 6; ++i) gr.setExternal (dynId (i, "Dyn"), r == 3 ? 1.0f : 0.0f);
            else if (r < 200) { gr.setExternal (dynId (k, "DTh"), (float) -(r - 100)); gr.setExternal (dynId (k, "Dyn"), 1.0f); }
            else if (r < 400) { gr.setExternal (dynId (k, "DRg"), (float) (r - 250)); gr.setExternal (dynId (k, "Dyn"), 1.0f); }
            else if (r < 1000) gr.setExternal (dynId (k, "DAt"), (float) (r - 400));
            else gr.setExternal (dynId (k, "DRl"), (float) (r - 1000));
            gr.commitNow(); safe->repaint();
        });
    }
    void timerCallback() override
    {
        if (! isShowing()) return;
        if (auto* t = tap ? tap() : nullptr)
        {
            rate = t->rate.load();
            t->read (snap.data(), fftN, t->pos.load());
            std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);
            for (int i = 0; i < fftN; ++i) fftBuf[(size_t) i] = snap[(size_t) i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftN));
            fft.performFrequencyOnlyForwardTransform (fftBuf.data());
            for (int i = 0; i < fftN / 2; ++i)
            {
                const float db = juce::Decibels::gainToDecibels (fftBuf[(size_t) i] / (float) (fftN / 4), -100.0f);
                spec[(size_t) i] = db > spec[(size_t) i] ? db : spec[(size_t) i] * 0.85f + db * 0.15f;
            }
        }
        repaint();
    }
    static constexpr int fftOrder = 11, fftN = 1 << fftOrder;
    ParamGrid& grid;
    std::function<BuiltinEffect*()> live;
    std::function<ScopeTap*()> tap;
    std::unique_ptr<BuiltinEffect> model;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> snap, fftBuf, spec;
    double rate = 0;
    int hot = -1, drag = -1;
};

// compressor: transfer curve (in -> out), threshold line and live gain-reduction meter
class DynGraph : public juce::Component, private juce::Timer
{
public:
    DynGraph (ParamGrid& g, std::function<BuiltinEffect*()> liveFx, juce::String th, juce::String ratio, juce::String knee)
        : grid (g), live (std::move (liveFx)), thId (std::move (th)), ratioId (std::move (ratio)), kneeId (std::move (knee)) { startTimerHz (30); }
    void paint (juce::Graphics& g) override
    {
        drawPanelBg (g, getLocalBounds().toFloat());
        auto r = getLocalBounds().toFloat().reduced (10.0f);
        auto meter = r.removeFromRight (40.0f); r.removeFromRight (12.0f);
        auto sq = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), r.getHeight()), juce::jmin (r.getWidth(), r.getHeight()));
        const float th = grid.valueOf (thId), ratio = juce::jmax (1.0f, grid.valueOf (ratioId)), knee = kneeId.isNotEmpty() ? grid.valueOf (kneeId) : 6.0f;
        auto px = [&] (float db) { return sq.getX() + (db + 60.0f) / 60.0f * sq.getWidth(); };
        auto py = [&] (float db) { return sq.getBottom() - (juce::jlimit (-60.0f, 0.0f, db) + 60.0f) / 60.0f * sq.getHeight(); };
        g.setColour (col::grid.withAlpha (0.5f));
        for (int d = -48; d < 0; d += 12) { g.drawVerticalLine ((int) px ((float) d), sq.getY(), sq.getBottom()); g.drawHorizontalLine ((int) py ((float) d), sq.getX(), sq.getRight()); }
        g.setColour (col::dim.withAlpha (0.6f)); g.drawLine (sq.getX(), sq.getBottom(), sq.getRight(), sq.getY(), 1.0f);
        juce::Path c;
        for (float in = -60.0f; in <= 0.0f; in += 0.5f)
        {
            const float over = in - th;
            float out = in;
            if (knee > 0 && 2 * std::abs (over) <= knee) out = in + (1 / ratio - 1) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
            else if (2 * over > knee) out = th + over / ratio;
            if (in == -60.0f) c.startNewSubPath (px (in), py (out)); else c.lineTo (px (in), py (out));
        }
        g.setColour (col::accent.withAlpha (0.35f)); g.strokePath (c, juce::PathStrokeType (5.0f));
        g.setColour (col::accent); g.strokePath (c, juce::PathStrokeType (2.0f));
        g.setColour (col::accent2.withAlpha (0.7f)); g.drawVerticalLine ((int) px (th), sq.getY(), sq.getBottom());
        g.setColour (col::text); g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText ("THRESH " + juce::String (th, 1) + " dB   RATIO " + juce::String (ratio, 1) + ":1", sq.toNearestInt().removeFromTop (16), juce::Justification::centredLeft);
        const float gr = std::abs (live && live() ? live()->meter (0) : 0.0f);
        shown = std::max (gr, shown * 0.9f);
        g.setColour (col::bg); g.fillRoundedRectangle (meter, 4.0f);
        g.setColour (col::rec.withAlpha (0.85f));
        g.fillRoundedRectangle (meter.withHeight (meter.getHeight() * juce::jlimit (0.0f, 1.0f, shown / 24.0f)), 4.0f);
        g.setColour (col::text); g.setFont (juce::FontOptions (10.0f));
        g.drawText ("GR", meter.toNearestInt().removeFromBottom (28), juce::Justification::centred);
        g.drawText ("-" + juce::String (shown, 1), meter.toNearestInt().removeFromBottom (14), juce::Justification::centred);
    }
private:
    void timerCallback() override { if (isShowing()) repaint(); }
    ParamGrid& grid;
    std::function<BuiltinEffect*()> live;
    juce::String thId, ratioId, kneeId;
    float shown = 0;
};

// simple pictures of what an effect does (reverb tail, echo taps, filter curve)
class FxPicture : public juce::Component, private juce::Timer
{
public:
    FxPicture (ParamGrid& g, juce::String t) : grid (g), type (std::move (t)) { startTimerHz (15); }
    void paint (juce::Graphics& g) override
    {
        drawPanelBg (g, getLocalBounds().toFloat());
        auto a = getLocalBounds().toFloat().reduced (12.0f, 14.0f);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        if (type == "verb")
        {
            const float size = grid.valueOf ("size"), damp = grid.valueOf ("damp"), mix = grid.valueOf ("mix");
            const float decay = 0.08f + 0.9f * size;
            juce::Random rnd (7);
            const auto c = col::accent.interpolatedWith (col::accent2, damp);
            for (float x = 0; x < a.getWidth(); x += 3.0f)
            {
                const float t = x / a.getWidth();
                const float h = (t < 0.02f ? t / 0.02f : std::exp (-t / decay)) * (0.55f + 0.45f * rnd.nextFloat()) * (0.3f + 0.7f * mix);
                g.setColour (c.withAlpha (0.35f + 0.6f * (1.0f - t)));
                g.fillRect (a.getX() + x, a.getBottom() - h * a.getHeight(), 2.0f, h * a.getHeight());
            }
            g.setColour (col::text); g.drawText ("REVERB TAIL  size " + juce::String ((int) (size * 100)) + "%  damp " + juce::String ((int) (damp * 100)) + "%", a.toNearestInt().removeFromTop (14), juce::Justification::centredLeft);
        }
        else if (type == "delay")
        {
            static const float spacing[] { 0.125f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
            const int ti = juce::jlimit (0, 6, (int) std::round (grid.valueOf ("time")));
            const float fb = grid.valueOf ("feedback"), mix = grid.valueOf ("mix");
            const bool pp = grid.valueOf ("pingpong") > 0.5f;
            const float step = a.getWidth() * spacing[ti] / 4.0f;
            g.setColour (col::text); g.fillRect (a.getX(), a.getY() + 16, 4.0f, a.getHeight() - 16);
            float h = mix;
            for (int k = 1; a.getX() + k * step < a.getRight() && h > 0.01f; ++k, h *= fb)
            {
                const float x = a.getX() + k * step, hh = (a.getHeight() - 16) * juce::jmin (1.0f, h * 1.6f);
                g.setColour ((pp && (k % 2 == 0) ? col::accent2 : col::accent).withAlpha (0.9f));
                g.fillRoundedRectangle (x, a.getBottom() - hh, 4.0f, hh, 2.0f);
            }
            g.setColour (col::text); g.drawText ("ECHO TAPS  feedback " + juce::String ((int) (fb * 100)) + "%" + (pp ? "  ping-pong" : ""), a.toNearestInt().removeFromTop (14), juce::Justification::centredLeft);
        }
        else if (type == "filter")
        {
            const int mode = (int) std::round (grid.valueOf ("mode"));
            const float cut = grid.valueOf ("cutoff"), res = grid.valueOf ("res");
            Biquad q; q.set (mode == 0 ? Biquad::LowPass : mode == 1 ? Biquad::HighPass : Biquad::BandPass, 48000, cut, 0.707 + res * 8.0, 0);
            juce::Path c;
            for (float x = 0; x <= a.getWidth(); x += 2.0f)
            {
                const double f = 20.0 * std::pow (1000.0, x / a.getWidth()), w = juce::MathConstants<double>::twoPi * f / 48000.0;
                const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
                const double m = std::abs (((double) q.b0 + (double) q.b1 * z1 + (double) q.b2 * z2) / (1.0 + (double) q.a1 * z1 + (double) q.a2 * z2));
                const float db = juce::jlimit (-36.0f, 18.0f, (float) (20.0 * std::log10 (std::max (1.0e-6, m))));
                const float y = juce::jmap (db, -36.0f, 18.0f, a.getBottom(), a.getY() + 16);
                if (x == 0) c.startNewSubPath (a.getX() + x, y); else c.lineTo (a.getX() + x, y);
            }
            g.setColour (col::accent.withAlpha (0.35f)); g.strokePath (c, juce::PathStrokeType (5.0f));
            g.setColour (col::accent); g.strokePath (c, juce::PathStrokeType (2.0f));
            g.setColour (col::text); g.drawText ("FILTER  " + juce::String (cut >= 1000 ? juce::String (cut / 1000, 1) + " kHz" : juce::String ((int) cut) + " Hz"), a.toNearestInt().removeFromTop (14), juce::Justification::centredLeft);
        }
    }
private:
    void timerCallback() override { if (isShowing()) repaint(); }
    ParamGrid& grid;
    juce::String type;
};

class FxEditor : public juce::Component
{
public:
    FxEditor (ParamGrid* g, juce::Component* v, int visualH) : grid (g), visual (v), vh (visualH)
    {
        addAndMakeVisible (*grid); addAndMakeVisible (*visual);
        setSize (juce::jmax (640, grid->getWidth()), vh + grid->getHeight());
    }
    void paint (juce::Graphics& g) override { g.fillAll (col::panel); }
    void resized() override { auto r = getLocalBounds(); visual->setBounds (r.removeFromTop (vh).reduced (10, 8)); grid->setBounds (r); }
private:
    std::unique_ptr<ParamGrid> grid;
    std::unique_ptr<juce::Component> visual;
    int vh;
};
}

void openEffectEditor (Studio& s, int track, int slotUid)
{
    auto* slot = findSlot (s, track, slotUid);
    if (slot == nullptr) return;
    const auto key = "fx:" + juce::String (track) + ":" + juce::String (slotUid);
    if (focusExisting (key)) return;
    const auto trackName = track == 0 ? juce::String ("Master") : s.p().mixer[(size_t) track].name;

    if (slot->type == "plugin")
    {
        auto* plugin = s.engine.fxPlugin (track, slotUid);
        if (plugin == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Plugin House", "This plugin could not be loaded on this computer.");
            return;
        }
        s.engine.beginRiskyPluginCall (plugin->getPluginDescription());
        juce::Component* ed = plugin->hasEditor() ? plugin->createEditorIfNeeded() : nullptr;
        s.engine.endRiskyPluginCall();
        if (ed == nullptr) ed = new juce::GenericAudioProcessorEditor (*plugin);
        EditorWindow::windows().add (new EditorWindow (plugin->getName() + " - " + trackName, ed, key,
                                                       [&s, track, slotUid, plugin] { return s.engine.fxPlugin (track, slotUid) == plugin; }, plugin));
        return;
    }

    if (slot->type == "scope")
    {
        auto* view = createScopeView ([&s, track, slotUid]() -> ScopeTap* {
            auto* sc = dynamic_cast<ScopeFx*> (s.engine.builtinFx (track, slotUid));
            return sc != nullptr ? sc->tap.get() : nullptr; }, trackName);
        auto* w = new EditorWindow ("Scope - " + trackName, view, key, [&s, track, slotUid] { return findSlot (s, track, slotUid) != nullptr; });
        w->setResizable (true, false); w->setResizeLimits (420, 240, 3000, 2000);
        EditorWindow::windows().add (w);
        return;
    }
    auto fx = createBuiltinEffect (slot->type);
    if (fx == nullptr) return;
    juce::String title = slot->type;
    for (auto& inf : builtinEffects()) if (slot->type == inf.id) title = inf.name;
    std::vector<ParamGrid::P> ps;
    for (auto& d : fx->params())
    {
        auto it = slot->params.find (d.id);
        ps.push_back ({ d.id, d.name, d.min, d.max, d.def, it != slot->params.end() ? it->second : d.def, d.choices });
    }
    auto* grid = new ParamGrid (title + "  |  " + trackName, ps,
        [&s, track, slotUid] (const juce::String& id, float v) {
            s.live ([&] (Project&) { if (auto* sl = findSlot (s, track, slotUid)) sl->params[id] = v; });
            if (auto* e = s.engine.builtinFx (track, slotUid)) e->setParam (id, v);
        },
        [&s, title] { s.commit (title); }, slot->type);
    auto liveFx = [&s, track, slotUid] { return s.engine.builtinFx (track, slotUid); };
    auto tapFn = [&s, track]() -> ScopeTap* { return s.engine.trackScope (track); };
    const auto& ty = slot->type;
    juce::Component* visual = nullptr;
    int vh = 170;
    if (ty == "steq7")
    {
        grid->hideWhere ([] (const juce::String& id) { return id.length() > 3 && juce::StringArray { "Dyn", "DTh", "DRg", "DAt", "DRl" }.contains (id.getLastCharacters (3)); });
        visual = new EqGraph (*grid, liveFx, tapFn); vh = 300;
    }
    else if (ty == "stdyn" || ty == "comp") visual = new DynGraph (*grid, liveFx, "threshold", "ratio", "knee");
    else if (ty == "strip") visual = new DynGraph (*grid, liveFx, "compTh", "compRatio", {});
    else if (ty == "verb" || ty == "delay" || ty == "filter") visual = new FxPicture (*grid, ty);
    else visual = createScopeView (tapFn, trackName);
    auto* editor = new FxEditor (grid, visual, vh);
    EditorWindow::windows().add (new EditorWindow (title + " - " + trackName, editor, key,
                                                   [&s, track, slotUid] { return findSlot (s, track, slotUid) != nullptr; }));
}


// FL-style channel settings: SMP / INS / FUNC pages (the "wrench" panel)
// sampler WAVE page: the sample after SMP edits (normalize, reverse, trim, fades, stretch), Ableton-style
struct SampleWaveView : juce::Component, private juce::Timer
{
    SampleWaveView (Studio& st, int id) : s (st), channelId (id) { setSize (560, 300); startTimer (250); timerCallback(); }
    void timerCallback() override
    {
        auto* c = s.channel (channelId);
        if (c == nullptr) return;
        const juce::String k = c->sampleFile.getFullPathName() + juce::String ((int) c->smpNormalize) + juce::String ((int) c->smpReverse)
            + juce::String ((int) c->smpInvert) + juce::String ((int) c->smpDeclick) + juce::String ((int) c->smpTrim) + juce::String (c->smpBoostDb)
            + "|" + juce::String (c->smpFadeIn) + "|" + juce::String (c->smpFadeOut) + "|" + juce::String (c->stretchMode) + "|" + juce::String (c->stretchTime);
        if (k == key) return;
        key = k;
        const Channel copy = *c;
        auto raw = loadSampleFile (copy.sampleFile, 44100.0);
        auto done = raw != nullptr ? processSample (raw, copy, 44100.0) : nullptr;
        peaks.clear();
        if (done != nullptr && done->getNumSamples() > 0)
        {
            const int cols = 1024, n = done->getNumSamples();
            for (int x = 0; x < cols; ++x)
            {
                const int a = (int) ((int64_t) n * x / cols), b = juce::jmax (a + 1, (int) ((int64_t) n * (x + 1) / cols));
                float lo = 0, hi = 0;
                for (int ch = 0; ch < done->getNumChannels(); ++ch)
                {
                    auto r = done->findMinMax (ch, a, juce::jmin (b, n) - a);
                    lo = juce::jmin (lo, r.getStart()); hi = juce::jmax (hi, r.getEnd());
                }
                peaks.push_back ({ lo, hi });
            }
            seconds = (double) n / 44100.0;
        }
        fadeIn = copy.smpFadeIn; fadeOut = copy.smpFadeOut;
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        auto a = getLocalBounds().toFloat().reduced (8);
        g.fillAll (col::panel);
        g.setColour (col::bg); g.fillRoundedRectangle (a, 6);
        g.setColour (col::grid);
        for (int i = 1; i < 8; ++i) g.drawVerticalLine ((int) (a.getX() + a.getWidth() * (float) i / 8.0f), a.getY(), a.getBottom());
        g.drawHorizontalLine ((int) a.getCentreY(), a.getX(), a.getRight());
        if (peaks.empty()) { g.setColour (col::dim); g.drawText ("No sample loaded", a, juce::Justification::centred); return; }
        juce::Path p; const float mid = a.getCentreY(), hh = a.getHeight() * 0.46f;
        for (size_t i = 0; i < peaks.size(); ++i)
            p.lineTo (a.getX() + a.getWidth() * (float) i / (float) (peaks.size() - 1), mid - peaks[i].second * hh);
        for (size_t i = peaks.size(); i-- > 0;)
            p.lineTo (a.getX() + a.getWidth() * (float) i / (float) (peaks.size() - 1), mid - peaks[i].first * hh);
        p.closeSubPath();
        g.setColour (col::accent.withAlpha (0.25f)); g.fillPath (p);
        g.setColour (col::accent); g.strokePath (p, juce::PathStrokeType (1.0f));
        g.setColour (col::accent2);
        if (fadeIn > 0 && seconds > 0) g.drawLine (a.getX(), a.getBottom(), a.getX() + a.getWidth() * (float) juce::jmin (1.0, fadeIn / seconds), a.getY(), 1.5f);
        if (fadeOut > 0 && seconds > 0) g.drawLine (a.getRight() - a.getWidth() * (float) juce::jmin (1.0, fadeOut / seconds), a.getY(), a.getRight(), a.getBottom(), 1.5f);
        g.setColour (col::dim); g.setFont (juce::FontOptions (11.0f));
        g.drawText (juce::String (seconds, 2) + " s", a.reduced (6), juce::Justification::topRight);
    }
    Studio& s; int channelId; juce::String key;
    std::vector<std::pair<float, float>> peaks; double seconds = 0; float fadeIn = 0, fadeOut = 0;
};

struct ChannelTabs : juce::Component
{
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    int w = 360, h = 200;
    ChannelTabs() { tabs.setTabBarDepth (28); tabs.setOutline (0); addAndMakeVisible (tabs); }
    void add (const juce::String& name, juce::Component* page)
    {
        w = juce::jmax (w, page->getWidth()); h = juce::jmax (h, page->getHeight());
        tabs.addTab (name, col::panel, page, true);
    }
    void finish() { setSize (w, h + tabs.getTabBarDepth()); tabs.setCurrentTabIndex (0); }
    void resized() override { tabs.setBounds (getLocalBounds()); }
};

void openChannelEditor (Studio& s, int channelId)
{
    auto* ch = s.channel (channelId);
    if (ch == nullptr) return;
    const auto key = "ch:" + juce::String (channelId);
    if (focusExisting (key)) return;
    if (ch->type == ChannelType::Plugin)
    {
        auto* plugin = s.engine.channelPlugin (channelId);
        if (plugin == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Plugin House", "This instrument could not be loaded on this computer.");
            return;
        }
        s.engine.beginRiskyPluginCall (plugin->getPluginDescription());
        juce::Component* ed = plugin->hasEditor() ? plugin->createEditorIfNeeded() : nullptr;
        s.engine.endRiskyPluginCall();
        if (ed == nullptr) ed = new juce::GenericAudioProcessorEditor (*plugin);
        EditorWindow::windows().add (new EditorWindow (ch->name, ed, key, [&s, channelId, plugin] { return s.engine.channelPlugin (channelId) == plugin; }, plugin));
        return;
    }
    const bool sampler = ch->type == ChannelType::Sampler;
    std::vector<ParamGrid::P> smp, ins, func;
    if (sampler)
    {
        smp.push_back ({ "smpNorm", "Normalize", 0, 1, 0, ch->smpNormalize ? 1.0f : 0.0f, { "Off", "On" } });
        smp.push_back ({ "smpBoost", "Boost (dB)", -24, 24, 0, ch->smpBoostDb, {} });
        smp.push_back ({ "smpRev", "Reverse", 0, 1, 0, ch->smpReverse ? 1.0f : 0.0f, { "Off", "On" } });
        smp.push_back ({ "smpInv", "Polarity", 0, 1, 0, ch->smpInvert ? 1.0f : 0.0f, { "Normal", "Inverted" } });
        smp.push_back ({ "smpDeclick", "Declick", 0, 1, 1, ch->smpDeclick ? 1.0f : 0.0f, { "Off", "On" } });
        smp.push_back ({ "smpTrim", "Trim silence", 0, 1, 0, ch->smpTrim ? 1.0f : 0.0f, { "Off", "On" } });
        smp.push_back ({ "smpFadeIn", "Fade in (s)", 0, 2, 0, ch->smpFadeIn, {} });
        smp.push_back ({ "smpFadeOut", "Fade out (s)", 0, 2, 0, ch->smpFadeOut, {} });
        smp.push_back ({ "stretchMode", "Time mode", 0, 2, 0, (float) ch->stretchMode, { "Resample", "Stretch", "Stretch X" } });
        smp.push_back ({ "stretchTime", "Time mul", 0.25f, 4, 1, ch->stretchTime, {} });
        smp.push_back ({ "pitch", "Pitch (st)", -24, 24, 0, ch->pitch, {} });
        smp.push_back ({ "root", "Root / step note", 24, 96, 60, (float) ch->rootKey, {} });
    }
    ins.push_back ({ "volume", "Volume", 0, 1.25f, 0.8f, ch->volume, {} });
    ins.push_back ({ "pan", "Pan", -1, 1, 0, ch->pan, {} });
    if (sampler)
    {
        ins.push_back ({ "oneShot", "Mode", 0, 1, 1, ch->oneShot ? 1.0f : 0.0f, { "Gated (note length)", "One-shot" } });
        ins.push_back ({ "cutSelf", "Cut itself", 0, 1, 0, ch->cutItself ? 1.0f : 0.0f, { "Off", "On (808s never overlap)" } });
        ins.push_back ({ "cutGroup", "Cut group", 0, 8, 0, (float) ch->cutGroup, { "None", "1", "2", "3", "4", "5", "6", "7", "8" } });
        ins.push_back ({ "ampA", "Attack (s)", 0, 0.5f, 0, ch->ampAttack, {} });
        ins.push_back ({ "ampH", "Hold (s, 0=all)", 0, 4, 0, ch->ampHold, {} });
        ins.push_back ({ "ampR", "Release (s)", 0.002f, 2, 0.01f, ch->ampRelease, {} });
    }
    else
    {
        auto& sp = ch->synth;
        ins.push_back ({ "wave", "Wave", 0, 8, 0, (float) sp.wave, { "Saw", "Square", "Triangle", "Sine", "808", "Supersaw", "FM keys", "Morph", "Air" } });
        ins.push_back ({ "attack", "Attack", 0.001f, 2, 0.002f, sp.attack, {} });
        ins.push_back ({ "decay", "Decay", 0.01f, 4, 0.3f, sp.decay, {} });
        ins.push_back ({ "sustain", "Sustain", 0, 1, 0.7f, sp.sustain, {} });
        ins.push_back ({ "release", "Release", 0.01f, 4, 0.25f, sp.release, {} });
        ins.push_back ({ "cutoff", "Cutoff", 40, 20000, 12000, sp.cutoff, {} });
        ins.push_back ({ "res", "Resonance", 0, 1, 0.2f, sp.resonance, {} });
        ins.push_back ({ "detune", "Detune", 0, 1, 0, sp.detune, {} });
        ins.push_back ({ "unison", "Unison", 1, 7, 1, (float) sp.unison, {} });
        ins.push_back ({ "drive", "Drive", 0, 1, 0, sp.drive, {} });
        ins.push_back ({ "fmRatio", "FM ratio", 0.5f, 8, 1, sp.fmRatio, {} });
        ins.push_back ({ "fmIndex", "FM amount", 0, 8, 0, sp.fmIndex, {} });
        ins.push_back ({ "morph", "Morph", 0, 1, 0, sp.morph, {} });
        ins.push_back ({ "noise", "Air / noise", 0, 1, 0, sp.noise, {} });
        func.push_back ({ "lfoRate", "LFO rate (Hz)", 0, 12, 0, sp.lfoRate, {} });
        func.push_back ({ "vibrato", "Vibrato (st)", 0, 1, 0, sp.vibrato, {} });
        func.push_back ({ "pitch", "Pitch (st)", -24, 24, 0, ch->pitch, {} });
        func.push_back ({ "root", "Step note", 24, 96, 60, (float) ch->rootKey, {} });
    }
    func.push_back ({ "glide", "Portamento / glide (s)", 0, 1, 0, ch->glide, {} });
    func.push_back ({ "mono", "Mono / legato", 0, 1, 0, ch->mono ? 1.0f : 0.0f, { "Off", "On" } });

    auto setter = [&s, channelId] (const juce::String& id, float v) {
            s.live ([&] (Project&) {
                auto* c = s.channel (channelId);
                if (c == nullptr) return;
                auto& sp = c->synth;
                if (id == "volume") c->volume = v; else if (id == "pan") c->pan = v; else if (id == "pitch") c->pitch = v;
                else if (id == "glide") c->glide = v; else if (id == "mono") c->mono = v > 0.5f; else if (id == "root") c->rootKey = (int) v;
                else if (id == "wave") sp.wave = (int) v; else if (id == "attack") sp.attack = v; else if (id == "decay") sp.decay = v;
                else if (id == "sustain") sp.sustain = v; else if (id == "release") sp.release = v; else if (id == "cutoff") sp.cutoff = v;
                else if (id == "res") sp.resonance = v; else if (id == "detune") sp.detune = v; else if (id == "unison") sp.unison = (int) v;
                else if (id == "drive") sp.drive = v;
                else if (id == "fmRatio") sp.fmRatio = v; else if (id == "fmIndex") sp.fmIndex = v; else if (id == "morph") sp.morph = v;
                else if (id == "noise") sp.noise = v; else if (id == "lfoRate") sp.lfoRate = v; else if (id == "vibrato") sp.vibrato = v;
                else if (id == "oneShot") c->oneShot = v > 0.5f; else if (id == "cutSelf") c->cutItself = v > 0.5f;
                else if (id == "cutGroup") c->cutGroup = (int) v; else if (id == "ampA") c->ampAttack = v;
                else if (id == "ampH") c->ampHold = v; else if (id == "ampR") c->ampRelease = v;
                else if (id == "smpNorm") c->smpNormalize = v > 0.5f; else if (id == "smpBoost") c->smpBoostDb = v;
                else if (id == "smpRev") c->smpReverse = v > 0.5f; else if (id == "smpInv") c->smpInvert = v > 0.5f;
                else if (id == "smpDeclick") c->smpDeclick = v > 0.5f; else if (id == "smpTrim") c->smpTrim = v > 0.5f;
                else if (id == "smpFadeIn") c->smpFadeIn = v; else if (id == "smpFadeOut") c->smpFadeOut = v;
                else if (id == "stretchMode") c->stretchMode = (int) v; else if (id == "stretchTime") c->stretchTime = v;
            });
        };
    auto commit = [&s] { s.commit ("Channel settings"); };
    const juce::String kind = sampler ? "Sampler" : "Synth";
    auto* tabs = new ChannelTabs();
    if (sampler) tabs->add ("SMP", new ParamGrid (ch->name + "  |  Sample", smp, setter, commit, "Sampler SMP"));
    tabs->add ("INS", new ParamGrid (ch->name + "  |  " + kind, ins, setter, commit, kind));
    tabs->add ("FUNC", new ParamGrid (ch->name + "  |  Functions", func, setter, commit, kind + " FUNC"));
    if (sampler) tabs->add ("WAVE", new SampleWaveView (s, channelId));
    tabs->add ("SCOPE", createScopeView ([&s, channelId]() -> ScopeTap* {
        auto* c = s.channel (channelId);
        const int t = c != nullptr ? c->mixerTrack : 0;
        return t > 0 ? s.engine.trackScope (t) : &masterScope(); }, ch->name));
    tabs->finish();
    EditorWindow::windows().add (new EditorWindow (ch->name, tabs, key, [&s, channelId] { return s.channel (channelId) != nullptr; }));
}

void closeAllEditorWindows() { EditorWindow::windows().clear(); }

void openScopeWindow (Studio&)
{
    if (focusExisting ("scope:master")) return;
    auto* w = new EditorWindow ("Scope - Master", createScopeView ([]() -> ScopeTap* { return &masterScope(); }, "Master"),
                                "scope:master", [] { return true; });
    w->setResizable (true, false); w->setResizeLimits (420, 240, 3000, 2000);
    EditorWindow::windows().add (w);
}

void openTrackScopeWindow (Studio& s, int track)
{
    const juce::String key = "scope:track:" + juce::String (track);
    if (focusExisting (key)) return;
    const auto title = track == 0 ? juce::String ("Master") : s.p().mixer.size() > (size_t) track ? s.p().mixer[(size_t) track].name : "Track " + juce::String (track);
    auto* w = new EditorWindow ("Scope - " + title, createScopeView ([&s, track] { return s.engine.trackScope (track); }, title), key, [] { return true; });
    w->setResizable (true, false);
    w->setResizeLimits (420, 240, 3000, 2000);
    EditorWindow::windows().add (w);
}

void closeEditorsForPlugin (juce::AudioProcessor* p)
{
    auto& w = EditorWindow::windows();
    for (int i = w.size(); --i >= 0;)
        if (w[i]->proc == p) w.remove (i);
}
}

namespace hhs
{
static juce::File presetsRoot() { return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("HumHouse Studio").getChildFile ("Presets"); }

static void saveChainPreset (Studio& s, int track)
{
    auto* aw = new juce::AlertWindow ("Save FX chain preset", "Name this chain (every effect on the insert, in order)", juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("name", "My vocal chain");
    aw->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([&s, track, aw] (int r) {
        const auto name = juce::File::createLegalFileName (aw->getTextEditorContents ("name").trim());
        if (r != 1 || name.isEmpty() || track < 0 || track >= (int) s.p().mixer.size()) return;
        s.engine.storePluginStates();
        juce::XmlElement x ("HUMHOUSE_CHAIN");
        for (auto& f : s.p().mixer[(size_t) track].fx)
        {
            auto* fe = x.createNewChildElement ("FX");
            fe->setAttribute ("type", f.type); fe->setAttribute ("plugin", f.pluginId);
            fe->setAttribute ("bypass", f.bypass); fe->setAttribute ("mix", (double) f.mix);
            if (f.pluginState.getSize() > 0) fe->setAttribute ("state", f.pluginState.toBase64Encoding());
            for (auto& [id, v] : f.params) { auto* pe = fe->createNewChildElement ("P"); pe->setAttribute ("id", id); pe->setAttribute ("v", (double) v); }
        }
        auto dir = presetsRoot().getChildFile ("Chains");
        dir.createDirectory();
        x.writeTo (dir.getChildFile (name + ".hhchain"));
    }), true);
}

static void loadChainPreset (Studio& s, int track, const juce::File& file, bool replace)
{
    auto x = juce::XmlDocument::parse (file);
    if (x == nullptr) return;
    s.edit ("Load FX chain", [&] (Project& p) {
        if (track < 0 || track >= (int) p.mixer.size()) return;
        if (replace) p.mixer[(size_t) track].fx.clear();
        for (auto* fe : x->getChildWithTagNameIterator ("FX"))
        {
            auto& f = p.addFx (track, fe->getStringAttribute ("type"));
            f.pluginId = fe->getStringAttribute ("plugin");
            f.bypass = fe->getBoolAttribute ("bypass");
            f.mix = (float) fe->getDoubleAttribute ("mix", 1.0);
            if (fe->hasAttribute ("state")) f.pluginState.fromBase64Encoding (fe->getStringAttribute ("state"));
            for (auto* pe : fe->getChildWithTagNameIterator ("P")) f.params[pe->getStringAttribute ("id")] = (float) pe->getDoubleAttribute ("v");
        }
    });
}

juce::PopupMenu trackPresetMenu (Studio& s, int track)
{
    juce::PopupMenu m;
    if (track < 0) { m.addItem ("(no mixer insert)", false, false, [] {}); return m; }
    juce::PopupMenu chains;
    auto chainFiles = presetsRoot().getChildFile ("Chains").findChildFiles (juce::File::findFiles, false, "*.hhchain");
    chainFiles.sort();
    for (auto& f : chainFiles)
    {
        juce::PopupMenu one;
        one.addItem ("Replace this track's FX", [&s, track, f] { loadChainPreset (s, track, f, true); });
        one.addItem ("Add after current FX", [&s, track, f] { loadChainPreset (s, track, f, false); });
        chains.addSubMenu (f.getFileNameWithoutExtension(), one);
    }
    if (chainFiles.isEmpty()) chains.addItem ("(none saved yet)", false, false, [] {});
    m.addSubMenu ("FX chain presets", chains);
    m.addItem ("Save this track's FX chain as preset...", [&s, track] { saveChainPreset (s, track); });
    m.addSeparator();
    m.addSectionHeader ("Add effect from saved preset");
    bool any = false;
    for (auto& inf : builtinEffects())
    {
        auto files = presetsRoot().getChildFile (juce::File::createLegalFileName (inf.id)).findChildFiles (juce::File::findFiles, false, "*.hhpreset");
        if (files.isEmpty()) continue;
        files.sort();
        any = true;
        juce::PopupMenu sub;
        const juce::String type = inf.id;
        for (auto& f : files)
            sub.addItem (f.getFileNameWithoutExtension(), [&s, track, type, f] {
                auto x = juce::XmlDocument::parse (f);
                if (x == nullptr) return;
                int uid = 0;
                s.edit ("Add preset", [&] (Project& p) {
                    if (track >= (int) p.mixer.size()) return;
                    auto& slot = p.addFx (track, type);
                    uid = slot.uid;
                    for (auto* e : x->getChildWithTagNameIterator ("P")) slot.params[e->getStringAttribute ("id")] = (float) e->getDoubleAttribute ("v");
                });
                if (uid != 0) openEffectEditor (s, track, uid);
            });
        m.addSubMenu (inf.name, sub);
    }
    if (! any) m.addItem ("(save presets from any effect's PRESETS button)", false, false, [] {});
    return m;
}
}
