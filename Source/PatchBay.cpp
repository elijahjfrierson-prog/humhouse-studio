#include "UI.h"
#include <climits>

// Patch Bay: node view of the project. Step-sequencer nodes play sounds, sounds cable into their mixer insert's
// FX chain and output, LFO nodes cabled onto an FX knob write an automation clip. Everything edits the normal
// project (patterns, routing, automation), so the engine plays it exactly as before: no new audio-thread code.
namespace hhs
{
namespace
{
struct Lfo { int shape = 0; double perBar = 1.0; float lo = 0.0f, hi = 1.0f; juce::String name; };
const char* shapeNames[] { "Sine", "Triangle", "Saw", "Square", "Random" };
float lfoAt (const Lfo& l, double ph)
{
    ph -= std::floor (ph);
    float v = 0.0f;
    switch (l.shape)
    {
        case 0: v = 0.5f + 0.5f * (float) std::sin (ph * juce::MathConstants<double>::twoPi); break;
        case 1: v = (float) (ph < 0.5 ? ph * 2.0 : 2.0 - ph * 2.0); break;
        case 2: v = (float) ph; break;
        case 3: v = ph < 0.5 ? 1.0f : 0.0f; break;
        default: break;
    }
    return l.lo + (l.hi - l.lo) * v;
}
std::vector<Lfo>& lfos() { static std::vector<Lfo> v { { 0, 1.0, 0.0f, 1.0f, "LFO 1" } }; return v; }
// LFO nodes persist in the app settings ("patchBayLfos": shape,perBar,lo,hi,name;...)
void saveLfos (Studio& s)
{
    if (s.settings == nullptr) return;
    juce::StringArray rows;
    for (const auto& l : lfos())
        rows.add (juce::String (l.shape) + "," + juce::String (l.perBar) + "," + juce::String (l.lo) + "," + juce::String (l.hi) + "," + l.name.removeCharacters (",;"));
    s.settings->setValue ("patchBayLfos", rows.joinIntoString (";"));
    s.settings->saveIfNeeded();
}
void loadLfos (Studio& s)
{
    static bool done = false;
    if (done || s.settings == nullptr) return;
    done = true;
    std::vector<Lfo> v;
    for (const auto& row : juce::StringArray::fromTokens (s.settings->getValue ("patchBayLfos"), ";", ""))
    {
        const auto f = juce::StringArray::fromTokens (row, ",", "");
        if (f.size() < 5) continue;
        v.push_back ({ juce::jlimit (0, 4, f[0].getIntValue()), juce::jlimit (0.0625, 16.0, f[1].getDoubleValue()),
                       juce::jlimit (0.0f, 1.0f, f[2].getFloatValue()), juce::jlimit (0.0f, 1.0f, f[3].getFloatValue()), f[4] });
    }
    if (! v.empty()) lfos() = v;
}
std::map<juce::String, juce::Point<int>>& positions() { static std::map<juce::String, juce::Point<int>> m; return m; }

enum Kind { Seq, Sound, LfoNode, Fx, Out };
struct Node { Kind kind; int a = 0, b = 0; juce::String key, title; juce::Rectangle<int> r; juce::Colour c; };

class Canvas : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit Canvas (Studio& st) : s (st) { loadLfos (s); s.addChangeListener (this); setSize (1800, 1400); rebuild(); startTimerHz (15); }
    ~Canvas() override { s.removeChangeListener (this); }

    void rebuild()
    {
        nodes.clear();
        auto& p = s.p();
        auto place = [&] (Node n, int x, int y, int w, int h)
        {
            auto it = positions().find (n.key);
            const auto at = it != positions().end() ? it->second : juce::Point<int> (x, y);
            n.r = { at.x, at.y, w, h }; nodes.push_back (n);
        };
        for (int i = 0; i < (int) lfos().size(); ++i)
            place ({ LfoNode, i, 0, "L" + juce::String (i), lfos()[(size_t) i].name, {}, juce::Colour (0xffb98cff) }, 20, 20 + i * 130, 190, 112);
        int y = 20;
        std::map<int, int> trackY;
        for (auto& c : p.channels)
        {
            place ({ Seq, c.id, 0, "S" + juce::String (c.id), c.name + "  MIDI Rhythm", {}, c.colour }, 240, y, 300, 70);
            place ({ Sound, c.id, c.mixerTrack, "C" + juce::String (c.id), c.name, {}, c.colour }, 570, y, 170, 70);
            if (! trackY.count (c.mixerTrack)) trackY[c.mixerTrack] = y;
            y += 86;
        }
        for (auto& [tr, ty] : trackY)
        {
            if (tr < 0 || tr >= (int) p.mixer.size()) continue;
            int x = 770;
            for (auto& f : p.mixer[(size_t) tr].fx)
            {
                place ({ Fx, tr, f.uid, "F" + juce::String (f.uid), fxName (tr, f), {}, juce::Colour (0xff4fb3ff) }, x, ty, 170, 70);
                x += 190;
            }
            const auto& mt = p.mixer[(size_t) tr];
            place ({ Out, tr, 0, "O" + juce::String (tr), (mt.name.isNotEmpty() ? mt.name : "Insert " + juce::String (tr)) + "  Out", {}, col::accent }, x, ty, 150, 70);
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff141619));
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        for (int x = 0; x < getWidth(); x += 24) for (int yy = 0; yy < getHeight(); yy += 24) g.fillRect (x, yy, 2, 2);
        auto& p = s.p();
        // cables: sequencer -> sound -> FX chain -> out, LFO -> FX it automates
        for (auto& n : nodes)
        {
            if (n.kind == Seq) cable (g, n, find (Sound, n.a), juce::Colour (0xff6ad1ff), false);
            if (n.kind == Sound && n.b >= 0 && n.b < (int) p.mixer.size())
            {
                auto& fx = p.mixer[(size_t) n.b].fx;
                cable (g, n, fx.empty() ? find (Out, n.b) : find (Fx, n.b, fx.front().uid), juce::Colour (0xff6ad1ff), false);
            }
            if (n.kind == Fx)
            {
                auto& fx = p.mixer[(size_t) n.a].fx;
                for (size_t k = 0; k < fx.size(); ++k)
                    if (fx[k].uid == n.b) cable (g, n, k + 1 < fx.size() ? find (Fx, n.a, fx[k + 1].uid) : find (Out, n.a), juce::Colour (0xff6ad1ff), false);
            }
        }
        for (auto& c : p.clips)
            if (c.isAuto() && c.autoTarget.kind == AutoFxParam && c.autoTarget.name.startsWith ("LFO "))
                if (auto* from = find (LfoNode, c.autoTarget.name.fromFirstOccurrenceOf ("LFO ", false, false).getIntValue() - 1))
                    cable (g, *from, find (Fx, c.autoTarget.track, c.autoTarget.fxUid), juce::Colour (0xffd59cff), true);
        if (dragFrom != nullptr) wire (g, port (*dragFrom), dragTo.toFloat(), juce::Colours::white, true);
        for (auto& n : nodes) drawNode (g, n);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        moving = nullptr; dragFrom = nullptr;
        auto* n = at (e.getPosition());
        if (n == nullptr) return;
        if (port (*n).getDistanceFrom (e.position) < 12.0f && n->kind != Out) { dragFrom = n; dragTo = e.getPosition(); return; }
        if (e.mods.isPopupMenu()) { nodeMenu (*n); return; }
        if (n->kind == Seq && e.y > n->r.getY() + 30)
        {
            const int step = (e.x - n->r.getX() - 8) * 16 / juce::jmax (1, n->r.getWidth() - 16);
            if (step >= 0 && step < 16) toggle (n->a, step);
            return;
        }
        moving = n; grab = e.getPosition() - n->r.getPosition();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragFrom != nullptr) { dragTo = e.getPosition(); repaint(); return; }
        if (moving == nullptr) return;
        moving->r.setPosition (e.getPosition() - grab);
        positions()[moving->key] = moving->r.getPosition();
        repaint();
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragFrom == nullptr) return;
        auto* from = dragFrom; dragFrom = nullptr; repaint();
        auto* to = at (e.getPosition());
        if (to == nullptr || to == from) return;
        if (from->kind == Seq && to->kind == Sound) moveSequence (from->a, to->a);
        else if (from->kind == Sound && to->kind == Out) route (from->a, to->a);
        else if (from->kind == Sound && to->kind == Fx) route (from->a, to->a);
        else if (from->kind == LfoNode && to->kind == Fx) lfoToParam (from->a, to->a, to->b);
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (at (e.getPosition()) == nullptr) { lfos().push_back ({ 0, 1.0, 0.0f, 1.0f, "LFO " + juce::String ((int) lfos().size() + 1) }); saveLfos (s); rebuild(); }
    }

private:
    juce::String fxName (int tr, const FxSlot& f)
    {
        if (auto* pl = s.engine.fxPlugin (tr, f.uid)) return pl->getName();
        for (auto& e : builtinEffects()) if (f.type == e.id) return e.name;
        return f.type;
    }
    Node* find (Kind k, int a, int b = INT_MIN)
    {
        for (auto& n : nodes) if (n.kind == k && n.a == a && (b == INT_MIN || n.b == b)) return &n;
        return nullptr;
    }
    Node* at (juce::Point<int> pt) { for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) if (it->r.expanded (8, 0).contains (pt)) return &*it; return nullptr; }
    static juce::Point<float> port (const Node& n) { return { (float) n.r.getRight(), (float) n.r.getY() + 14.0f }; }
    static juce::Point<float> inPort (const Node& n) { return { (float) n.r.getX(), (float) n.r.getY() + 14.0f }; }
    static void wire (juce::Graphics& g, juce::Point<float> a, juce::Point<float> b, juce::Colour c, bool dashed)
    {
        juce::Path w; w.startNewSubPath (a);
        const float dx = juce::jmax (40.0f, std::abs (b.x - a.x) * 0.5f);
        w.cubicTo (a.x + dx, a.y, b.x - dx, b.y, b.x, b.y);
        g.setColour (c.withAlpha (0.85f));
        if (! dashed) { g.strokePath (w, juce::PathStrokeType (2.0f)); return; }
        juce::Path d; const float dash[] { 6.0f, 4.0f };
        juce::PathStrokeType (1.6f).createDashedStroke (d, w, dash, 2);
        g.fillPath (d);
    }
    static void cable (juce::Graphics& g, const Node& a, const Node* b, juce::Colour c, bool dashed) { if (b != nullptr) wire (g, port (a), inPort (*b), c, dashed); }

    void drawNode (juce::Graphics& g, const Node& n)
    {
        const auto r = n.r.toFloat();
        g.setColour (juce::Colour (0xff23262b)); g.fillRoundedRectangle (r, 6.0f);
        g.setColour (juce::Colours::white.withAlpha (0.12f)); g.drawRoundedRectangle (r, 6.0f, 1.0f);
        g.setColour (n.c); g.fillRoundedRectangle (r.withWidth (4.0f), 2.0f);
        g.setColour (col::text); g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        g.drawText (n.title, n.r.getX() + 10, n.r.getY() + 4, n.r.getWidth() - 20, 18, juce::Justification::centredLeft, true);
        g.setColour (juce::Colour (0xff6ad1ff));
        if (n.kind != LfoNode) g.fillEllipse (inPort (n).x - 5, inPort (n).y - 5, 10, 10);
        if (n.kind != Out) { g.setColour (n.kind == LfoNode ? juce::Colour (0xffd59cff) : juce::Colour (0xff6ad1ff)); g.fillEllipse (port (n).x - 5, port (n).y - 5, 10, 10); }
        g.setFont (juce::FontOptions (10.5f)); g.setColour (col::dim);
        auto& p = s.p();
        if (n.kind == Seq)
        {
            const int cur = juce::jlimit (0, juce::jmax (0, (int) p.patterns.size() - 1), p.currentPattern);
            const double beat = s.engine.positionBeats();
            const int play = s.engine.isPlaying() ? (int) (std::fmod (beat, 4.0) * 4.0) : -1;
            const float cw = (r.getWidth() - 16.0f) / 16.0f;
            for (int k = 0; k < 16; ++k)
            {
                const bool on = cur < (int) p.patterns.size() && p.patterns[(size_t) cur].stepOn (n.a, k);
                auto cell = juce::Rectangle<float> (r.getX() + 8 + k * cw, r.getY() + 34, cw - 2, 26);
                g.setColour (on ? n.c : (k / 4) % 2 ? juce::Colour (0xff30343a) : juce::Colour (0xff3a3f46));
                g.fillRoundedRectangle (cell, 3.0f);
                if (k == play) { g.setColour (juce::Colours::white); g.drawRoundedRectangle (cell, 3.0f, 1.5f); }
            }
            g.setColour (col::dim); g.drawText ("click steps / drag out to another sound", n.r.getX() + 10, n.r.getY() + 20, 260, 12, juce::Justification::centredLeft);
        }
        else if (n.kind == LfoNode && n.a < (int) lfos().size())
        {
            const auto& l = lfos()[(size_t) n.a];
            g.drawText (juce::String (shapeNames[l.shape]) + "  " + juce::String (l.perBar, 2) + " / bar  " + juce::String (juce::roundToInt (l.lo * 100)) + "-" + juce::String (juce::roundToInt (l.hi * 100)) + "%",
                        n.r.getX() + 10, n.r.getY() + 22, n.r.getWidth() - 20, 14, juce::Justification::centredLeft);
            juce::Path w; const auto box = r.reduced (10.0f).withTrimmedTop (32.0f);
            for (int i = 0; i <= 80; ++i)
            {
                const float x = box.getX() + box.getWidth() * i / 80.0f, yv = box.getBottom() - box.getHeight() * lfoAt (l, i / 40.0);
                if (i == 0) w.startNewSubPath (x, yv); else w.lineTo (x, yv);
            }
            g.setColour (n.c); g.strokePath (w, juce::PathStrokeType (1.6f));
            g.setColour (col::dim); g.drawText ("right-click: shape / rate / range", n.r.getX() + 10, n.r.getBottom() - 14, 170, 12, juce::Justification::centredLeft);
        }
        else if (n.kind == Sound) g.drawText ("SIG OUT  >  insert " + juce::String (n.b), n.r.getX() + 10, n.r.getY() + 30, 150, 14, juce::Justification::centredLeft);
        else if (n.kind == Fx) g.drawText ("drop an LFO here to modulate", n.r.getX() + 10, n.r.getY() + 30, 150, 14, juce::Justification::centredLeft);
        else if (n.kind == Out) g.drawText ("drop a sound here to route it", n.r.getX() + 10, n.r.getY() + 30, 140, 14, juce::Justification::centredLeft);
    }

    void toggle (int ch, int step)
    {
        s.edit ("Patch Bay step", [&] (Project& p) {
            if (p.patterns.empty()) return;
            auto& pt = p.patterns[(size_t) juce::jlimit (0, (int) p.patterns.size() - 1, p.currentPattern)];
            int key = 60;
            if (auto it = pt.notes.find (ch); it != pt.notes.end() && ! it->second.empty()) key = it->second.front().key;
            pt.toggleStep (ch, step, key);
        });
    }
    void moveSequence (int fromCh, int toCh)
    {
        s.edit ("Patch Bay: sequence to sound", [&] (Project& p) {
            if (p.patterns.empty()) return;
            auto& pt = p.patterns[(size_t) juce::jlimit (0, (int) p.patterns.size() - 1, p.currentPattern)];
            auto it = pt.notes.find (fromCh);
            if (it == pt.notes.end()) return;
            auto& dst = pt.notes[toCh];
            dst.insert (dst.end(), it->second.begin(), it->second.end());
            pt.notes.erase (fromCh);
        });
    }
    void route (int ch, int track)
    {
        s.edit ("Patch Bay: route sound", [&] (Project& p) {
            if (auto* c = p.findChannel (ch); c != nullptr && track > 0 && track < (int) p.mixer.size()) c->mixerTrack = track;
        });
    }
    void lfoToParam (int li, int track, int uid)
    {
        juce::PopupMenu m;
        std::vector<std::pair<juce::String, juce::String>> ps;
        if (auto* b = s.engine.builtinFx (track, uid)) for (auto& d : b->params()) ps.push_back ({ d.id, d.name });
        else if (auto* pl = s.engine.fxPlugin (track, uid))
            for (int k = 0; k < juce::jmin (128, pl->getParameters().size()); ++k) ps.push_back ({ "#" + juce::String (k), pl->getParameters()[k]->getName (32) });
        for (int i = 0; i < (int) ps.size(); ++i) m.addItem (i + 1, ps[(size_t) i].second);
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<Canvas> (this), ps, li, track, uid] (int r) {
            if (safe == nullptr || r <= 0 || li >= (int) lfos().size()) return;
            auto& st = safe->s;
            AutoTarget t; t.kind = AutoFxParam; t.track = track; t.fxUid = uid; t.param = ps[(size_t) r - 1].first;
            t.name = "LFO " + juce::String (li + 1) + " > " + ps[(size_t) r - 1].second;
            createAutomationClip (st, t, (int) st.p().lanes.size() - 1);
            const auto l = lfos()[(size_t) li];
            st.edit ("Patch Bay: LFO shape", [&] (Project& p) {
                Clip* c = nullptr;
                for (auto& cl : p.clips) if (cl.isAuto() && cl.autoTarget.name == t.name && (c == nullptr || cl.uid > c->uid)) c = &cl;
                if (c == nullptr) return;
                const double period = p.beatsPerBar / juce::jmax (0.01, l.perBar);
                const double stepB = juce::jmax (0.125, c->length / 4000.0);
                c->autoPts.clear();
                for (double b = 0.0; b <= c->length + 1.0e-9; b += stepB)
                {
                    float v = l.shape == 4 ? l.lo + (l.hi - l.lo) * juce::Random ((juce::int64) std::floor (b / period * 4.0) + 7).nextFloat()
                                           : lfoAt (l, b / period);
                    c->autoPts.push_back ({ b, juce::jlimit (0.0f, 1.0f, v) });
                }
            });
        });
    }
    void nodeMenu (Node& n)
    {
        if (n.kind != LfoNode || n.a >= (int) lfos().size()) return;
        juce::PopupMenu m, sh, rt, rg;
        const auto& cur = lfos()[(size_t) n.a];
        for (int i = 0; i < 5; ++i) sh.addItem (10 + i, shapeNames[i], true, cur.shape == i);
        const double rates[] { 0.25, 0.5, 1, 2, 4, 8, 16 };
        for (int i = 0; i < 7; ++i) rt.addItem (20 + i, juce::String (rates[i], 2) + " per bar", true, std::abs (cur.perBar - rates[i]) < 1.0e-9);
        rg.addItem (30, "Full 0-100%"); rg.addItem (31, "Upper 50-100%"); rg.addItem (32, "Lower 0-50%"); rg.addItem (33, "Subtle 40-60%");
        m.addSubMenu ("Shape", sh); m.addSubMenu ("Rate", rt); m.addSubMenu ("Range", rg);
        m.addItem (40, "Remove LFO", lfos().size() > 1);
        const int li = n.a;
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<Canvas> (this), li, rates] (int r) {
            if (safe == nullptr || r <= 0 || li >= (int) lfos().size()) return;
            auto& l = lfos()[(size_t) li];
            if (r >= 10 && r < 15) l.shape = r - 10;
            else if (r >= 20 && r < 27) l.perBar = rates[r - 20];
            else if (r >= 30 && r < 34) { const float lo[] { 0, 0.5f, 0, 0.4f }, hi[] { 1, 1, 0.5f, 0.6f }; l.lo = lo[r - 30]; l.hi = hi[r - 30]; }
            else if (r == 40) lfos().erase (lfos().begin() + li);
            saveLfos (safe->s);
            safe->rebuild();
        });
    }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { moving = nullptr; dragFrom = nullptr; rebuild(); }
    void timerCallback() override { if (isShowing() && s.engine.isPlaying()) repaint(); }

    Studio& s;
    std::vector<Node> nodes;
    Node* moving = nullptr; Node* dragFrom = nullptr;
    juce::Point<int> grab, dragTo;
};

struct PatchBayWindow : public juce::DocumentWindow
{
    explicit PatchBayWindow (Studio& s) : juce::DocumentWindow ("Patch Bay", col::panel, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (! phoneUI); if (phoneUI) setTitleBarHeight (44);
        auto* vp = new juce::Viewport();
        vp->setViewedComponent (new Canvas (s), true);
        vp->setSize (phoneUI ? 380 : 1280, phoneUI ? 700 : 800);
        setContentOwned (vp, true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};
std::unique_ptr<PatchBayWindow>& patchWindow() { static std::unique_ptr<PatchBayWindow> w; return w; }
}

void showPatchBay (Studio& s)
{
    auto& w = patchWindow();
    if (w == nullptr) w = std::make_unique<PatchBayWindow> (s);
    w->setVisible (true); w->toFront (true);
}
void closePatchBay() { patchWindow().reset(); }
}
