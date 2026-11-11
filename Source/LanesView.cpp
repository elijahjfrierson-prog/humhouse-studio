#include "UI.h"
#include "Effects.h"

namespace hhs
{

namespace
{
const juce::Colour autoRed (0xffff4b4b), ghostAmber (0xffffb532);

juce::String slotLabel (Studio& s, const FxSlot& f)
{
    if (f.type == "plugin")
    {
        auto label = f.pluginId.upToFirstOccurrenceOf ("-", false, false);
        for (auto& d : s.engine.knownPlugins.getTypes()) if (d.createIdentifierString() == f.pluginId) label = d.name;
        return label;
    }
    for (auto& inf : builtinEffects()) if (f.type == inf.id) return inf.name;
    return f.type;
}
}

// Ableton-style lanes for the selected mixer track: every insert as an FX lane (EQ curve / knobs over the track spectrum,
// G ghosts the effect so you hear the track with and without it) or its automation lanes on the song timeline
class LanesView : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    explicit LanesView (Studio& st) : s (st)
    {
        setupButton (fxBtn, "FX LANES", "Every effect on the selected mixer track as its own lane: curve, knobs and the track spectrum", true);
        setupButton (autoBtn, "AUTOMATION", "Automation lanes of the selected mixer track: drag a point, double-click to add one", true);
        fxBtn.setRadioGroupId (7101); autoBtn.setRadioGroupId (7101);
        fxBtn.setToggleState (true, juce::dontSendNotification);
        fxBtn.onClick = autoBtn.onClick = [this] { scroll = 0; repaint(); };
        setupButton (addBtn, "+ LANE", "Automate this track: volume, pan, a send or any effect knob");
        addBtn.onClick = [this] { addMenu(); };
        for (auto* b : { &fxBtn, &autoBtn, &addBtn }) addAndMakeVisible (b);
        snap.assign (fftN, 0.0f); fftBuf.assign (fftN * 2, 0.0f); spec.assign (fftN / 2, -100.0f);
        s.addChangeListener (this);
        startTimerHz (30);
    }
    ~LanesView() override { s.removeChangeListener (this); }

    void resized() override
    {
        auto top = getLocalBounds().removeFromTop (barH).reduced (4, 3);
        addBtn.setBounds (top.removeFromRight (phoneUI ? 84 : 70)); top.removeFromRight (8);
        autoBtn.setBounds (top.removeFromRight (phoneUI ? 116 : 100)); top.removeFromRight (4);
        fxBtn.setBounds (top.removeFromRight (phoneUI ? 104 : 90));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::bg);
        auto bar = getLocalBounds().removeFromTop (barH);
        g.setColour (col::panel); g.fillRect (bar);
        g.setColour (col::grid); g.drawHorizontalLine (barH - 1, 0.0f, (float) getWidth());
        auto& p = s.p(); const int tr = track();
        g.setColour (col::text); g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText ("LANES   " + (tr < (int) p.mixer.size() ? p.mixer[(size_t) tr].name : juce::String()),
                    bar.withTrimmedLeft (10).withRight (juce::jmax (10, fxBtn.getX() - 8)), juce::Justification::centredLeft, true);
        const auto rs = rows();
        g.reduceClipRegion (0, barH, getWidth(), getHeight() - barH);
        if (rs.empty())
        {
            g.setColour (col::dim); g.setFont (juce::FontOptions (13.0f));
            g.drawText (fxMode() ? "No effects on this track yet: add one in the Mixer or Plugin House"
                                 : "No automation on this track yet: right-click here or + LANE to automate volume, pan, a send or any effect knob",
                        getLocalBounds().withTrimmedTop (barH), juce::Justification::centred, true);
            return;
        }
        for (int i = 0; i < (int) rs.size(); ++i)
        {
            const auto r = laneRect (i, (int) rs.size());
            if (r.getBottom() > barH && r.getY() < getHeight()) paintLane (g, rs[(size_t) i], r);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        drag = {};
        const auto rs = rows(); const int tr = track();
        if (e.mods.isPopupMenu() && e.y > barH && (rs.empty() || fxMode() || e.x < headW || e.y >= laneRect ((int) rs.size() - 1, (int) rs.size()).getBottom()))
        { addMenu (true); return; }
        for (int i = 0; i < (int) rs.size(); ++i)
        {
            const auto r = laneRect (i, (int) rs.size());
            if (! r.contains (e.getPosition())) continue;
            const auto& row = rs[(size_t) i];
            if (row.slot >= 0)
            {
                const auto& f = s.p().mixer[(size_t) tr].fx[(size_t) row.slot];
                if (ghostBox (r).contains (e.getPosition()))
                    s.edit (f.bypass ? "Unghost effect" : "Ghost effect", [tr, slot = row.slot] (Project& p) {
                        auto& fx = p.mixer[(size_t) tr].fx; if (slot < (int) fx.size()) fx[(size_t) slot].bypass = ! fx[(size_t) slot].bypass; });
                else openEffectEditor (s, tr, f.uid);
                return;
            }
            if (e.x < headW) return;
            const auto body = bodyOf (r);
            for (int ci : row.clips)
            {
                const auto& c = s.p().clips[(size_t) ci];
                for (int k = 0; k < (int) c.autoPts.size(); ++k)
                {
                    const auto& q = c.autoPts[(size_t) k];
                    if (q.beat < c.patOffset || q.beat > c.patOffset + c.length) continue;
                    if (std::abs (beatX (c.start + q.beat - c.patOffset, body) - (float) e.x) < 7.0f && std::abs (valueY (q.value, body) - (float) e.y) < 9.0f)
                    { drag = { c.uid, k, body, false }; return; }
                }
            }
            return;
        }
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag.uid == 0) return;
        const double beat = (e.x - drag.body.getX()) / (double) juce::jmax (1, drag.body.getWidth()) * songLen();
        const float v = juce::jlimit (0.0f, 1.0f, (float) (drag.body.getBottom() - 3 - e.y) / (float) juce::jmax (1, drag.body.getHeight() - 6));
        s.live ([&] (Project& p) {
            for (auto& c : p.clips)
            {
                if (c.uid != drag.uid || drag.point >= (int) c.autoPts.size()) continue;
                auto& pts = c.autoPts;
                const double lo = drag.point > 0 ? pts[(size_t) drag.point - 1].beat : 0.0;
                const double hi = drag.point + 1 < (int) pts.size() ? pts[(size_t) drag.point + 1].beat : c.patOffset + c.length;
                pts[(size_t) drag.point].beat = juce::jlimit (lo, juce::jmax (lo, hi), c.patOffset + beat - c.start);
                pts[(size_t) drag.point].value = v;
            }
        });
        drag.moved = true;
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (drag.uid != 0 && drag.moved) s.commit ("Move automation point");
        drag = {};
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (fxMode() || e.x < headW) return;
        const auto rs = rows();
        for (int i = 0; i < (int) rs.size(); ++i)
        {
            const auto r = laneRect (i, (int) rs.size());
            if (! r.contains (e.getPosition())) continue;
            const auto body = bodyOf (r);
            const double beat = (e.x - body.getX()) / (double) juce::jmax (1, body.getWidth()) * songLen();
            const float v = juce::jlimit (0.0f, 1.0f, (float) (body.getBottom() - 3 - e.y) / (float) juce::jmax (1, body.getHeight() - 6));
            for (int ci : rs[(size_t) i].clips)
            {
                const int uid = s.p().clips[(size_t) ci].uid;
                const auto& c0 = s.p().clips[(size_t) ci];
                if (beat < c0.start || beat > c0.start + c0.length) continue;
                s.edit ("Add automation point", [uid, beat, v] (Project& p) {
                    for (auto& c : p.clips)
                        if (c.uid == uid)
                        {
                            const AutoPoint q { c.patOffset + beat - c.start, v };
                            c.autoPts.insert (std::upper_bound (c.autoPts.begin(), c.autoPts.end(), q, [] (const AutoPoint& a, const AutoPoint& b) { return a.beat < b.beat; }), q);
                        }
                });
                return;
            }
        }
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        const int n = (int) rows().size();
        const int over = juce::jmax (0, n * laneH (n) - (getHeight() - barH));
        scroll = juce::jlimit (0, over, scroll - juce::roundToInt (w.deltaY * 120.0f));
        repaint();
    }

private:
    struct Row { int slot = -1; AutoTarget t; std::vector<int> clips; };
    struct Drag { int uid = 0, point = -1; juce::Rectangle<int> body; bool moved = false; };

    bool fxMode() const { return fxBtn.getToggleState(); }
    int track() { return juce::jlimit (0, juce::jmax (0, (int) s.p().mixer.size() - 1), s.selectedTrack); }
    double songLen() { auto& p = s.p(); return juce::jmax ((double) p.beatsPerBar * 8.0, p.songLengthBeats() + p.beatsPerBar); }
    int laneH (int n) const { const int body = getHeight() - barH; return juce::jlimit (phoneUI ? 60 : 46, 120, n > 0 ? body / n : body); }
    juce::Rectangle<int> laneRect (int i, int n) const { const int h = laneH (n); return { 0, barH + i * h - scroll, getWidth(), h }; }
    static juce::Rectangle<int> bodyOf (juce::Rectangle<int> lane) { return lane.withTrimmedLeft (headW).reduced (0, 3); }
    static juce::Rectangle<int> ghostBox (juce::Rectangle<int> lane) { return lane.withWidth (headW).removeFromRight (36).withSizeKeepingCentre (26, 20); }
    float beatX (double b, juce::Rectangle<int> body) { return (float) body.getX() + (float) (b / songLen()) * (float) body.getWidth(); }
    static float valueY (float v, juce::Rectangle<int> b) { return (float) b.getBottom() - 3.0f - v * (float) (b.getHeight() - 6); }
    static float hzX (double hz, juce::Rectangle<int> b) { return (float) b.getX() + (float) (std::log (hz / 20.0) / std::log (1000.0)) * (float) b.getWidth(); }

    std::vector<Row> rows()
    {
        std::vector<Row> out;
        auto& p = s.p(); const int tr = track();
        if (tr >= (int) p.mixer.size()) return out;
        if (fxMode())
        {
            const auto& fx = p.mixer[(size_t) tr].fx;
            for (int i = 0; i < (int) fx.size(); ++i) if (fx[(size_t) i].type.isNotEmpty()) { Row r; r.slot = i; out.push_back (r); }
            return out;
        }
        for (int i = 0; i < (int) p.clips.size(); ++i)
        {
            const auto& c = p.clips[(size_t) i];
            const int k = c.autoTarget.kind;
            if (! c.isAuto() || c.autoTarget.track != tr || k == AutoChanVol || k == AutoChanPan || k == AutoChanPitch) continue;
            Row* hit = nullptr;
            for (auto& r : out) if (r.t.kind == k && r.t.fxUid == c.autoTarget.fxUid && r.t.param == c.autoTarget.param) hit = &r;
            if (hit == nullptr) { out.emplace_back(); hit = &out.back(); hit->t = c.autoTarget; }
            hit->clips.push_back (i);
        }
        return out;
    }

    BuiltinEffect* modelFor (const FxSlot& f)
    {
        if (f.type == "plugin") return nullptr;
        auto& m = models[f.uid];
        if (m == nullptr || f.type != m->typeId())
        {
            m = createBuiltinEffect (f.type);
            if (m == nullptr) return nullptr;
            m->prepare (48000.0, 512);
        }
        for (auto& d : m->params()) { auto it = f.params.find (d.id); m->setParam (d.id, it != f.params.end() ? it->second : d.def); }
        return m.get();
    }

    void paintLane (juce::Graphics& g, const Row& row, juce::Rectangle<int> r)
    {
        auto& p = s.p(); const int tr = track();
        const auto gb = ghostBox (r);
        const auto body = bodyOf (r);
        auto head = r.withWidth (headW);
        g.setColour (col::panel); g.fillRect (head);
        g.setColour (col::panel2.darker (0.3f)); g.fillRect (body);
        g.setColour (col::grid); g.drawHorizontalLine (r.getBottom() - 1, 0.0f, (float) getWidth());
        juce::String name, sub; bool ghost = false; juce::Colour c = autoRed;
        const FxSlot* f = nullptr;
        if (row.slot >= 0)
        {
            f = &p.mixer[(size_t) tr].fx[(size_t) row.slot];
            name = juce::String (row.slot + 1) + "  " + slotLabel (s, *f);
            ghost = f->bypass;
            sub = ghost ? "ghosted: hearing it off" : "mix " + juce::String (juce::roundToInt (f->mix * 100.0f)) + "%  -  click to edit";
            c = col::accent2;
        }
        else
        {
            name = row.t.name.isNotEmpty() ? row.t.name : juce::String ("Automation");
            sub = juce::String ((int) row.clips.size()) + (row.clips.size() == 1 ? " clip" : " clips");
        }
        g.setColour (c); g.fillRect (head.removeFromLeft (4));
        auto hb = head.reduced (8, 4);
        if (f != nullptr)
        {
            hb.removeFromRight (36);
            g.setColour (ghost ? ghostAmber : col::panel2); g.fillRoundedRectangle (gb.toFloat(), 3.0f);
            g.setColour (ghost ? juce::Colours::black : col::text); g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("G", gb, juce::Justification::centred, false);
        }
        g.setColour (ghost ? col::dim : col::text); g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (name, hb.removeFromTop (hb.getHeight() / 2), juce::Justification::bottomLeft, true);
        g.setColour (col::dim); g.setFont (juce::FontOptions (11.0f));
        g.drawText (sub, hb, juce::Justification::topLeft, true);
        if (f != nullptr) paintFx (g, *f, body, ghost);
        else paintAuto (g, row, body);
    }

    void paintFx (juce::Graphics& g, const FxSlot& f, juce::Rectangle<int> b, bool ghost)
    {
        g.setColour (col::grid);
        for (double hz : { 100.0, 1000.0, 10000.0 }) g.drawVerticalLine ((int) hzX (hz, b), (float) b.getY(), (float) b.getBottom());
        g.drawHorizontalLine (b.getCentreY(), (float) b.getX(), (float) b.getRight());
        if (rate > 0 && b.getWidth() > 4)
        {
            juce::Path sp; sp.startNewSubPath ((float) b.getX(), (float) b.getBottom());
            for (int x = b.getX(); x <= b.getRight(); x += 2)
            {
                const double hz = 20.0 * std::pow (1000.0, (x - b.getX()) / (double) b.getWidth());
                const int bin = juce::jlimit (1, fftN / 2 - 1, (int) (hz * fftN / rate));
                const float db = juce::jlimit (-90.0f, 0.0f, spec[(size_t) bin]);
                sp.lineTo ((float) x, (float) b.getBottom() - (db + 90.0f) / 90.0f * (float) b.getHeight());
            }
            sp.lineTo ((float) b.getRight(), (float) b.getBottom()); sp.closeSubPath();
            g.setColour (col::text.withAlpha (0.16f)); g.fillPath (sp);
        }
        if (auto* m = modelFor (f))
        {
            if (f.type.containsIgnoreCase ("eq"))
            {
                juce::Path cv;
                for (int x = b.getX(); x <= b.getRight(); x += 3)
                {
                    const double hz = 20.0 * std::pow (1000.0, (x - b.getX()) / (double) b.getWidth());
                    const float y = (float) b.getCentreY() - juce::jlimit (-24.0f, 24.0f, m->responseDb (hz)) / 24.0f * (float) b.getHeight() * 0.5f;
                    if (x == b.getX()) cv.startNewSubPath ((float) x, y); else cv.lineTo ((float) x, y);
                }
                g.setColour (col::accent2); g.strokePath (cv, juce::PathStrokeType (1.6f));
            }
            else
            {   // knob strip: each parameter as a small value bar, like a device collapsed into a lane
                const auto& ps = m->params();
                const int cw = 104, n = juce::jmin ((int) ps.size(), juce::jmax (1, b.getWidth() / cw));
                auto row = b.reduced (6, juce::jmax (2, (b.getHeight() - 30) / 2));
                for (int i = 0; i < n; ++i)
                {
                    auto cell = row.removeFromLeft (cw).reduced (4, 0);
                    const auto& d = ps[(size_t) i];
                    auto it = f.params.find (d.id);
                    const float v = it != f.params.end() ? it->second : d.def;
                    const float t = juce::jlimit (0.0f, 1.0f, (v - d.min) / juce::jmax (1.0e-6f, d.max - d.min));
                    g.setColour (col::text.withAlpha (0.85f)); g.setFont (juce::FontOptions (10.5f));
                    g.drawText (d.name, cell.removeFromTop (14), juce::Justification::centredLeft, true);
                    auto bar = cell.removeFromTop (8).toFloat();
                    g.setColour (col::panel); g.fillRoundedRectangle (bar, 2.0f);
                    g.setColour (col::accent); g.fillRoundedRectangle (bar.withWidth (juce::jmax (2.0f, bar.getWidth() * t)), 2.0f);
                }
            }
        }
        if (ghost)
        {
            g.setColour (juce::Colours::black.withAlpha (0.4f)); g.fillRect (b);
            g.setColour (ghostAmber); g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText ("GHOSTED - you hear the track without it (click G)", b, juce::Justification::centred, true);
        }
    }

    void paintAuto (juce::Graphics& g, const Row& row, juce::Rectangle<int> b)
    {
        auto& p = s.p();
        const double len = songLen();
        const int bpb = juce::jmax (1, p.beatsPerBar);
        const int step = len / bpb > 64 ? 4 : 1;
        g.setColour (col::grid);
        for (int bar = 0; bar * (double) bpb < len; bar += step) g.drawVerticalLine ((int) beatX (bar * (double) bpb, b), (float) b.getY(), (float) b.getBottom());
        for (int ci : row.clips)
        {
            const auto& c = p.clips[(size_t) ci];
            const float x0 = beatX (c.start, b), x1 = beatX (c.start + c.length, b);
            if (x1 - x0 < 1.0f) continue;
            g.setColour (autoRed.withAlpha (c.muted ? 0.03f : 0.09f)); g.fillRect (juce::Rectangle<float> (x0, (float) b.getY(), x1 - x0, (float) b.getHeight()));
            juce::Path line;
            for (float x = x0;; x += 2.0f)
            {
                const float xx = juce::jmin (x, x1);
                const float y = valueY (autoValueAt (c.autoPts, c.patOffset + (xx - x0) / (x1 - x0) * c.length), b);
                if (xx == x0) line.startNewSubPath (xx, y); else line.lineTo (xx, y);
                if (xx >= x1) break;
            }
            g.setColour (c.muted ? col::dim : autoRed); g.strokePath (line, juce::PathStrokeType (1.5f));
            for (auto& q : c.autoPts)
            {
                if (q.beat < c.patOffset || q.beat > c.patOffset + c.length) continue;
                const float x = beatX (c.start + q.beat - c.patOffset, b), y = valueY (q.value, b);
                g.fillRect (x - 2.5f, y - 2.5f, 5.0f, 5.0f);
            }
        }
        const float ph = beatX (s.engine.positionBeats(), b);
        g.setColour (col::text.withAlpha (0.7f)); g.drawVerticalLine ((int) ph, (float) b.getY(), (float) b.getBottom());
        paintMiniPlugin (g, row, b);
    }

    // automation as a mini plug-in: the effect's own look (EQ curve / knob) moving with the automated value at the playhead
    void paintMiniPlugin (juce::Graphics& g, const Row& row, juce::Rectangle<int> b)
    {
        if (row.t.kind != AutoFxParam || b.getWidth() < 260 || b.getHeight() < 30) return;
        auto& p = s.p(); const int tr = track();
        const FxSlot* f = nullptr;
        for (auto& x : p.mixer[(size_t) tr].fx) if (x.uid == row.t.fxUid) f = &x;
        if (f == nullptr) return;
        const double pos = s.engine.positionBeats();
        float v = -1.0f;
        for (int ci : row.clips)
        {
            const auto& cl = p.clips[(size_t) ci];
            if (! cl.muted && pos >= cl.start && pos < cl.start + cl.length) v = autoValueAt (cl.autoPts, cl.patOffset + pos - cl.start);
        }
        auto card = b.withWidth (juce::jmin (220, b.getWidth() / 3)).withX (b.getRight() - juce::jmin (220, b.getWidth() / 3)).reduced (2).toFloat();
        g.setColour (col::panel.withAlpha (0.94f)); g.fillRoundedRectangle (card, 5.0f);
        g.setColour (col::accent2.withAlpha (0.8f)); g.drawRoundedRectangle (card, 5.0f, 1.2f);
        auto in = card.reduced (6.0f, 3.0f);
        g.setColour (col::text); g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawText (slotLabel (s, *f) + "  " + row.t.param, in.removeFromTop (12.0f), juce::Justification::centredLeft, true);
        auto* m = modelFor (*f);
        const ParamDef* d = nullptr;
        if (m != nullptr) for (auto& q : m->params()) if (row.t.param == q.id) d = &q;
        const float t = v >= 0.0f ? v : (d != nullptr ? juce::jlimit (0.0f, 1.0f, ((f->params.count (d->id) ? f->params.at (d->id) : d->def) - d->min) / juce::jmax (1.0e-6f, d->max - d->min)) : 0.5f);
        auto knob = in.removeFromLeft (juce::jmin (in.getHeight(), 40.0f)).reduced (2.0f);
        const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = a0 + juce::MathConstants<float>::pi * 1.5f, ang = a0 + (a1 - a0) * t;
        const auto cx = knob.getCentreX(), cy = knob.getCentreY(), rad = juce::jmin (knob.getWidth(), knob.getHeight()) * 0.5f - 1.0f;
        juce::Path arc; arc.addCentredArc (cx, cy, rad, rad, 0.0f, a0, a1, true);
        g.setColour (col::grid); g.strokePath (arc, juce::PathStrokeType (3.0f));
        juce::Path on; on.addCentredArc (cx, cy, rad, rad, 0.0f, a0, ang, true);
        g.setColour (v >= 0.0f ? autoRed : col::accent); g.strokePath (on, juce::PathStrokeType (3.0f));
        g.drawLine (cx, cy, cx + std::sin (ang) * rad * 0.8f, cy - std::cos (ang) * rad * 0.8f, 2.0f);
        in.removeFromLeft (4.0f);
        if (m != nullptr && d != nullptr && f->type.containsIgnoreCase ("eq") && in.getWidth() > 30.0f)
        {   // the EQ curve with this knob at its automated value: the band visibly moves while it plays
            m->setParam (d->id, d->min + t * (d->max - d->min));
            juce::Path cv;
            const auto r = in.toNearestInt();
            for (int x = r.getX(); x <= r.getRight(); x += 3)
            {
                const double hz = 20.0 * std::pow (1000.0, (x - r.getX()) / (double) juce::jmax (1, r.getWidth()));
                const float y = (float) r.getCentreY() - juce::jlimit (-24.0f, 24.0f, m->responseDb (hz)) / 24.0f * (float) r.getHeight() * 0.5f;
                if (x == r.getX()) cv.startNewSubPath ((float) x, y); else cv.lineTo ((float) x, y);
            }
            g.setColour (col::accent2); g.strokePath (cv, juce::PathStrokeType (1.4f));
        }
        else
        {   // value meter + readout
            auto meter = in.removeFromTop (juce::jmin (10.0f, in.getHeight() * 0.4f));
            g.setColour (col::bg); g.fillRoundedRectangle (meter, 2.0f);
            g.setColour (v >= 0.0f ? autoRed : col::accent); g.fillRoundedRectangle (meter.withWidth (juce::jmax (2.0f, meter.getWidth() * t)), 2.0f);
            g.setColour (col::dim); g.setFont (juce::FontOptions (10.0f));
            const juce::String val = d != nullptr ? juce::String (d->min + t * (d->max - d->min), 2) : juce::String (juce::roundToInt (t * 100.0f)) + "%";
            g.drawText ((d != nullptr ? juce::String (d->name) + "  " : juce::String()) + val + (v >= 0.0f ? "  (automated)" : ""), in, juce::Justification::centredLeft, true);
        }
    }

    void addMenu (bool atMouse = false)
    {
        auto& p = s.p(); const int tr = track();
        if (tr >= (int) p.mixer.size()) return;
        const auto& mt = p.mixer[(size_t) tr];
        std::vector<AutoTarget> ts;
        juce::PopupMenu m, routed;
        for (int i = 0; i < (int) p.mixer.size(); ++i)   // routed track: any mixer track / bus; the lanes dock to it
            routed.addItem (100000 + i, p.mixer[(size_t) i].name.isNotEmpty() ? p.mixer[(size_t) i].name : (i == 0 ? juce::String ("Master") : "Insert " + juce::String (i)), true, i == tr);
        m.addSubMenu ("Routed track: " + mt.name, routed);
        m.addSeparator();
        auto add = [&] (juce::PopupMenu& menu, AutoTarget t, const juce::String& label) { ts.push_back (t); menu.addItem ((int) ts.size(), label); };
        AutoTarget t; t.track = tr;
        t.kind = AutoInsertVol; t.name = mt.name + " - Volume"; add (m, t, "Volume");
        t.kind = AutoInsertPan; t.name = mt.name + " - Pan"; add (m, t, "Pan");
        for (auto& sd : mt.sends)
            if (sd.target >= 0 && sd.target < (int) p.mixer.size())
            {
                AutoTarget st; st.track = tr; st.kind = AutoSendLevel; st.param = juce::String (sd.target);
                st.name = mt.name + " - Send to " + p.mixer[(size_t) sd.target].name;
                add (m, st, "Send to " + p.mixer[(size_t) sd.target].name);
            }
        for (auto& f : mt.fx)
            if (auto* fm = modelFor (f))
            {
                juce::PopupMenu sub;
                const auto label = slotLabel (s, f);
                for (auto& d : fm->params())
                {
                    AutoTarget ft; ft.track = tr; ft.kind = AutoFxParam; ft.fxUid = f.uid; ft.param = d.id; ft.name = label + " - " + d.name;
                    add (sub, ft, d.name);
                }
                m.addSubMenu (label, sub);
            }
        int below = (int) p.lanes.size() - 1;
        for (int i = 0; i < (int) p.lanes.size(); ++i) if (p.lanes[(size_t) i].mixerTrack == tr) { below = i; break; }
        m.showMenuAsync (atMouse ? juce::PopupMenu::Options().withMousePosition() : juce::PopupMenu::Options().withTargetComponent (&addBtn),
                         [safe = juce::Component::SafePointer<LanesView> (this), ts, below, atMouse] (int r)
                         {
                             if (safe != nullptr && r >= 100000) { safe->s.selectedTrack = r - 100000; safe->s.sendChangeMessage(); safe->repaint(); safe->addMenu (atMouse); return; }
                             if (safe == nullptr || r <= 0 || r > (int) ts.size()) return;
                             createAutomationClip (safe->s, ts[(size_t) r - 1], below);
                             safe->autoBtn.setToggleState (true, juce::sendNotification);
                         });
    }

    void timerCallback() override
    {
        if (! isShowing()) return;
        const auto area = getLocalBounds().withTrimmedTop (barH).withTrimmedLeft (headW);
        if (! fxMode()) { if (s.engine.isPlaying()) repaint (area); return; }
        auto* t = s.engine.trackScope (track());
        if (t == nullptr) return;
        rate = s.engine.getSampleRate();
        t->read (snap.data(), fftN, t->heard());
        float peak = 0.0f;
        for (int i = 0; i < fftN; ++i)
        {
            peak = juce::jmax (peak, std::abs (snap[(size_t) i]));
            fftBuf[(size_t) i] = snap[(size_t) i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftN));
        }
        if (peak < 1.0e-5f && quiet) return;   // silent and already drawn silent: no repaint
        quiet = peak < 1.0e-5f;
        std::fill (fftBuf.begin() + fftN, fftBuf.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (fftBuf.data());
        for (int i = 0; i < fftN / 2; ++i)
        {
            const float db = juce::Decibels::gainToDecibels (fftBuf[(size_t) i] / (float) (fftN / 4), -100.0f);
            spec[(size_t) i] = db > spec[(size_t) i] ? db : spec[(size_t) i] * 0.85f + db * 0.15f;
        }
        repaint (area);
    }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

    static constexpr int barH = phoneUI ? 36 : 30, headW = phoneUI ? 150 : 180;
    static constexpr int fftOrder = 11, fftN = 1 << fftOrder;
    Studio& s;
    juce::TextButton fxBtn, autoBtn, addBtn;
    std::map<int, std::unique_ptr<BuiltinEffect>> models;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> snap, fftBuf, spec;
    double rate = 0;
    bool quiet = false;
    int scroll = 0;
    Drag drag;
};

std::unique_ptr<juce::Component> createLanes (Studio& s) { return std::make_unique<LanesView> (s); }
}
