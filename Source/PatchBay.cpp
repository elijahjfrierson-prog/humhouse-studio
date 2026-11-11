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
// Bays: several Patch Bays (intro, chorus...), each a set of channels (empty = all) routed to a Playlist track, a mixer insert or both
struct Bay { juce::String name; std::vector<int> chans; int route = 2; int insert = 0; };
std::vector<Bay>& bays() { static std::vector<Bay> v { { "Bay 1", {}, 2, 0 } }; return v; }
int& curBay() { static int i = 0; return i; }
Bay& bay() { curBay() = juce::jlimit (0, (int) bays().size() - 1, curBay()); return bays()[(size_t) curBay()]; }
void saveBays (Studio& s)
{
    if (s.settings == nullptr) return;
    juce::StringArray rows;
    for (const auto& b : bays())
    {
        juce::StringArray ch; for (int id : b.chans) ch.add (juce::String (id));
        rows.add (b.name.removeCharacters ("|;") + "|" + juce::String (b.route) + "|" + juce::String (b.insert) + "|" + ch.joinIntoString (","));
    }
    s.settings->setValue ("patchBays", rows.joinIntoString (";")); s.settings->saveIfNeeded();
}
void loadBays (Studio& s)
{
    static bool done = false;
    if (done || s.settings == nullptr) return;
    done = true;
    std::vector<Bay> v;
    for (const auto& row : juce::StringArray::fromTokens (s.settings->getValue ("patchBays"), ";", ""))
    {
        const auto f = juce::StringArray::fromTokens (row, "|", "");
        if (f.size() < 3) continue;
        Bay b { f[0], {}, juce::jlimit (0, 2, f[1].getIntValue()), juce::jmax (0, f[2].getIntValue()) };
        for (const auto& id : juce::StringArray::fromTokens (f[3], ",", "")) b.chans.push_back (id.getIntValue());
        v.push_back (b);
    }
    if (! v.empty()) bays() = v;
}
bool inBay (int ch) { const auto& b = bay(); return b.chans.empty() || std::find (b.chans.begin(), b.chans.end(), ch) != b.chans.end(); }

std::map<juce::String, juce::Point<int>>& positions() { static std::map<juce::String, juce::Point<int>> m; return m; }

enum Kind { Seq, Sound, LfoNode, Fx, Out };
struct Node { Kind kind; int a = 0, b = 0; juce::String key, title; juce::Rectangle<int> r; juce::Colour c; };

class Canvas : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit Canvas (Studio& st) : s (st) { loadLfos (s); loadBays (s); setWantsKeyboardFocus (true); s.addChangeListener (this); setSize (1800, 1400); rebuild(); startTimerHz (15); }
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
            if (! inBay (c.id)) continue;
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
        if (n == nullptr) { if (e.mods.isPopupMenu()) canvasMenu(); return; }
        grabKeyboardFocus();
        if (n->kind == Seq || n->kind == Sound) lastChan = n->a;
        if (port (*n).getDistanceFrom (e.position) < 12.0f && n->kind != Out) { dragFrom = n; dragTo = e.getPosition(); return; }
        if (e.mods.isPopupMenu()) { nodeMenu (*n); return; }
        if (n->kind == Seq && e.mods.isAltDown() && e.y > n->r.getY() + 30)
        {
            const int step = (e.x - n->r.getX() - 8) * 16 / juce::jmax (1, n->r.getWidth() - 16);
            if (step >= 0 && step < 16) hatRoll (n->a, step, 8);
            return;
        }
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
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k.getModifiers().isAltDown() && (k.getKeyCode() == 'S' || k.getKeyCode() == 's') && lastChan >= 0) { hatMenu (lastChan, -1); return true; }
        return false;
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (phoneUI) return;   // phone: double-tap is right-click
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
            g.setColour (col::dim); g.drawText ("click steps, Alt-click: hat roll, right-click: Piano Roll", n.r.getX() + 10, n.r.getY() + 20, 260, 12, juce::Justification::centredLeft);
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
        else if (n.kind == Fx) g.drawText ("right-click: automate / gate / LFO", n.r.getX() + 10, n.r.getY() + 30, 150, 14, juce::Justification::centredLeft);
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
        if (n.kind == Seq || n.kind == Sound) { channelMenu (n.a); return; }
        if (n.kind == Fx) { fxMenu (n.a, n.b); return; }
        if (n.kind == Out) { canvasMenu(); return; }
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

public:
    // ---- right-click (phone: double-tap) menus
    void canvasMenu()
    {
        juce::PopupMenu m, inst, fx, vst, inv, rt, ins, bm;
        inst.addItem (1, "Synth"); inst.addItem (2, "808"); inst.addItem (3, "Sampler (load a sound)");
        int k = 0;
        for (auto& d : s.engine.knownPlugins.getTypes()) { if (d.isInstrument) inv.addItem (1000 + k, d.name); else vst.addItem (3000 + k, d.name); ++k; }
        inst.addSubMenu ("VST instrument", inv);
        int fi = 0; for (auto& e : builtinEffects()) fx.addItem (2000 + fi++, e.name);
        m.addSubMenu ("Add instrument", inst);
        m.addSubMenu ("Add FX (built-in)", fx); m.addSubMenu ("Add VST effect", vst);
        m.addItem (4, "Add LFO");
        m.addSeparator();
        auto& b = bay();
        rt.addItem (10, "Track (Playlist)", true, b.route == 0); rt.addItem (11, "Mixer insert", true, b.route == 1); rt.addItem (12, "Both", true, b.route == 2);
        auto& p = s.p();
        for (int i = 1; i < (int) p.mixer.size(); ++i) ins.addItem (100 + i, p.mixer[(size_t) i].name.isNotEmpty() ? p.mixer[(size_t) i].name : "Insert " + juce::String (i), true, b.insert == i);
        ins.addItem (99, "New insert");
        rt.addSubMenu ("Mixer insert", ins);
        m.addSubMenu ("Route " + b.name + " to", rt);
        m.addItem (13, "Send " + b.name + " now (" + juce::String (b.route == 0 ? "track" : b.route == 1 ? "mixer" : "track + mixer") + ")");
        m.addItem (14, p.songMode ? "Sync: SONG (switch to pattern)" : "Sync: PATTERN (switch to song)");
        m.addItem (15, s.engine.isPlaying() ? "Stop" : "Play");
        m.addSeparator();
        for (int i = 0; i < (int) bays().size(); ++i) bm.addItem (200 + i, bays()[(size_t) i].name, true, i == curBay());
        m.addSubMenu ("Go to bay", bm);
        m.addItem (5, "New Patch Bay"); m.addItem (6, "Rename " + b.name); m.addItem (7, "Duplicate " + b.name); m.addItem (8, "Delete " + b.name, bays().size() > 1);
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<Canvas> (this)] (int r) { if (safe != nullptr) safe->canvasResult (r); });
    }
    void canvasResult (int r)
    {
        if (r <= 0) return;
        auto& b = bay();
        auto addCh = [&] (Channel c) {
            int id = -1;
            s.edit ("Patch Bay: add instrument", [&] (Project& p) { if (b.route != 0 && b.insert > 0 && b.insert < (int) p.mixer.size()) c.mixerTrack = b.insert; else c.mixerTrack = -1; id = p.addChannel (c); });
            if (! b.chans.empty() && id >= 0) b.chans.push_back (id);
            s.selectedChannel = id; saveBays (s); rebuild();
        };
        if (r == 1) { Channel c; c.type = ChannelType::Synth; c.name = "Synth"; c.keyTrack = true; addCh (c); }
        else if (r == 2) { Channel c; c.type = ChannelType::Synth; c.name = "808"; c.synth.wave = 4; c.glide = 0.08f; c.mono = true; c.rootKey = 36; c.colour = juce::Colour (0xffff5d8a); addCh (c); }
        else if (r == 3)
        {
            chooser = std::make_unique<juce::FileChooser> ("Load a sound", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe = juce::Component::SafePointer<Canvas> (this)] (const juce::FileChooser& fc) {
                if (safe == nullptr || ! fc.getResult().existsAsFile()) return;
                Channel c; c.type = ChannelType::Sampler; c.sampleFile = fc.getResult(); c.name = fc.getResult().getFileNameWithoutExtension(); safe->canvasResult (0); safe->addChannelPublic (c);
            });
        }
        else if (r == 4) { lfos().push_back ({ 0, 1.0, 0.0f, 1.0f, "LFO " + juce::String ((int) lfos().size() + 1) }); saveLfos (s); rebuild(); }
        else if (r == 5) { bays().push_back ({ "Bay " + juce::String ((int) bays().size() + 1), { -1 }, 2, 0 }); curBay() = (int) bays().size() - 1; saveBays (s); rebuild(); if (onBays) onBays(); }
        else if (r == 6) rename();
        else if (r == 7) { auto c = b; c.name << " copy"; bays().push_back (c); curBay() = (int) bays().size() - 1; saveBays (s); rebuild(); if (onBays) onBays(); }
        else if (r == 8 && bays().size() > 1) { bays().erase (bays().begin() + curBay()); curBay() = 0; saveBays (s); rebuild(); if (onBays) onBays(); }
        else if (r >= 10 && r <= 12) { b.route = r - 10; saveBays (s); }
        else if (r == 13) sendBay();
        else if (r == 14) s.edit ("Pattern/Song", [] (Project& p) { p.songMode = ! p.songMode; });
        else if (r == 15) s.engine.togglePlay();
        else if (r == 99) { s.edit ("Patch Bay: new insert", [&] (Project& p) { b.insert = p.addMixerTrack (b.name); }); saveBays (s); }
        else if (r >= 100 && r < 200) { b.insert = r - 100; saveBays (s); }
        else if (r >= 200 && r < 300) { curBay() = r - 200; rebuild(); if (onBays) onBays(); }
        else if (r >= 1000 && r < 4000)
        {
            const auto& types = s.engine.knownPlugins.getTypes();
            const int i = r >= 3000 ? r - 3000 : r - 1000;
            if (i >= types.size()) return;
            const auto d = types[i];
            if (r < 3000) { Channel c; c.type = ChannelType::Plugin; c.pluginId = d.createIdentifierString(); c.name = d.name; addCh (c); }
            else addFxTo (fxTrack(), "plugin", d.createIdentifierString());
        }
        else if (r >= 2000 && r < 3000)
        {
            const auto& fx = builtinEffects();
            if (r - 2000 < (int) fx.size()) addFxTo (fxTrack(), fx[(size_t) (r - 2000)].id, {});
        }
    }
    void addChannelPublic (Channel c)
    {
        auto& b = bay(); int id = -1;
        s.edit ("Patch Bay: add sound", [&] (Project& p) { c.mixerTrack = (b.route != 0 && b.insert > 0 && b.insert < (int) p.mixer.size()) ? b.insert : -1; id = p.addChannel (c); });
        if (! b.chans.empty() && id >= 0) b.chans.push_back (id);
        s.selectedChannel = id; saveBays (s); rebuild();
    }
    std::function<void()> onBays;
    void rename()
    {
        auto* w = new juce::AlertWindow ("Rename bay", {}, juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("n", bay().name); w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey)); w->addButton ("Cancel", 0);
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<Canvas> (this), w] (int r) {
            if (safe != nullptr && r == 1 && w->getTextEditorContents ("n").trim().isNotEmpty()) { bay().name = w->getTextEditorContents ("n").trim(); saveBays (safe->s); if (safe->onBays) safe->onBays(); }
        }), true);
    }
private:
    int fxTrack()
    {
        auto& p = s.p(); auto& b = bay();
        if (b.insert > 0 && b.insert < (int) p.mixer.size()) return b.insert;
        if (auto* c = p.findChannel (lastChan)) if (c->mixerTrack > 0) return c->mixerTrack;
        for (auto& c : p.channels) if (inBay (c.id) && c.mixerTrack > 0) return c.mixerTrack;
        return 0;
    }
    void addFxTo (int track, const juce::String& type, const juce::String& pluginId)
    {
        s.edit ("Patch Bay: add FX", [&] (Project& p) { if (track >= 0 && track < (int) p.mixer.size()) { auto& f = p.addFx (track, type); f.pluginId = pluginId; } });
    }
    // send the bay: mixer = its channels go to the bay's insert; track = its pattern lands on a new Playlist track at the playhead
    void sendBay()
    {
        auto& b = bay();
        const double play = s.engine.positionBeats();
        s.edit ("Patch Bay: send " + b.name, [&] (Project& p) {
            if (b.route != 0)
            {
                if (b.insert <= 0 || b.insert >= (int) p.mixer.size()) b.insert = p.addMixerTrack (b.name);
                for (auto& c : p.channels) if (inBay (c.id)) c.mixerTrack = b.insert;
            }
            if (b.route != 1 && ! p.patterns.empty())
            {
                const int pi = juce::jlimit (0, (int) p.patterns.size() - 1, p.currentPattern);
                Lane l; l.name = b.name; l.mixerTrack = b.route == 2 && b.insert > 0 ? b.insert : 0; p.lanes.push_back (l);
                Clip c; c.uid = p.newUid(); c.lane = (int) p.lanes.size() - 1; c.patternIndex = pi;
                c.start = std::floor (play / p.beatsPerBar) * p.beatsPerBar; c.length = p.patterns[(size_t) pi].lengthBeats();
                p.clips.push_back (c);
            }
        });
        saveBays (s);
    }
    void channelMenu (int ch)
    {
        auto* c = s.p().findChannel (ch);
        if (c == nullptr) return;
        lastChan = ch;
        juce::PopupMenu m, vm, un, mv;
        m.addItem (1, "Open in Piano Roll");
        m.addItem (2, "Hi-hat roll / stutter (Alt+S)...");
        vm.addItem (10, "Poly", true, ! c->mono); vm.addItem (11, "Mono", true, c->mono && c->glide <= 0.0f); vm.addItem (12, "Legato (glide)", true, c->mono && c->glide > 0.0f);
        m.addSubMenu ("Voice mode", vm);
        if (c->type == ChannelType::Synth) { for (int u : { 1, 2, 3, 4, 5, 7 }) un.addItem (20 + u, u == 1 ? "Off" : juce::String (u) + " voices", true, c->synth.unison == u); m.addSubMenu ("Unison", un); }
        for (int i = 0; i < (int) bays().size(); ++i) mv.addItem (100 + i, bays()[(size_t) i].name, i != curBay());
        m.addSubMenu ("Move to bay", mv);
        m.addItem (3, "Remove from this bay", ! bay().chans.empty());
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<Canvas> (this), ch] (int r) {
            if (safe == nullptr || r <= 0) return;
            auto& s = safe->s;
            if (r == 1) { s.selectedChannel = ch; if (openPianoMain) openPianoMain (ch); }
            else if (r == 2) safe->hatMenu (ch, -1);
            else if (r >= 10 && r <= 12) s.edit ("Voice mode", [&] (Project& p) { if (auto* c = p.findChannel (ch)) { c->mono = r != 10; c->glide = r == 12 ? juce::jmax (0.06f, c->glide) : r == 11 ? 0.0f : c->glide; } });
            else if (r > 20 && r < 30) s.edit ("Unison", [&] (Project& p) { if (auto* c = p.findChannel (ch)) c->synth.unison = r - 20; });
            else if (r >= 100)
            {
                auto& from = bay().chans; from.erase (std::remove (from.begin(), from.end(), ch), from.end());
                auto& to = bays()[(size_t) (r - 100)].chans; to.erase (std::remove (to.begin(), to.end(), -1), to.end()); to.push_back (ch);
                saveBays (s); safe->rebuild();
            }
            else if (r == 3) { auto& v = bay().chans; v.erase (std::remove (v.begin(), v.end(), ch), v.end()); if (v.empty()) v.push_back (-1); saveBays (s); safe->rebuild(); }
        });
    }
    // Alt+S: hi-hat roll / stutter on a step (or the whole last beat): rate 1/16 .. 1/64 or triplets, with a velocity ramp
    void hatMenu (int ch, int step)
    {
        juce::PopupMenu m;
        const char* names[] { "Roll 1/32 (x2)", "Roll 1/48 triplet (x3)", "Roll 1/64 (x4)", "Stutter 1/32 last beat", "Stutter 1/64 last beat", "Clear rolls" };
        for (int i = 0; i < 6; ++i) m.addItem (i + 1, names[i]);
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<Canvas> (this), ch, step] (int r) {
            if (safe == nullptr || r <= 0) return;
            if (r <= 3) { if (step >= 0) safe->hatRoll (ch, step, r + 1); else for (int k = 12; k < 16; ++k) safe->hatRoll (ch, k, r + 1); }
            else if (r <= 5) for (int k = 12; k < 16; ++k) safe->hatRoll (ch, k, r == 4 ? 2 : 4);
            else safe->s.edit ("Clear rolls", [&] (Project& p) {
                if (p.patterns.empty()) return;
                auto& n = p.patterns[(size_t) juce::jlimit (0, (int) p.patterns.size() - 1, p.currentPattern)].notes[ch];
                n.erase (std::remove_if (n.begin(), n.end(), [] (const Note& x) { return x.length < 0.2 && std::fmod (x.start, 0.25) > 1.0e-6; }), n.end());
            });
        });
    }
    void hatRoll (int ch, int step, int div)
    {
        s.edit ("Hi-hat roll", [&] (Project& p) {
            if (p.patterns.empty()) return;
            auto& n = p.patterns[(size_t) juce::jlimit (0, (int) p.patterns.size() - 1, p.currentPattern)].notes[ch];
            const double t0 = step * 0.25, d = 0.25 / div;
            int key = 60; float vel = 0.8f;
            for (auto& x : n) if (x.start >= t0 - 1.0e-6 && x.start < t0 + 0.25 - 1.0e-6) { key = x.key; vel = x.velocity; }
            if (auto* c = p.findChannel (ch); c != nullptr && n.empty()) key = c->rootKey;
            n.erase (std::remove_if (n.begin(), n.end(), [&] (const Note& x) { return x.start >= t0 - 1.0e-6 && x.start < t0 + 0.25 - 1.0e-6; }), n.end());
            for (int i = 0; i < div; ++i) { Note x; x.key = key; x.start = t0 + i * d; x.length = d * 0.9; x.velocity = juce::jlimit (0.2f, 1.0f, vel * (0.7f + 0.3f * (float) (i + 1) / div)); n.push_back (x); }
            std::sort (n.begin(), n.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
        });
    }
    std::vector<std::pair<juce::String, juce::String>> fxParams (int track, int uid)
    {
        std::vector<std::pair<juce::String, juce::String>> ps;
        if (auto* b = s.engine.builtinFx (track, uid)) for (auto& d : b->params()) ps.push_back ({ d.id, d.name });
        else if (auto* pl = s.engine.fxPlugin (track, uid))
            for (int k = 0; k < juce::jmin (128, pl->getParameters().size()); ++k) ps.push_back ({ "#" + juce::String (k), pl->getParameters()[k]->getName (32) });
        return ps;
    }
    // FX right-click: automate a knob (starts at 1, pull it down), glitch-gate presets, LFO, remove
    void fxMenu (int track, int uid)
    {
        const auto ps = fxParams (track, uid);
        juce::PopupMenu m, au, gate[4], lf;
        const char* gates[] { "Glitch gate 1/4", "Glitch gate 1/8", "Stutter gate 1/16", "Weird gate (1/8 + triplet)" };
        for (int i = 0; i < (int) ps.size(); ++i)
        {
            au.addItem (1000 + i, ps[(size_t) i].second);
            for (int g = 0; g < 4; ++g) gate[g].addItem (2000 + g * 200 + i, ps[(size_t) i].second);
        }
        m.addSubMenu ("Automate", au);
        for (int g = 0; g < 4; ++g) m.addSubMenu (gates[g], gate[g]);
        for (int i = 0; i < (int) lfos().size(); ++i) lf.addItem (100 + i, lfos()[(size_t) i].name);
        m.addSubMenu ("Modulate with LFO", lf);
        m.addSeparator(); m.addItem (2, "Remove FX");
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<Canvas> (this), ps, track, uid] (int r) {
            if (safe == nullptr || r <= 0) return;
            auto& s = safe->s;
            if (r == 2) { s.edit ("Remove FX", [&] (Project& p) { if (track >= 0 && track < (int) p.mixer.size()) { auto& fx = p.mixer[(size_t) track].fx; fx.erase (std::remove_if (fx.begin(), fx.end(), [&] (const FxSlot& f) { return f.uid == uid; }), fx.end()); } }); return; }
            if (r >= 100 && r < 200) { safe->lfoToParam (r - 100, track, uid); return; }
            if (r >= 1000 && r < 1000 + (int) ps.size())
            {
                AutoTarget t; t.kind = AutoFxParam; t.track = track; t.fxUid = uid; t.param = ps[(size_t) r - 1000].first; t.name = ps[(size_t) r - 1000].second;
                createAutomationClip (s, t, (int) s.p().lanes.size() - 1);
                return;
            }
            if (r >= 2000 && r < 2800)
            {
                const int g = (r - 2000) / 200, i = (r - 2000) % 200;
                if (i >= (int) ps.size()) return;
                AutoTarget t; t.kind = AutoFxParam; t.track = track; t.fxUid = uid; t.param = ps[(size_t) i].first;
                t.name = juce::String (g == 0 ? "Gate 1/4" : g == 1 ? "Gate 1/8" : g == 2 ? "Gate 1/16" : "Weird gate") + " > " + ps[(size_t) i].second;
                createAutomationClip (s, t, (int) s.p().lanes.size() - 1);
                s.edit ("Glitch gate", [&] (Project& p) {
                    Clip* c = nullptr;
                    for (auto& cl : p.clips) if (cl.isAuto() && cl.autoTarget.name == t.name && (c == nullptr || cl.uid > c->uid)) c = &cl;
                    if (c == nullptr) return;
                    c->autoPts.clear();
                    const double steps[] { 1.0, 0.5, 0.25, 0.5 };
                    const double st = steps[g];
                    int k = 0;
                    for (double b = 0.0; b < c->length - 1.0e-9; b += (g == 3 && (k % 4 == 3)) ? st * 2.0 / 3.0 : st, ++k)
                    {
                        const float v = (k % 2 == 0) ? 1.0f : (g == 3 && k % 3 == 1 ? 0.35f : 0.0f);
                        c->autoPts.push_back ({ b, v }); c->autoPts.push_back ({ b + st * 0.92, v });
                    }
                });
            }
        });
    }
    int lastChan = -1;
    std::unique_ptr<juce::FileChooser> chooser;
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
// PATCH tab: bay picker + canvas; edits apply live while the pattern / song plays
struct PatchBayView : public juce::Component
{
    explicit PatchBayView (Studio& st) : s (st)
    {
        canvas = new Canvas (s);
        vp.setViewedComponent (canvas, true);
        addAndMakeVisible (vp);
        for (auto* b : { &addBay, &menu, &send, &play }) addAndMakeVisible (b);
        addAndMakeVisible (pick);
        pick.onChange = [this] { if (pick.getSelectedItemIndex() >= 0) { curBay() = pick.getSelectedItemIndex(); canvas->rebuild(); } };
        addBay.onClick = [this] { canvas->canvasResult (5); };
        menu.onClick = [this] { canvas->canvasMenu(); };
        send.onClick = [this] { canvas->canvasResult (13); };
        play.onClick = [this] { s.engine.togglePlay(); };
        canvas->onBays = [this] { refresh(); };
        addBay.setTooltip ("New Patch Bay (intro, chorus...): right-click / double-tap the canvas for everything");
        send.setTooltip ("Send this bay to its Playlist track, mixer insert or both (right-click > Route)");
        refresh();
    }
    void refresh()
    {
        pick.clear (juce::dontSendNotification);
        for (int i = 0; i < (int) bays().size(); ++i) pick.addItem (bays()[(size_t) i].name, i + 1);
        pick.setSelectedItemIndex (juce::jlimit (0, (int) bays().size() - 1, curBay()), juce::dontSendNotification);
    }
    void paint (juce::Graphics& g) override { g.fillAll (col::panel); }
    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop (phoneUI ? 44 : 32).reduced (4);
        pick.setBounds (bar.removeFromLeft (160)); bar.removeFromLeft (4);
        for (auto* b : { &addBay, &send, &play, &menu }) { b->setBounds (bar.removeFromLeft (phoneUI ? 70 : 84)); bar.removeFromLeft (4); }
        vp.setBounds (r);
    }
    Studio& s;
    Canvas* canvas = nullptr;
    juce::Viewport vp;
    juce::ComboBox pick;
    juce::TextButton addBay { "+ BAY" }, menu { "ADD..." }, send { "SEND" }, play { "PLAY" };
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
std::unique_ptr<juce::Component> createPatchBayView (Studio& s) { return std::make_unique<PatchBayView> (s); }
}
