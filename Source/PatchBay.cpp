#include "UI.h"
#include <climits>
#include <set>

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
struct Bay { juce::String name; std::vector<int> chans; int route = 2; int insert = 0; std::vector<int> stacks; int mode = 0; };   // stacks: FX stack node inserts feeding the bay insert; mode 0 MANUAL, 1 AUTO
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
        juce::StringArray st; for (int t : b.stacks) st.add (juce::String (t));
        rows.add (b.name.removeCharacters ("|;") + "|" + juce::String (b.route) + "|" + juce::String (b.insert) + "|" + ch.joinIntoString (",") + "|" + st.joinIntoString (",") + "|" + juce::String (b.mode));
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
        for (const auto& id : juce::StringArray::fromTokens (f[4], ",", "")) if (id.getIntValue() > 0) b.stacks.push_back (id.getIntValue());
        b.mode = juce::jlimit (0, 1, f[5].getIntValue());
        v.push_back (b);
    }
    if (! v.empty()) bays() = v;
}
// a bay's insert (node 0) or FX stack node k (1..), node -1 = a new stack node; stack nodes feed the bay insert, not the Master
int bayInsert (Project& p, int i, int node)
{
    if (i < 0 || i >= (int) bays().size()) return -1;
    auto& b = bays()[(size_t) i];
    const auto ok = [&] (int t) { return t > 0 && t < (int) p.mixer.size(); };
    if (! ok (b.insert)) b.insert = p.addMixerTrack (b.name);
    if (b.route == 0) b.route = 2;
    b.stacks.erase (std::remove_if (b.stacks.begin(), b.stacks.end(), [&] (int t) { return ! ok (t) || t == b.insert; }), b.stacks.end());
    if (node == 0) return b.insert;
    if (node > 0 && node <= (int) b.stacks.size()) return b.stacks[(size_t) node - 1];
    const int t = p.addMixerTrack (b.name + " FX " + juce::String ((int) b.stacks.size() + 1));
    if (! ok (t)) return b.insert;
    auto& mt = p.mixer[(size_t) t];
    mt.routesToMaster = false; mt.sends.clear(); mt.sends.push_back ({ b.insert, 1.0f });
    b.stacks.push_back (t);
    return t;
}
void ensurePattern (Project& p)
{
    if (! p.patterns.empty()) return;
    Pattern np; np.name = "Pattern 1"; p.patterns.push_back (np); p.currentPattern = 0;
}
bool inBay (int ch) { const auto& b = bay(); return b.chans.empty() || std::find (b.chans.begin(), b.chans.end(), ch) != b.chans.end(); }

std::map<juce::String, juce::Point<int>>& positions() { static std::map<juce::String, juce::Point<int>> m; return m; }

enum Kind { Seq, Sound, LfoNode, Fx, Out, AutoNode };
// extra in/out dots per node (right-click a dot to add more) and free cables between any two dots ("from>out>to>in;...")
std::map<juce::String, std::pair<int, int>>& extraDots() { static std::map<juce::String, std::pair<int, int>> m; return m; }
struct Link { juce::String from, to; int out = 0, in = 0; };
std::vector<Link>& links() { static std::vector<Link> v; return v; }
void saveWires (Studio& s)
{
    if (s.settings == nullptr) return;
    juce::StringArray d, l;
    for (auto& [k, v] : extraDots()) if (v.first > 0 || v.second > 0) d.add (k + "," + juce::String (v.first) + "," + juce::String (v.second));
    for (auto& x : links()) l.add (x.from + ">" + juce::String (x.out) + ">" + x.to + ">" + juce::String (x.in));
    s.settings->setValue ("patchBayDots", d.joinIntoString (";")); s.settings->setValue ("patchBayLinks", l.joinIntoString (";")); s.settings->saveIfNeeded();
}
void loadWires (Studio& s)
{
    static bool done = false;
    if (done || s.settings == nullptr) return;
    done = true;
    for (auto& row : juce::StringArray::fromTokens (s.settings->getValue ("patchBayDots"), ";", ""))
    {
        const auto f = juce::StringArray::fromTokens (row, ",", "");
        if (f.size() == 3) extraDots()[f[0]] = { juce::jlimit (0, 6, f[1].getIntValue()), juce::jlimit (0, 6, f[2].getIntValue()) };
    }
    for (auto& row : juce::StringArray::fromTokens (s.settings->getValue ("patchBayLinks"), ";", ""))
    {
        const auto f = juce::StringArray::fromTokens (row, ">", "");
        if (f.size() == 4) links().push_back ({ f[0], f[2], f[1].getIntValue(), f[3].getIntValue() });
    }
}
struct Node { Kind kind; int a = 0, b = 0; juce::String key, title; juce::Rectangle<int> r; juce::Colour c; };

class Canvas : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit Canvas (Studio& st) : s (st) { loadLfos (s); loadBays (s); loadWires (s); setWantsKeyboardFocus (true); setOpaque (true); s.addChangeListener (this); setSize (1800, 1400); rebuild(); startTimerHz (15); }
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
        {
            const auto& b = bay();
            std::vector<int> mine { b.insert }; mine.insert (mine.end(), b.stacks.begin(), b.stacks.end());
            for (int t : mine) if (t > 0 && t < (int) p.mixer.size() && ! trackY.count (t)) { trackY[t] = y; y += 86; }
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
            int feeds = 0;
            for (auto& l : p.lanes) feeds += l.mixerTrack == tr;
            for (auto& cl : p.clips) feeds += cl.mixerTrack == tr;
            place ({ Out, tr, 0, "O" + juce::String (tr), (mt.name.isNotEmpty() ? mt.name : "Insert " + juce::String (tr)) + (feeds > 0 ? "  < " + juce::String (feeds) + " in" : juce::String ("  Out")), {}, col::accent }, x, ty, 150, 70);
        }
        {   // every automation clip of this bay is its own mini plug-in node (LANE look), pluggable onto any node
            std::set<juce::String> seen;
            int ay = 20 + (int) lfos().size() * 130;
            for (auto& c : p.clips)
            {
                if (! c.isAuto() || c.autoTarget.name.startsWith ("LFO ") || ! seen.insert (c.autoTarget.name).second) continue;
                const auto& a = c.autoTarget;
                const bool chan = a.kind == AutoChanVol || a.kind == AutoChanPan || a.kind == AutoChanPitch;
                if (chan ? ! inBay (a.channel) : ! trackY.count (a.track)) continue;
                place ({ AutoNode, c.uid, 0, "A" + a.name, a.name, {}, juce::Colour (0xffff5a5a) }, 20, ay, 190, 96);
                ay += 112;
            }
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff141619));
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        for (int x = 0; x < getWidth(); x += 24) for (int yy = 0; yy < getHeight(); yy += 24) g.fillRect (x, yy, 2, 2);
        collectCables();
        for (auto& c : cables) drawPath (g, c.path, c.colour, c.dashed);
        if (dragFrom != nullptr)
        {
            const auto a = dragIsIn ? inPort (*dragFrom, dragPort) : port (*dragFrom, dragPort);
            wire (g, dragIsIn ? dragTo.toFloat() : a, dragIsIn ? a : dragTo.toFloat(), juce::Colours::white, true);
        }
        for (auto& n : nodes) drawNode (g, n);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        moving = nullptr; dragFrom = nullptr; kKey = {};
        auto* n = at (e.getPosition());
        if (n == nullptr)
        {
            if (e.mods.isPopupMenu()) { if (auto* c = cableAt (e.position)) cableMenu (*c); else canvasMenu(); }
            return;
        }
        grabKeyboardFocus();
        if (n->kind == Seq || n->kind == Sound) { lastChan = n->a; lastTrack = -1; }
        if (n->kind == Fx || n->kind == Out) lastTrack = n->a;
        for (int w = 0; w < 3; ++w)
            if (hasK (*n, w) && kRect (*n, w).expanded (2).contains (e.getPosition()))
            {
                if (w == 2) { autoMenu (*n, -1); return; }
                if (e.mods.isPopupMenu()) { knobMenu (*n, w); return; }
                if (e.mods.isAltDown()) { kReset (*n, w); return; }
                kKey = n->key; kWhich = w; kStart = kGet (*n, w); kY = e.y; return;
            }
        for (int i = 0; i < outs (*n); ++i)
            if (port (*n, i).getDistanceFrom (e.position) < 10.0f)
            {
                if (e.mods.isPopupMenu()) { dotMenu (*n, false, i); return; }
                dragFrom = n; dragPort = i; dragIsIn = false; dragTo = e.getPosition(); return;
            }
        for (int i = 0; i < ins (*n); ++i)
            if (inPort (*n, i).getDistanceFrom (e.position) < 10.0f)
            {
                if (e.mods.isPopupMenu()) { dotMenu (*n, true, i); return; }
                // grab a plugged cable off its input dot to move or unplug it, or start a new cable backwards from an empty one
                for (size_t k = 0; k < cables.size(); ++k)
                    if (cables[k].to == n->key && cables[k].in == i && cables[k].type != ChainCable)
                        if (auto* src = byKey (cables[k].from))
                        {
                            const auto c = cables[k]; const int sp = c.out;
                            detach (c);
                            n = nullptr;
                            for (auto& x : nodes) if (x.key == c.from) n = &x;
                            if (n == nullptr) return;
                            dragFrom = n; dragPort = sp; dragIsIn = false; dragTo = e.getPosition(); juce::ignoreUnused (src); return;
                        }
                dragFrom = n; dragPort = i; dragIsIn = true; dragTo = e.getPosition(); return;
            }
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
        if (kKey.isNotEmpty())
        {
            if (auto* n = byKey (kKey))
            {
                const float range = kWhich == 1 ? 2.0f : 1.0f, lo = kWhich == 1 ? -1.0f : 0.0f;
                kSet (*n, kWhich, juce::jlimit (lo, lo + range, kStart + (float) (kY - e.y) * range / (e.mods.isShiftDown() ? 600.0f : 150.0f)), false);
            }
            return;
        }
        if (moving == nullptr) return;
        moving->r.setPosition (e.getPosition() - grab);
        positions()[moving->key] = moving->r.getPosition();
        repaint();
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (kKey.isNotEmpty()) { kKey = {}; s.commit ("Patch Bay knob"); return; }
        if (dragFrom == nullptr) return;
        auto* from = dragFrom; dragFrom = nullptr; repaint();
        auto* to = at (e.getPosition());
        if (to == nullptr || to == from) return;
        int outDot = dragPort, inDot = nearestIn (*to, e.position);
        if (dragIsIn) { std::swap (from, to); inDot = dragPort; outDot = nearestOut (*from, e.position); }
        connect (*from, outDot, *to, inDot);
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k.getModifiers().isAltDown() && (k.getKeyCode() == 'S' || k.getKeyCode() == 's') && lastChan >= 0) { hatMenu (lastChan, -1); return true; }
        return false;
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (phoneUI) return;   // phone: double-tap is right-click
        if (auto* n = at (e.getPosition()))
            for (int w = 0; w < 2; ++w)
                if (hasK (*n, w) && kRect (*n, w).expanded (2).contains (e.getPosition())) { kReset (*n, w); return; }
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
    // every node has at least one in and one out dot, plus any extra ones added from a dot's right-click menu
    static int ins (const Node& n)  { auto it = extraDots().find (n.key); return 1 + (it != extraDots().end() ? it->second.first : 0); }
    static int outs (const Node& n) { auto it = extraDots().find (n.key); return 1 + (it != extraDots().end() ? it->second.second : 0); }
    static float dotY (const Node& n, int i) { return (float) juce::jmin (n.r.getBottom() - 6, n.r.getY() + 14 + i * 14); }
    static juce::Point<float> port (const Node& n, int i = 0) { return { (float) n.r.getRight(), dotY (n, i) }; }
    static juce::Point<float> inPort (const Node& n, int i = 0) { return { (float) n.r.getX(), dotY (n, i) }; }
    int nearestIn (const Node& n, juce::Point<float> pt) const  { int b = 0; for (int i = 1; i < ins (n); ++i)  if (inPort (n, i).getDistanceFrom (pt) < inPort (n, b).getDistanceFrom (pt)) b = i; return b; }
    int nearestOut (const Node& n, juce::Point<float> pt) const { int b = 0; for (int i = 1; i < outs (n); ++i) if (port (n, i).getDistanceFrom (pt) < port (n, b).getDistanceFrom (pt)) b = i; return b; }
    static juce::Path wirePath (juce::Point<float> a, juce::Point<float> b)
    {
        juce::Path w; w.startNewSubPath (a);
        const float dx = juce::jmax (40.0f, std::abs (b.x - a.x) * 0.5f);
        w.cubicTo (a.x + dx, a.y, b.x - dx, b.y, b.x, b.y);
        return w;
    }
    static void drawPath (juce::Graphics& g, const juce::Path& w, juce::Colour c, bool dashed)
    {
        g.setColour (c.withAlpha (0.85f));
        if (! dashed) { g.strokePath (w, juce::PathStrokeType (2.0f)); return; }
        juce::Path d; const float dash[] { 6.0f, 4.0f };
        juce::PathStrokeType (1.6f).createDashedStroke (d, w, dash, 2);
        g.fillPath (d);
    }
    static void wire (juce::Graphics& g, juce::Point<float> a, juce::Point<float> b, juce::Colour c, bool dashed) { drawPath (g, wirePath (a, b), c, dashed); }

    // ---- cables: everything the project already connects, plus free cables; each one can be unplugged
    enum CableType { ChainCable, SeqCable, RouteCable, LfoCable, SendCable, FreeCable, AutoCable };
    struct Cable { CableType type; juce::String from, to; int out = 0, in = 0; juce::String autoName; int a = 0, b = 0; juce::Path path; juce::Colour colour; bool dashed = false; };
    std::vector<Cable> cables;
    Node* byKey (const juce::String& k) { for (auto& n : nodes) if (n.key == k) return &n; return nullptr; }
    void addCable (CableType t, const Node& f, int o, const Node* to, int i, juce::Colour c, bool dashed, juce::String autoName = {}, int a = 0, int b = 0)
    {
        if (to == nullptr) return;
        o = juce::jlimit (0, outs (f) - 1, o); i = juce::jlimit (0, ins (*to) - 1, i);
        cables.push_back ({ t, f.key, to->key, o, i, autoName, a, b, wirePath (port (f, o), inPort (*to, i)), c, dashed });
    }
    Node* firstOf (int track) { auto& p = s.p(); if (track < 0 || track >= (int) p.mixer.size()) return nullptr; auto& fx = p.mixer[(size_t) track].fx; return fx.empty() ? find (Out, track) : find (Fx, track, fx.front().uid); }
    void collectCables()
    {
        cables.clear();
        auto& p = s.p();
        const juce::Colour blue (0xff6ad1ff), lfoC (0xffd59cff), sendC (0xffffc04d);
        for (auto& n : nodes)
        {
            if (n.kind == Seq) addCable (SeqCable, n, 0, find (Sound, n.a), 0, blue, false);
            if (n.kind == Sound) addCable (RouteCable, n, 0, firstOf (n.b), 0, blue, false);
            if (n.kind == Fx)
            {
                auto& fx = p.mixer[(size_t) n.a].fx;
                for (size_t k = 0; k < fx.size(); ++k)
                    if (fx[k].uid == n.b) addCable (ChainCable, n, 0, k + 1 < fx.size() ? find (Fx, n.a, fx[k + 1].uid) : find (Out, n.a), 0, blue, false);
            }
            if (n.kind == Out && n.a > 0 && n.a < (int) p.mixer.size())
                for (auto& sd : p.mixer[(size_t) n.a].sends) addCable (SendCable, n, 0, firstOf (sd.target), 0, sendC, false, {}, n.a, sd.target);
        }
        std::set<juce::String> seen;
        for (auto& c : p.clips)
        {
            if (! c.isAuto() || ! c.autoTarget.name.startsWith ("LFO ") || ! seen.insert (c.autoTarget.name).second) continue;
            auto* from = find (LfoNode, c.autoTarget.name.fromFirstOccurrenceOf ("LFO ", false, false).getIntValue() - 1);
            if (from == nullptr) continue;
            const auto& a = c.autoTarget;
            addCable (LfoCable, *from, 0, targetNode (a), 0, lfoC, true, a.name);
        }
        for (auto& n : nodes)
            if (n.kind == AutoNode)
                if (auto* c = autoClip (n))
                    addCable (AutoCable, n, 0, targetNode (c->autoTarget), 0, juce::Colour (0xffff5a5a), true, c->autoTarget.name);
        for (auto& l : links())
        {
            auto* f = byKey (l.from); auto* to = byKey (l.to);
            if (f != nullptr) addCable (FreeCable, *f, l.out, to, l.in, juce::Colours::white.withAlpha (0.7f), false);
        }
    }
    Node* targetNode (const AutoTarget& a)
    {
        return a.kind == AutoFxParam ? find (Fx, a.track, a.fxUid)
             : (a.kind == AutoChanVol || a.kind == AutoChanPan || a.kind == AutoChanPitch) ? find (Sound, a.channel)
             : find (Out, a.track);
    }
    const Clip* autoClip (const Node& n)
    {
        const auto nm = n.key.substring (1);
        for (auto& c : s.p().clips) if (c.isAuto() && c.autoTarget.name == nm) return &c;
        return nullptr;
    }
    // point an automation clip node at a new target: FX node = pick a knob, sound = vol/pan/pitch, insert = vol/pan
    void retarget (const juce::String& oldName, const Node& to)
    {
        auto apply = [safe = juce::Component::SafePointer<Canvas> (this), oldName] (AutoTarget t) {
            if (safe == nullptr) return;
            safe->s.edit ("Patch Bay: move automation", [&] (Project& p) { for (auto& c : p.clips) if (c.isAuto() && c.autoTarget.name == oldName) c.autoTarget = t; });
            const auto it = positions().find ("A" + oldName);
            if (it != positions().end()) positions()["A" + t.name] = it->second;
            safe->rebuild();
        };
        juce::PopupMenu m;
        auto& p = s.p();
        if (to.kind == Fx)
        {
            std::vector<std::pair<juce::String, juce::String>> ps;
            if (auto* b = s.engine.builtinFx (to.a, to.b)) for (auto& d : b->params()) ps.push_back ({ d.id, d.name });
            else if (auto* pl = s.engine.fxPlugin (to.a, to.b))
                for (int k = 0; k < juce::jmin (128, pl->getParameters().size()); ++k) ps.push_back ({ "#" + juce::String (k), pl->getParameters()[k]->getName (32) });
            for (int i = 0; i < (int) ps.size(); ++i) m.addItem (i + 1, ps[(size_t) i].second);
            if (ps.empty()) m.addItem (-1, "This plug-in has no automatable knobs", false);
            const int tr = to.a, uid = to.b; const auto title = to.title;
            m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [apply, ps, tr, uid, title] (int r) {
                if (r <= 0) return;
                AutoTarget t; t.kind = AutoFxParam; t.track = tr; t.fxUid = uid; t.param = ps[(size_t) r - 1].first;
                t.name = title + " - " + ps[(size_t) r - 1].second; apply (t);
            });
            return;
        }
        const bool chan = to.kind == Sound || to.kind == Seq;
        if (! chan && to.kind != Out) return;
        m.addItem (1, "Volume"); m.addItem (2, "Pan"); if (chan) m.addItem (3, "Pitch");
        const int a = to.a;
        juce::String who = to.title; int track = a;
        if (chan) if (auto* c = p.findChannel (a)) { who = c->name; track = juce::jmax (0, c->mixerTrack); }
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [apply, chan, a, who, track] (int r) {
            if (r <= 0) return;
            AutoTarget t; const char* names[] { "", "Volume", "Pan", "Pitch" };
            if (chan) { t.kind = r == 1 ? AutoChanVol : r == 2 ? AutoChanPan : AutoChanPitch; t.channel = a; t.track = track; }
            else { t.kind = r == 1 ? AutoInsertVol : AutoInsertPan; t.track = a; }
            t.name = who + " - " + names[r]; apply (t);
        });
    }
    const Cable* cableAt (juce::Point<float> pt) const
    {
        for (auto it = cables.rbegin(); it != cables.rend(); ++it)
        {
            juce::Point<float> near; it->path.getNearestPoint (pt, near);
            if (near.getDistanceFrom (pt) < 7.0f) return &*it;
        }
        return nullptr;
    }
    // plug: whatever the two nodes mean together (sequence > sound, sound > insert, LFO > knob, insert > insert send); anything else is a free cable
    void connect (const Node& f, int o, const Node& to, int i)
    {
        if (f.kind == Seq && to.kind == Sound) { moveSequence (f.a, to.a); return; }
        // sound > any sequence (e.g. track 1 808 onto track 12's sequence), sequence > sequence, sound > sound: the target plays that rhythm too
        if ((f.kind == Sound || f.kind == Seq) && (to.kind == Seq || to.kind == Sound) && f.a != to.a)
        {
            const bool soundTakes = f.kind == Sound && to.kind == Seq;
            copyRhythm (soundTakes ? to.a : f.a, soundTakes ? f.a : to.a);
        }
        if (f.kind == Sound && (to.kind == Out || to.kind == Fx)) { route (f.a, to.a); return; }
        if (f.kind == LfoNode && ! (to.kind == LfoNode && to.a == f.a)) { lfoTo (f.a, to); if (to.kind != LfoNode && to.kind != AutoNode) return; }
        if (f.kind == AutoNode && to.kind != AutoNode && to.kind != LfoNode) { retarget (f.key.substring (1), to); return; }
        if ((f.kind == Fx || f.kind == Out) && (to.kind == Fx || to.kind == Out) && f.a != to.a && f.a > 0)
        {
            bool ok = false;
            s.edit ("Patch Bay: route insert", [&] (Project& p) {
                for (auto& sd : p.mixer[(size_t) f.a].sends) if (sd.target == to.a) { ok = true; return; }
                ok = p.toggleRoute (f.a, to.a);
            });
            if (ok) { rebuild(); return; }
        }
        for (auto& l : links()) if (l.from == f.key && l.to == to.key && l.out == o && l.in == i) return;
        links().push_back ({ f.key, to.key, o, i }); saveWires (s); repaint();
    }
    void detach (const Cable& c)
    {
        switch (c.type)
        {
            case SeqCable: case ChainCable: break;   // a sound always plays its own sequence and an insert's FX run in order: remove the node instead
            case RouteCable:
                if (auto* f = byKey (c.from)) { const int ch = f->a; s.edit ("Patch Bay: unplug sound", [&] (Project& p) { if (auto* x = p.findChannel (ch)) x->mixerTrack = 0; }); }
                break;
            case SendCable: { const int a = c.a, b = c.b; s.edit ("Patch Bay: unplug send", [&] (Project& p) { p.toggleRoute (a, b); }); break; }
            case LfoCable: case AutoCable:
            {
                const auto nm = c.autoName;
                s.edit ("Patch Bay: unplug LFO", [&] (Project& p) { p.clips.erase (std::remove_if (p.clips.begin(), p.clips.end(), [&] (const Clip& x) { return x.isAuto() && x.autoTarget.name == nm; }), p.clips.end()); });
                break;
            }
            case FreeCable:
                links().erase (std::remove_if (links().begin(), links().end(), [&] (const Link& l) { return l.from == c.from && l.to == c.to && l.out == c.out && l.in == c.in; }), links().end());
                saveWires (s);
                break;
        }
        rebuild();
    }
    void cableMenu (const Cable& c)
    {
        juce::PopupMenu m;
        const bool fixed = c.type == SeqCable || c.type == ChainCable;
        m.addItem (1, fixed ? "Fixed cable (remove the node to unplug)" : "Unplug cable", ! fixed);
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<Canvas> (this), c] (int r) { if (safe != nullptr && r == 1) safe->detach (c); });
    }
    void dotMenu (const Node& n, bool in, int dot)
    {
        juce::PopupMenu m;
        auto& d = extraDots()[n.key];
        m.addItem (1, "Add input dot", d.first < 6); m.addItem (2, "Add output dot", d.second < 6);
        m.addItem (3, "Remove extra dots", d.first > 0 || d.second > 0);
        m.addSeparator(); m.addItem (4, juce::String ("Unplug everything on this ") + (in ? "input" : "output") + " dot");
        const auto key = n.key;
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<Canvas> (this), key, in, dot] (int r) {
            if (safe == nullptr || r <= 0) return;
            auto& e = extraDots()[key];
            if (r == 1) ++e.first; else if (r == 2) ++e.second; else if (r == 3) e = { 0, 0 };
            else if (r == 4)
            {
                std::vector<Cable> hit;
                for (auto& c : safe->cables) if (in ? (c.to == key && c.in == dot) : (c.from == key && c.out == dot)) hit.push_back (c);
                for (auto& c : hit) if (safe != nullptr) safe->detach (c);
            }
            if (safe == nullptr) return;
            if (r == 3) links().erase (std::remove_if (links().begin(), links().end(), [&] (const Link& l) { return (l.from == key && l.out > 0) || (l.to == key && l.in > 0); }), links().end());
            saveWires (safe->s); safe->repaint();
        });
    }

    void drawNode (juce::Graphics& g, const Node& n)
    {
        const auto r = n.r.toFloat();
        g.setColour (juce::Colour (0xff23262b)); g.fillRoundedRectangle (r, 6.0f);
        g.setColour (juce::Colours::white.withAlpha (0.12f)); g.drawRoundedRectangle (r, 6.0f, 1.0f);
        g.setColour (n.c); g.fillRoundedRectangle (r.withWidth (4.0f), 2.0f);
        g.setColour (col::text); g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        fitText (g, n.title, n.r.getX() + 10, n.r.getY() + 4, n.r.getWidth() - (n.kind == LfoNode ? 88 : 96), 18, juce::Justification::centredLeft, true);
        drawKnobs (g, n);
        g.setColour (juce::Colour (0xff6ad1ff));
        for (int i = 0; i < ins (n); ++i) g.fillEllipse (inPort (n, i).x - 5, inPort (n, i).y - 5, 10, 10);
        g.setColour (n.kind == LfoNode ? juce::Colour (0xffd59cff) : juce::Colour (0xff6ad1ff));
        for (int i = 0; i < outs (n); ++i) g.fillEllipse (port (n, i).x - 5, port (n, i).y - 5, 10, 10);
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
            g.setColour (col::dim); fitText (g, "click steps, Alt-click: hat roll, right-click: Piano Roll", n.r.getX() + 10, n.r.getY() + 20, 260, 12, juce::Justification::centredLeft);
        }
        else if (n.kind == LfoNode && n.a < (int) lfos().size())
        {
            const auto& l = lfos()[(size_t) n.a];
            fitText (g, juce::String (shapeNames[l.shape]) + "  " + juce::String (l.perBar, 2) + " / bar  " + juce::String (juce::roundToInt (l.lo * 100)) + "-" + juce::String (juce::roundToInt (l.hi * 100)) + "%",
                        n.r.getX() + 10, n.r.getY() + 22, n.r.getWidth() - 20, 14, juce::Justification::centredLeft);
            juce::Path w; const auto box = r.reduced (10.0f).withTrimmedTop (32.0f);
            for (int i = 0; i <= 80; ++i)
            {
                const float x = box.getX() + box.getWidth() * i / 80.0f, yv = box.getBottom() - box.getHeight() * lfoAt (l, i / 40.0);
                if (i == 0) w.startNewSubPath (x, yv); else w.lineTo (x, yv);
            }
            g.setColour (n.c); g.strokePath (w, juce::PathStrokeType (1.6f));
            g.setColour (col::dim); fitText (g, "right-click: shape / rate / range", n.r.getX() + 10, n.r.getBottom() - 14, 170, 12, juce::Justification::centredLeft);
        }
        else if (n.kind == AutoNode)
        {
            if (auto* c = autoClip (n))
            {
                const double pos = s.engine.positionBeats();
                float v = -1.0f;
                for (auto& cl : p.clips)
                    if (cl.isAuto() && cl.autoTarget.name == c->autoTarget.name && ! cl.muted && pos >= cl.start && pos < cl.start + cl.length)
                        v = autoValueAt (cl.autoPts, cl.patOffset + pos - cl.start);
                auto box = r.reduced (10.0f).withTrimmedTop (20.0f);
                auto meter = box.removeFromBottom (8.0f); box.removeFromBottom (4.0f);
                g.setColour (juce::Colour (0xff15171a)); g.fillRoundedRectangle (box, 3.0f);
                juce::Path w;
                const double len = juce::jmax (0.001, c->length);
                for (int i = 0; i <= 60; ++i)
                {
                    const float x = box.getX() + box.getWidth() * i / 60.0f;
                    const float yv = box.getBottom() - box.getHeight() * juce::jlimit (0.0f, 1.0f, autoValueAt (c->autoPts, c->patOffset + len * i / 60.0));
                    if (i == 0) w.startNewSubPath (x, yv); else w.lineTo (x, yv);
                }
                g.setColour (n.c); g.strokePath (w, juce::PathStrokeType (1.5f));
                g.setColour (juce::Colour (0xff15171a)); g.fillRoundedRectangle (meter, 2.0f);
                const float t = v >= 0.0f ? v : autoValueAt (c->autoPts, c->patOffset);
                g.setColour (v >= 0.0f ? n.c : col::accent); g.fillRoundedRectangle (meter.withWidth (juce::jmax (2.0f, meter.getWidth() * juce::jlimit (0.0f, 1.0f, t))), 2.0f);
                g.setColour (col::dim); g.setFont (juce::FontOptions (10.0f));
                fitText (g, juce::String (juce::roundToInt (t * 100.0f)) + "%" + (v >= 0.0f ? "  automating" : "  drag dot onto a node to move it"), n.r.getX() + 10, n.r.getY() + 20, n.r.getWidth() - 20, 12, juce::Justification::centredLeft);
            }
        }
        else if (n.kind == Sound) fitText (g, "SIG OUT  >  insert " + juce::String (n.b), n.r.getX() + 10, n.r.getY() + 30, 150, 14, juce::Justification::centredLeft);
        else if (n.kind == Fx) fitText (g, "right-click: automate / gate / LFO", n.r.getX() + 10, n.r.getY() + 30, 150, 14, juce::Justification::centredLeft);
        else if (n.kind == Out) fitText (g, "drop a sound here to route it", n.r.getX() + 10, n.r.getY() + 30, 140, 14, juce::Justification::centredLeft);
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
            if (p.patterns.empty() || fromCh == toCh) return;
            auto& pt = p.patterns[(size_t) juce::jlimit (0, (int) p.patterns.size() - 1, p.currentPattern)];
            auto it = pt.notes.find (fromCh);
            if (it == pt.notes.end()) return;
            const auto src = it->second;   // stacked: the first sound keeps playing it too
            mergeNotes (pt.notes[toCh], src, 0);
        });
    }
    void copyRhythm (int fromCh, int toCh)
    {
        s.edit ("Patch Bay: link rhythm", [&] (Project& p) {
            if (p.patterns.empty()) return;
            auto& pt = p.patterns[(size_t) juce::jlimit (0, (int) p.patterns.size() - 1, p.currentPattern)];
            auto it = pt.notes.find (fromCh);
            if (it == pt.notes.end() || it->second.empty()) return;
            auto& dst = pt.notes[toCh];
            const int shift = dst.empty() ? 0 : dst.front().key - it->second.front().key;   // keep the target's own pitch
            const auto src = it->second;
            mergeNotes (dst, src, shift);   // added on top: nothing the target already plays is cut
        });
    }
    static void mergeNotes (NoteList& dst, const NoteList& src, int shift)
    {
        for (auto nt : src)
        {
            nt.key = juce::jlimit (0, 127, nt.key + shift);
            bool dup = false;
            for (const auto& d : dst) dup |= d.key == nt.key && std::abs (d.start - nt.start) < 1.0e-6;
            if (! dup) dst.push_back (nt);
        }
        std::sort (dst.begin(), dst.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
    }
    // a sound that shares its insert with other sounds gets its own insert (feeding the old one), so FX / gates added for it never cut the rest
    int ownInsert (int ch)
    {
        int t = -1;
        s.edit ("Patch Bay: own insert", [&] (Project& p) {
            auto* c = p.findChannel (ch);
            if (c == nullptr) return;
            const int old = c->mixerTrack;
            bool shared = old <= 0;
            for (auto& o : p.channels) shared |= o.id != ch && o.mixerTrack == old;
            if (! shared) { t = old; return; }
            t = p.addMixerTrack (c->name);
            if (t <= 0 || t >= (int) p.mixer.size()) { t = old; return; }
            if (old > 0 && old < (int) p.mixer.size()) { auto& mt = p.mixer[(size_t) t]; mt.routesToMaster = false; mt.sends.clear(); mt.sends.push_back ({ old, 1.0f }); }
            c->mixerTrack = t;
        });
        rebuild();
        return t;
    }
    // quick knobs on every node: 0 level, 1 pan, 2 automate (LFO: 0 = depth)
    static bool hasK (const Node& n, int w) { return n.kind == AutoNode ? false : n.kind == LfoNode ? w == 0 : true; }
    static juce::Rectangle<int> kRect (const Node& n, int w) { return { n.r.getRight() - 80 + w * 22, n.r.getY() + 3, 18, 18 }; }
    float kGet (const Node& n, int w)
    {
        auto& p = s.p();
        if (n.kind == LfoNode) return n.a < (int) lfos().size() ? lfos()[(size_t) n.a].hi : 1.0f;
        if (n.kind == Seq || n.kind == Sound) { if (auto* c = p.findChannel (n.a)) return w == 0 ? c->volume : c->pan; return 0.0f; }
        if (n.a < 0 || n.a >= (int) p.mixer.size()) return 0.0f;
        auto& t = p.mixer[(size_t) n.a];
        if (n.kind == Fx && w == 0) { for (auto& f : t.fx) if (f.uid == n.b) return f.mix; return 1.0f; }
        return w == 0 ? t.volume : t.pan;
    }
    void kSet (const Node& n, int w, float v, bool commit)
    {
        if (n.kind == LfoNode) { if (n.a < (int) lfos().size()) { lfos()[(size_t) n.a].hi = juce::jlimit (lfos()[(size_t) n.a].lo, 1.0f, v); saveLfos (s); } repaint(); return; }
        const Kind k = n.kind; const int a = n.a, b = n.b;
        s.live ([&] (Project& p) {
            if (k == Seq || k == Sound) { if (auto* c = p.findChannel (a)) (w == 0 ? c->volume : c->pan) = v; return; }
            if (a < 0 || a >= (int) p.mixer.size()) return;
            auto& t = p.mixer[(size_t) a];
            if (k == Fx && w == 0) { for (auto& f : t.fx) if (f.uid == b) f.mix = v; return; }
            (w == 0 ? t.volume : t.pan) = v;
        });
        if (auto& ro = Studio::readout()) ro ((w == 0 ? (k == Fx ? "Mix " : "Level ") + juce::String (juce::roundToInt (v * 100)) + "%"
                                                   : "Pan " + juce::String (juce::roundToInt (v * 100))));
        if (commit) s.commit ("Patch Bay knob");
        repaint();
    }
    void kReset (const Node& n, int w) { kSet (n, w, w == 1 ? 0.0f : n.kind == Fx || n.kind == LfoNode ? 1.0f : 0.8f, true); }
    void knobMenu (const Node& n, int w)
    {
        juce::PopupMenu m;
        m.addItem (1, "Reset");
        m.addItem (2, "Min"); m.addItem (3, "Middle"); m.addItem (4, "Max");
        if (n.kind != LfoNode) { m.addSeparator(); m.addItem (5, w == 0 ? (n.kind == Fx ? "Automate this FX..." : "Automate level") : "Automate pan"); }
        const auto key = n.key;
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<Canvas> (this), key, w] (int r) {
            if (safe == nullptr || r <= 0) return;
            auto* x = safe->byKey (key); if (x == nullptr) return;
            const float lo = w == 1 ? -1.0f : 0.0f;
            if (r == 1) safe->kReset (*x, w);
            else if (r <= 4) safe->kSet (*x, w, r == 2 ? lo : r == 3 ? (lo + 1.0f) * 0.5f : 1.0f, true);
            else safe->autoMenu (*x, w == 0 ? 1 : 2);
        });
    }
    // automate a node's level / pan / pitch as a Playlist automation clip (FX nodes: any knob of that FX)
    void autoMenu (const Node& n, int pick)
    {
        if (n.kind == LfoNode || n.kind == AutoNode) return;
        if (n.kind == Fx && pick != 2) { fxMenu (n.a, n.b); return; }
        const bool chan = n.kind == Sound || n.kind == Seq;
        const int a = n.a;
        auto make = [safe = juce::Component::SafePointer<Canvas> (this), a, chan] (int r) {
            if (safe == nullptr || r <= 0) return;
            AutoTarget t; juce::String who;
            const char* names[] { "", "Volume", "Pan", "Pitch" };
            if (chan)
            {
                t.kind = r == 1 ? AutoChanVol : r == 2 ? AutoChanPan : AutoChanPitch; t.channel = a;
                if (auto* c = safe->s.p().findChannel (a)) { t.track = juce::jmax (0, c->mixerTrack); who = c->name; }
            }
            else
            {
                t.kind = r == 1 ? AutoInsertVol : AutoInsertPan; t.track = a;
                if (a >= 0 && a < (int) safe->s.p().mixer.size()) who = safe->s.p().mixer[(size_t) a].name;
            }
            t.name = who + " - " + names[r];
            createAutomationClip (safe->s, t, (int) safe->s.p().lanes.size() - 1);
            safe->rebuild();
        };
        if (pick > 0) { make (pick); return; }
        juce::PopupMenu m;
        m.addItem (1, "Automate volume"); m.addItem (2, "Automate pan");
        if (chan) m.addItem (3, "Automate pitch");
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), make);
    }
    void drawKnobs (juce::Graphics& g, const Node& n)
    {
        for (int w = 0; w < 2; ++w)
        {
            if (! hasK (n, w)) continue;
            const auto r = kRect (n, w).toFloat();
            const float v = kGet (n, w), t = w == 1 ? (v + 1.0f) * 0.5f : juce::jlimit (0.0f, 1.0f, v);
            const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = a0 + juce::MathConstants<float>::pi * 1.5f;
            g.setColour (juce::Colour (0xff15171a)); g.fillEllipse (r);
            juce::Path bg, arc;
            bg.addCentredArc (r.getCentreX(), r.getCentreY(), 7.0f, 7.0f, 0.0f, a0, a1, true);
            g.setColour (juce::Colours::white.withAlpha (0.15f)); g.strokePath (bg, juce::PathStrokeType (2.0f));
            const float from = w == 1 ? (a0 + a1) * 0.5f : a0, to = a0 + (a1 - a0) * t;
            arc.addCentredArc (r.getCentreX(), r.getCentreY(), 7.0f, 7.0f, 0.0f, juce::jmin (from, to), juce::jmax (from, to), true);
            g.setColour (n.c.brighter (0.3f)); g.strokePath (arc, juce::PathStrokeType (2.0f));
            const float ang = a0 + (a1 - a0) * t;
            g.drawLine (r.getCentreX(), r.getCentreY(), r.getCentreX() + 6.0f * std::sin (ang), r.getCentreY() - 6.0f * std::cos (ang), 1.4f);
        }
        if (hasK (n, 2))
        {
            const auto r = kRect (n, 2).toFloat();
            g.setColour (juce::Colour (0xff15171a)); g.fillRoundedRectangle (r, 3.0f);
            g.setColour (juce::Colour (0xffd59cff)); g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("A", r, juce::Justification::centred);
        }
    }
    void route (int ch, int track)
    {
        s.edit ("Patch Bay: route sound", [&] (Project& p) {
            if (auto* c = p.findChannel (ch); c != nullptr && track > 0 && track < (int) p.mixer.size()) c->mixerTrack = track;
        });
    }
    // LFO cable onto any node: FX knob, sound volume / pan / pitch, insert volume / pan; anything else (sequence) automates its sound
    // LFO onto any node: sound (volume / pan / pitch / every instrument + VST knob / its insert), insert (volume / pan / sends / every FX knob), FX, other LFOs, automation
    void lfoTo (int li, const Node& to)
    {
        if (to.kind == Fx) { lfoToParam (li, to.a, to.b); return; }
        if (to.kind == LfoNode || to.kind == AutoNode) { lfoStack (li, to); return; }
        auto& p = s.p();
        const bool chan = to.kind == Sound || to.kind == Seq;
        int track = to.a; juce::String who = to.title;
        std::vector<AutoTarget> ts;
        juce::PopupMenu m, kn, ins;
        auto add = [&] (juce::PopupMenu& menu, AutoTarget t, const juce::String& label) { t.name = "LFO " + juce::String (li + 1) + " > " + t.name; ts.push_back (t); menu.addItem ((int) ts.size(), label); };
        if (chan)
        {
            auto* c = p.findChannel (to.a);
            if (c == nullptr) return;
            who = c->name; track = c->mixerTrack;
            const char* nm3[] { "Volume", "Pan", "Pitch" };
            for (int k = 0; k < 3; ++k) { AutoTarget t; t.kind = k == 0 ? AutoChanVol : k == 1 ? AutoChanPan : AutoChanPitch; t.channel = c->id; t.track = juce::jmax (0, track); t.name = who + " - " + nm3[k]; add (m, t, nm3[k]); }
            if (auto* pl = s.engine.channelPlugin (c->id))
            {
                for (int k = 0; k < juce::jmin (256, pl->getParameters().size()); ++k)
                {
                    const auto pn = pl->getParameters()[k]->getName (32);
                    AutoTarget t; t.kind = AutoChanParam; t.channel = c->id; t.track = juce::jmax (0, track); t.param = "#" + juce::String (k); t.name = who + " - " + pn; add (kn, t, pn);
                }
                m.addSubMenu ("Instrument knobs (" + pl->getName() + ")", kn);
            }
        }
        if (track > 0 && track < (int) p.mixer.size())
        {
            auto& menu = chan ? ins : m;
            const auto& mt = p.mixer[(size_t) track];
            const auto tn = mt.name.isNotEmpty() ? mt.name : "Insert " + juce::String (track);
            { AutoTarget t; t.kind = AutoInsertVol; t.track = track; t.name = tn + " - Volume"; add (menu, t, "Insert volume"); }
            { AutoTarget t; t.kind = AutoInsertPan; t.track = track; t.name = tn + " - Pan"; add (menu, t, "Insert pan"); }
            for (const auto& sd : mt.sends) { AutoTarget t; t.kind = AutoSendLevel; t.track = track; t.param = juce::String (sd.target); t.name = tn + " - Send " + juce::String (sd.target); add (menu, t, "Send to insert " + juce::String (sd.target)); }
            for (const auto& f : mt.fx)
            {
                juce::PopupMenu fm;
                for (const auto& pr : fxParams (track, f.uid)) { AutoTarget t; t.kind = AutoFxParam; t.track = track; t.fxUid = f.uid; t.param = pr.first; t.name = tn + " - " + pr.second; add (fm, t, pr.second); }
                juce::String fxn = f.type;
                if (auto* pl = s.engine.fxPlugin (track, f.uid)) fxn = pl->getName();
                else for (auto& e : builtinEffects()) if (f.type == e.id) fxn = juce::String (e.name);
                menu.addSubMenu (fxn, fm);
            }
            if (chan) m.addSubMenu ("Its mixer insert (" + tn + ")", ins);
        }
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<Canvas> (this), li, ts] (int r) {
            if (safe == nullptr || r <= 0 || r > (int) ts.size()) return;
            safe->applyLfo (li, ts[(size_t) r - 1]);
        });
    }
    // LFO onto another LFO or an automation node: it rides on top (multiplies those moves), so LFOs stack into chains
    void lfoStack (int li, const Node& to)
    {
        if (li < 0 || li >= (int) lfos().size()) return;
        const auto l = lfos()[(size_t) li];
        const juce::String pre = to.kind == LfoNode ? "LFO " + juce::String (to.a + 1) + " > " : juce::String();
        const juce::String exact = to.kind == AutoNode ? to.key.substring (1) : juce::String();
        s.edit ("Patch Bay: stack LFO", [&] (Project& p) {
            const double period = p.beatsPerBar / juce::jmax (0.01, l.perBar);
            for (auto& c : p.clips)
            {
                if (! c.isAuto() || (exact.isNotEmpty() ? c.autoTarget.name != exact : ! c.autoTarget.name.startsWith (pre))) continue;
                const auto old = c.autoPts;
                const double stepB = juce::jmax (0.125, c.length / 4000.0);
                c.autoPts.clear();
                for (double x = 0.0; x <= c.length + 1.0e-9; x += stepB)
                    c.autoPts.push_back ({ x, juce::jlimit (0.0f, 1.0f, autoValueAt (old, x) * lfoAt (l, x / period)) });
            }
        });
        rebuild();
    }
    void applyLfo (int li, const AutoTarget& t)
    {
        if (li < 0 || li >= (int) lfos().size()) return;
        createAutomationClip (s, t, (int) s.p().lanes.size() - 1);
        const auto l = lfos()[(size_t) li];
        s.edit ("Patch Bay: LFO shape", [&] (Project& p) {
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
        rebuild();
    }
    void lfoToParam (int li, int track, int uid)
    {
        juce::PopupMenu m;
        std::vector<std::pair<juce::String, juce::String>> ps;
        if (auto* b = s.engine.builtinFx (track, uid)) for (auto& d : b->params()) ps.push_back ({ d.id, d.name });
        else if (auto* pl = s.engine.fxPlugin (track, uid))
            for (int k = 0; k < juce::jmin (128, pl->getParameters().size()); ++k) ps.push_back ({ "#" + juce::String (k), pl->getParameters()[k]->getName (32) });
        for (int i = 0; i < (int) ps.size(); ++i) m.addItem (i + 1, ps[(size_t) i].second);
        if (ps.empty()) m.addItem (-1, "This plug-in has no automatable knobs", false);
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<Canvas> (this), ps, li, track, uid] (int r) {
            if (safe == nullptr || r <= 0 || li >= (int) lfos().size()) return;
            AutoTarget t; t.kind = AutoFxParam; t.track = track; t.fxUid = uid; t.param = ps[(size_t) r - 1].first;
            t.name = "LFO " + juce::String (li + 1) + " > " + ps[(size_t) r - 1].second;
            safe->applyLfo (li, t);
        });
    }
    void nodeMenu (Node& n)
    {
        if (n.kind == Seq || n.kind == Sound) { channelMenu (n.a); return; }
        if (n.kind == Fx) { fxMenu (n.a, n.b); return; }
        if (n.kind == Out) { canvasMenu(); return; }
        if (n.kind == AutoNode)
        {
            juce::PopupMenu m;
            m.addItem (1, "Drag its dot onto any node to move this automation", false);
            m.addItem (2, "Delete automation");
            const auto nm = n.key.substring (1);
            m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<Canvas> (this), nm] (int r) {
                if (safe == nullptr || r != 2) return;
                safe->s.edit ("Patch Bay: delete automation", [&] (Project& p) { p.clips.erase (std::remove_if (p.clips.begin(), p.clips.end(), [&] (const Clip& x) { return x.isAuto() && x.autoTarget.name == nm; }), p.clips.end()); });
                safe->rebuild();
            });
            return;
        }
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
        juce::PopupMenu cn, mx;
        std::vector<Node> targets;
        for (const auto& o : nodes)
            if (! (o.kind == LfoNode && o.a == li)) { targets.push_back (o); cn.addItem (500 + (int) targets.size() - 1, o.title); }
        const auto& pj = s.p();
        for (int i = 1; i < (int) pj.mixer.size(); ++i)
        {
            Node o; o.kind = Out; o.a = i; o.key = "O" + juce::String (i);
            o.title = pj.mixer[(size_t) i].name.isNotEmpty() ? pj.mixer[(size_t) i].name : "Insert " + juce::String (i);
            targets.push_back (o); mx.addItem (500 + (int) targets.size() - 1, o.title);
        }
        cn.addSubMenu ("Mixer inserts", mx);
        m.addSeparator(); m.addSubMenu ("Connect to", cn);
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<Canvas> (this), li, rates, targets] (int r) {
            if (safe == nullptr || r <= 0 || li >= (int) lfos().size()) return;
            if (r >= 500) { if (r - 500 < (int) targets.size()) safe->lfoTo (li, targets[(size_t) (r - 500)]); return; }
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
        m.addItem (16, "AUTO: generate a pattern in " + juce::MidiMessage::getMidiNoteName (s.p().keyRoot, true, false, 3) + " for this bay");
        m.addItem (5, "New Patch Bay"); m.addItem (6, "Rename " + b.name); m.addItem (7, "Duplicate " + b.name); m.addItem (8, "Delete " + b.name, bays().size() > 1);
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<Canvas> (this)] (int r) { if (safe != nullptr) safe->canvasResult (r); });
    }
    void canvasResult (int r)
    {
        if (r <= 0) return;
        auto& b = bay();
        auto addCh = [&] (Channel c) {
            int id = -1;
            s.edit ("Patch Bay: add instrument", [&] (Project& p) { ensurePattern (p); if (b.route != 0 && b.insert > 0 && b.insert < (int) p.mixer.size()) c.mixerTrack = b.insert; else c.mixerTrack = -1; id = p.addChannel (c); });
            if (! b.chans.empty() && id >= 0) b.chans.push_back (id);
            s.selectedChannel = id; saveBays (s); if (b.mode == 1) autoGen(); rebuild();
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
        else if (r == 16) { autoGen(); rebuild(); }
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
        s.edit ("Patch Bay: add sound", [&] (Project& p) { ensurePattern (p); c.mixerTrack = (b.route != 0 && b.insert > 0 && b.insert < (int) p.mixer.size()) ? b.insert : -1; id = p.addChannel (c); });
        if (! b.chans.empty() && id >= 0) b.chans.push_back (id);
        s.selectedChannel = id; saveBays (s); if (b.mode == 1) autoGen(); rebuild();
    }
    // AUTO: a 2-bar pattern for every sound in the bay, in the project's key and scale (drums: kick / snare-clap / hats / percs; 808 + bass: low roots; the rest: a melody)
    void autoGen()
    {
        s.edit ("Patch Bay: AUTO pattern", [&] (Project& p) {
            ensurePattern (p);
            auto& pat = p.patterns[(size_t) juce::jlimit (0, (int) p.patterns.size() - 1, p.currentPattern)];
            juce::Random rnd;
            std::vector<int> keys, low;
            for (int k = 55; k <= 79; ++k) if (inScale (k, p.keyRoot, p.scaleIndex)) keys.push_back (k);
            for (int k = 28; k <= 43; ++k) if (inScale (k, p.keyRoot, p.scaleIndex)) low.push_back (k);
            if (keys.empty()) keys.push_back (60);
            if (low.empty()) low.push_back (36);
            const int root = 36 + ((p.keyRoot % 12) + 12) % 12;
            for (auto& c : p.channels)
            {
                if (! inBay (c.id) || c.type == ChannelType::Drummer) continue;
                NoteList nl; const auto n = c.name.toLowerCase();
                auto add = [&] (int key, double st, double l, float v) { Note x; x.key = key; x.start = st; x.length = l; x.velocity = v; nl.push_back (x); };
                const bool melodic = c.type != ChannelType::Sampler || c.keyTrack;
                if (n.contains ("808") || n.contains ("bass"))
                {
                    for (int bar = 0; bar < 2; ++bar)
                        for (double st : { 0.0, 1.5, 2.75 })
                            if (st == 0.0 || rnd.nextFloat() < 0.7f)
                                add (st == 0.0 ? root : low[(size_t) rnd.nextInt ((int) low.size())], bar * 4.0 + st, st == 0.0 ? 1.25 : 0.75, 0.9f);
                }
                else if (! melodic || isDrumSampleName (c.name))
                {
                    const bool kick = n.contains ("kick") || n.contains ("kik") || n.contains ("bd");
                    const bool snare = n.contains ("snare") || n.contains ("clap") || n.contains ("rim") || n.contains ("sd");
                    const bool hat = n.contains ("hat") || n.contains ("hh") || n.contains ("ride") || n.contains ("shaker");
                    for (int st = 0; st < 32; ++st)
                    {
                        const double b = st * 0.25; const int inBar = st % 16;
                        bool on = false; float v = 0.85f;
                        if (kick) on = inBar == 0 || inBar == 10 || (inBar == 7 && rnd.nextFloat() < 0.5f) || (inBar == 14 && rnd.nextFloat() < 0.3f);
                        else if (snare) on = inBar == 4 || inBar == 12;
                        else if (hat) { on = st % 2 == 0 || rnd.nextFloat() < 0.25f; v = st % 4 == 0 ? 0.8f : 0.55f; }
                        else on = rnd.nextFloat() < 0.12f;
                        if (on) add (60, b, 0.25, v);
                    }
                }
                else
                {
                    int at = rnd.nextInt ((int) keys.size());
                    for (int st = 0; st < 16; ++st)
                    {
                        if (st > 0 && rnd.nextFloat() < 0.3f) continue;
                        at = juce::jlimit (0, (int) keys.size() - 1, at + rnd.nextInt (5) - 2);
                        add (keys[(size_t) at], st * 0.5, rnd.nextFloat() < 0.3f ? 1.0 : 0.5, 0.65f + rnd.nextFloat() * 0.25f);
                    }
                }
                pat.notes[c.id] = nl;
            }
        });
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
        if (lastTrack <= 0 && p.findChannel (lastChan) != nullptr) if (const int t = ownInsert (lastChan); t > 0) return t;
        if (lastTrack > 0 && lastTrack < (int) p.mixer.size()) return lastTrack;
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
            if (b.route != 1) ensurePattern (p);
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
        m.addItem (4, c->type == ChannelType::Plugin ? "Open " + c->name + " (VST window)" : "Open " + c->name);
        m.addItem (5, "Own mixer insert (its FX never touch the other sounds)");
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
            else if (r == 4) { s.selectedChannel = ch; openChannelEditor (s, ch); }
            else if (r == 5) safe->ownInsert (ch);
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
    int lastChan = -1, lastTrack = -1;
    std::unique_ptr<juce::FileChooser> chooser;
    void changeListenerCallback (juce::ChangeBroadcaster*) override { moving = nullptr; dragFrom = nullptr; rebuild(); }
    void timerCallback() override { if (isShowing() && s.engine.isPlaying()) repaint(); }

    Studio& s;
    std::vector<Node> nodes;
    Node* moving = nullptr; Node* dragFrom = nullptr; int dragPort = 0; bool dragIsIn = false;
    juce::String kKey; int kWhich = 0, kY = 0; float kStart = 0.0f;
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
        for (auto* b : { &addBay, &menu, &send, &play, &mode }) addAndMakeVisible (b);
        mode.setTooltip ("MANUAL: program the pattern yourself.  AUTO: every sound you add gets a pattern in the song's key and scale (right-click the canvas > AUTO to regenerate)");
        mode.onClick = [this] {
            auto& b = bay();
            b.mode = 1 - b.mode; saveBays (s);
            if (b.mode == 1) { canvas->autoGen(); canvas->rebuild(); }
            refresh();
        };
        addAndMakeVisible (pick);
        pick.onChange = [this] { if (pick.getSelectedItemIndex() >= 0) { curBay() = pick.getSelectedItemIndex(); canvas->rebuild(); refresh(); } };
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
        mode.setButtonText (bay().mode == 1 ? "AUTO" : "MANUAL");
        mode.setToggleState (bay().mode == 1, juce::dontSendNotification);
    }
    void paint (juce::Graphics& g) override { g.fillAll (col::panel); }
    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop (phoneUI ? 44 : 32).reduced (4);
        pick.setBounds (bar.removeFromLeft (160)); bar.removeFromLeft (4);
        for (auto* b : { &addBay, &mode, &send, &play, &menu }) { b->setBounds (bar.removeFromLeft (phoneUI ? 70 : 84)); bar.removeFromLeft (4); }
        vp.setBounds (r);
    }
    Studio& s;
    Canvas* canvas = nullptr;
    juce::Viewport vp;
    juce::ComboBox pick;
    juce::TextButton addBay { "+ BAY" }, menu { "ADD..." }, send { "SEND" }, play { "PLAY" }, mode { "MANUAL" };
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
juce::PopupMenu patchBayRouteMenu (Studio& s, std::function<void (Project&, int)> apply)
{
    loadBays (s);
    juce::PopupMenu m;
    const auto& p = s.p();
    const auto nm = [&p] (int t) { return t > 0 && t < (int) p.mixer.size() ? "  (" + p.mixer[(size_t) t].name + ")" : juce::String(); };
    auto go = [&s, apply] (int i, int node) {
        s.edit ("Route to Patch Bay", [&] (Project& pr) { const int t = bayInsert (pr, i, node); if (t > 0) apply (pr, t); });
        saveBays (s);
    };
    for (int i = 0; i < (int) bays().size() && i < 32; ++i)
    {
        const auto& b = bays()[(size_t) i];
        juce::PopupMenu bm;
        bm.addItem ("Bay input" + nm (b.insert), [go, i] { go (i, 0); });
        for (int k = 0; k < (int) b.stacks.size(); ++k) bm.addItem ("FX stack node " + juce::String (k + 1) + nm (b.stacks[(size_t) k]), [go, i, k] { go (i, k + 1); });
        bm.addItem ("New FX stack node (own FX chain + automation, feeds " + b.name + ")", [go, i] { go (i, -1); });
        m.addSubMenu (b.name, bm);
    }
    m.addSeparator();
    m.addItem ("New Patch Bay", [go] { bays().push_back ({ "Bay " + juce::String ((int) bays().size() + 1), { -1 }, 2, 0 }); go ((int) bays().size() - 1, 0); });
    return m;
}
std::vector<BayInfo> bayInfo (Studio& s)
{
    loadBays (s);
    std::vector<BayInfo> v;
    for (const auto& b : bays()) v.push_back ({ b.name, b.chans, b.insert, b.stacks });
    return v;
}
std::unique_ptr<juce::Component> createPatchBayView (Studio& s) { return std::make_unique<PatchBayView> (s); }
}
