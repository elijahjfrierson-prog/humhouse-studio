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
        setupButton (autoBtn, "AUTOMATION", "Every automation as a live mini plug-in: click to open its effect, right-click for mute / delete / add", true);
        fxBtn.setRadioGroupId (7101); autoBtn.setRadioGroupId (7101);
        fxBtn.setToggleState (true, juce::dontSendNotification);
        fxBtn.onClick = autoBtn.onClick = [this] { scroll = 0; laneKey = {}; refill(); repaint(); };
        setupButton (addBtn, "+ LANE", "Automate this track: volume, pan, a send or any effect knob");
        addBtn.onClick = [this] { addMenu(); };
        for (auto* b : { &fxBtn, &autoBtn, &addBtn }) addAndMakeVisible (b);
        laneBox.setTooltip ("LANE: one automation / effect, or all of them");
        trackBox.setTooltip ("TRACK: only what sits on this Playlist track");
        mixBox.setTooltip ("MIXER: only this mixer insert's effects and automation");
        bayBox.setTooltip ("BAY: only this Patch Bay's sounds, insert and FX stacks");
        for (auto* c : { &laneBox, &trackBox, &mixBox, &bayBox })
        {
            addAndMakeVisible (c);
            c->onChange = [this, c] { pick (*c); };
        }
        snap.assign (fftN, 0.0f); fftBuf.assign (fftN * 2, 0.0f); spec.assign (fftN / 2, -100.0f);
        s.addChangeListener (this);
        refill();
        startTimerHz (30);
    }
    ~LanesView() override { s.removeChangeListener (this); }

    void resized() override
    {
        auto top = getLocalBounds().removeFromTop (barH).reduced (4, 3);
        addBtn.setBounds (top.removeFromRight (phoneUI ? 84 : 70)); top.removeFromRight (8);
        autoBtn.setBounds (top.removeFromRight (phoneUI ? 116 : 100)); top.removeFromRight (4);
        fxBtn.setBounds (top.removeFromRight (phoneUI ? 104 : 90)); top.removeFromRight (8);
        top.removeFromLeft (phoneUI ? 64 : 58);
        const int w = juce::jmin (180, top.getWidth() / 4);
        for (auto* c : { &laneBox, &trackBox, &mixBox, &bayBox }) { c->setBounds (top.removeFromLeft (w).reduced (2, 0)); }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::bg);
        auto bar = getLocalBounds().removeFromTop (barH);
        g.setColour (col::panel); g.fillRect (bar);
        g.setColour (col::grid); g.drawHorizontalLine (barH - 1, 0.0f, (float) getWidth());
        g.setColour (col::text); g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        fitText (g, "LANES", bar.withTrimmedLeft (8).withWidth (phoneUI ? 58 : 52), juce::Justification::centredLeft, false);
        const auto rs = rows();
        g.reduceClipRegion (0, barH, getWidth(), getHeight() - barH);
        if (rs.empty())
        {
            g.setColour (col::dim); g.setFont (juce::FontOptions (13.0f));
            fitText (g, fxMode() ? "No effects here yet: add one in the Mixer or Plugin House"
                                 : "No automation here yet: right-click or + LANE to automate volume, pan, a send or any effect knob",
                        getLocalBounds().withTrimmedTop (barH).reduced (12, 0), juce::Justification::centred, true);
            return;
        }
        for (int i = 0; i < (int) rs.size(); ++i)
        {
            const auto r = fxMode() ? laneRect (i, (int) rs.size()) : cardRect (i);
            if (r.getBottom() <= barH || r.getY() >= getHeight()) continue;
            if (fxMode()) paintLane (g, rs[(size_t) i], r); else paintMiniPlugin (g, rs[(size_t) i], r);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.y <= barH) return;
        const auto rs = rows(); const int tr = track();
        if (! fxMode())
        {
            const int i = cardAt (e.getPosition(), (int) rs.size());
            if (i < 0) { if (e.mods.isPopupMenu()) addMenu (true); return; }
            if (e.mods.isPopupMenu()) cardMenu (rs[(size_t) i]); else openCard (rs[(size_t) i]);
            return;
        }
        if (e.mods.isPopupMenu()) { addMenu (true); return; }
        for (int i = 0; i < (int) rs.size(); ++i)
        {
            const auto r = laneRect (i, (int) rs.size());
            if (! r.contains (e.getPosition()) || rs[(size_t) i].slot < 0) continue;
            const int slot = rs[(size_t) i].slot;
            const auto& f = s.p().mixer[(size_t) tr].fx[(size_t) slot];
            if (ghostBox (r).contains (e.getPosition()))
                s.edit (f.bypass ? "Unghost effect" : "Ghost effect", [tr, slot] (Project& p) {
                    auto& fx = p.mixer[(size_t) tr].fx; if (slot < (int) fx.size()) fx[(size_t) slot].bypass = ! fx[(size_t) slot].bypass; });
            else openEffectEditor (s, tr, f.uid);
            return;
        }
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (fxMode() || e.y <= barH) return;
        const auto rs = rows();
        if (const int i = cardAt (e.getPosition(), (int) rs.size()); i >= 0) openCard (rs[(size_t) i]);
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        const int n = (int) rows().size();
        const int over = juce::jmax (0, contentH (n) - (getHeight() - barH));
        scroll = juce::jlimit (0, over, scroll - juce::roundToInt (w.deltaY * 120.0f));
        repaint();
    }
private:
    struct Row { int slot = -1; AutoTarget t; std::vector<int> clips; };
    bool fxMode() const { return fxBtn.getToggleState(); }
    // the mixer track the FX lanes show: MIXER pick, else the TRACK pick's insert, else the BAY's insert, else the selected track
    int track()
    {
        auto& p = s.p(); int t = s.selectedTrack;
        if (fMix >= 0) t = fMix;
        else if (fTrack >= 0 && fTrack < (int) p.lanes.size()) t = p.lanes[(size_t) fTrack].mixerTrack;
        else if (fBay >= 0) { const auto b = bayInfo (s); if (fBay < (int) b.size()) t = b[(size_t) fBay].insert; }
        return juce::jlimit (0, juce::jmax (0, (int) p.mixer.size() - 1), t);
    }
    int laneH (int n) const { const int body = getHeight() - barH; return juce::jlimit (phoneUI ? 60 : 46, 120, n > 0 ? body / n : body); }
    juce::Rectangle<int> laneRect (int i, int n) const { const int h = laneH (n); return { 0, barH + i * h - scroll, getWidth(), h }; }
    static juce::Rectangle<int> bodyOf (juce::Rectangle<int> lane) { return lane.withTrimmedLeft (headW).reduced (0, 3); }
    static juce::Rectangle<int> ghostBox (juce::Rectangle<int> lane) { return lane.withWidth (headW).removeFromRight (36).withSizeKeepingCentre (26, 20); }
    static float hzX (double hz, juce::Rectangle<int> b) { return (float) b.getX() + (float) (std::log (hz / 20.0) / std::log (1000.0)) * (float) b.getWidth(); }
    // automation view: mini plug-ins lined up side by side, wrapping into rows
    int cols() const { return juce::jmax (1, getWidth() / (phoneUI ? 190 : 250)); }
    juce::Rectangle<int> cardRect (int i) const { const int c = cols(), w = getWidth() / c; return { (i % c) * w, barH + 4 + (i / c) * cardH - scroll, w, cardH }; }
    int cardAt (juce::Point<int> pt, int n) const { for (int i = 0; i < n; ++i) if (cardRect (i).contains (pt)) return i; return -1; }
    int contentH (int n) const { return fxMode() ? n * laneH (n) : (n + cols() - 1) / cols() * cardH + 8; }

    static juce::String keyOf (const AutoTarget& a) { return juce::String (a.kind) + "|" + juce::String (a.track) + "|" + juce::String (a.channel) + "|" + juce::String (a.fxUid) + "|" + a.param; }
    juce::String mixName (int i) { auto& m = s.p().mixer; return i < (int) m.size() && m[(size_t) i].name.isNotEmpty() ? m[(size_t) i].name : (i == 0 ? juce::String ("Master") : "Insert " + juce::String (i)); }
    // an automation belongs to mixer track tr: its insert / FX / sends, or a sound routed into it
    bool owns (const AutoTarget& a, int tr)
    {
        if (a.kind == AutoChanVol || a.kind == AutoChanPan || a.kind == AutoChanPitch)
        {
            const auto* ch = s.p().findChannel (a.channel);
            return ch != nullptr && (ch->mixerTrack == tr || (ch->mixerTrack <= 0 && tr == 0));
        }
        return a.track == tr;
    }
    static void fillBox (juce::ComboBox& c, const juce::String& all, const juce::StringArray& items, int sel)
    {
        juce::StringArray want (all), now; want.addArray (items);
        for (int i = 0; i < c.getNumItems(); ++i) now.add (c.getItemText (i));
        if (now != want) { c.clear (juce::dontSendNotification); for (int i = 0; i < want.size(); ++i) c.addItem (want[i], i + 1); }
        c.setSelectedId (sel >= 0 && sel < items.size() ? sel + 2 : 1, juce::dontSendNotification);
    }
    void refill()
    {
        auto& p = s.p();
        juce::StringArray ln, tk, mx, by;
        laneKeys.clear();
        if (fxMode())
        {
            for (auto& f : p.mixer.empty() ? std::vector<FxSlot>() : p.mixer[(size_t) track()].fx)
                if (f.type.isNotEmpty()) { laneKeys.add ("fx|" + juce::String (f.uid)); ln.add (slotLabel (s, f)); }
        }
        else
            for (auto& c : p.clips)
                if (c.isAuto() && ! laneKeys.contains (keyOf (c.autoTarget)))
                { laneKeys.add (keyOf (c.autoTarget)); ln.add (c.autoTarget.name.isNotEmpty() ? c.autoTarget.name : "Automation " + juce::String (ln.size() + 1)); }
        for (int i = 0; i < (int) p.lanes.size(); ++i) tk.add (juce::String (i + 1) + "  " + p.lanes[(size_t) i].name);
        for (int i = 0; i < (int) p.mixer.size(); ++i) mx.add (mixName (i));
        for (auto& b : bayInfo (s)) by.add (b.name);
        if (! laneKeys.contains (laneKey)) laneKey = {};
        if (fTrack >= tk.size()) fTrack = -1;
        if (fMix >= mx.size()) fMix = -1;
        if (fBay >= by.size()) fBay = -1;
        fillBox (laneBox, fxMode() ? "All effects" : "All automation", ln, laneKeys.indexOf (laneKey));
        fillBox (trackBox, "All tracks", tk, fTrack);
        fillBox (mixBox, "All mixer", mx, fMix);
        fillBox (bayBox, "All bays", by, fBay);
    }
    void pick (juce::ComboBox& c)
    {
        const int i = c.getSelectedId() - 2;
        if (&c == &laneBox) laneKey = i >= 0 && i < laneKeys.size() ? laneKeys[i] : juce::String();
        else if (&c == &trackBox) fTrack = i;
        else if (&c == &mixBox) fMix = i;
        else fBay = i;
        scroll = 0; refill(); repaint();
    }

    int fxTrackOf (const AutoTarget& a)
    {
        auto& p = s.p();
        for (int tr : { a.track, track() })
            if (tr >= 0 && tr < (int) p.mixer.size()) for (auto& x : p.mixer[(size_t) tr].fx) if (x.uid == a.fxUid) return tr;
        for (int tr = 0; tr < (int) p.mixer.size(); ++tr) for (auto& x : p.mixer[(size_t) tr].fx) if (x.uid == a.fxUid) return tr;
        return -1;
    }
    void openCard (const Row& row)
    {
        if (row.t.kind == AutoFxParam) { if (const int tr = fxTrackOf (row.t); tr >= 0) openEffectEditor (s, tr, row.t.fxUid); return; }
        int tr = row.t.track;
        if (const auto* ch = s.p().findChannel (row.t.channel); ch != nullptr && row.t.kind >= AutoChanVol && row.t.kind <= AutoChanPitch) tr = ch->mixerTrack;
        if (tr >= 0 && tr < (int) s.p().mixer.size()) { s.selectedTrack = tr; s.sendChangeMessage(); }
    }
    void cardMenu (const Row& row)
    {
        std::vector<int> uids; bool anyOn = false;
        for (int ci : row.clips) { uids.push_back (s.p().clips[(size_t) ci].uid); anyOn = anyOn || ! s.p().clips[(size_t) ci].muted; }
        juce::PopupMenu m;
        m.addSectionHeader (row.t.name.isNotEmpty() ? row.t.name : juce::String ("Automation"));
        if (row.t.kind == AutoFxParam) m.addItem (1, "Open effect");
        m.addItem (2, anyOn ? "Mute automation" : "Unmute automation");
        m.addItem (3, "Delete automation (" + juce::String ((int) uids.size()) + (uids.size() == 1 ? " clip)" : " clips)"));
        m.addSeparator();
        m.addItem (4, "Add automation lane...");
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                         [safe = juce::Component::SafePointer<LanesView> (this), row, uids, anyOn] (int r)
                         {
                             if (safe == nullptr) return;
                             auto has = [uids] (int u) { return std::find (uids.begin(), uids.end(), u) != uids.end(); };
                             if (r == 1) safe->openCard (row);
                             else if (r == 2) safe->s.edit (anyOn ? "Mute automation" : "Unmute automation", [has, anyOn] (Project& p) { for (auto& c : p.clips) if (has (c.uid)) c.muted = anyOn; });
                             else if (r == 3) safe->s.edit ("Delete automation", [has] (Project& p) { p.clips.erase (std::remove_if (p.clips.begin(), p.clips.end(), [&] (const Clip& c) { return has (c.uid); }), p.clips.end()); });
                             else if (r == 4) safe->addMenu (true);
                         });
    }

    std::vector<Row> rows()
    {
        std::vector<Row> out;
        auto& p = s.p(); const int tr = track();
        if (fxMode())
        {
            if (tr >= (int) p.mixer.size()) return out;
            const auto& fx = p.mixer[(size_t) tr].fx;
            for (int i = 0; i < (int) fx.size(); ++i)
                if (fx[(size_t) i].type.isNotEmpty() && (laneKey.isEmpty() || laneKey == "fx|" + juce::String (fx[(size_t) i].uid))) { Row r; r.slot = i; out.push_back (r); }
            return out;
        }
        // every automation clip in the song (old .hhs files too) becomes one mini plug-in; LANE / TRACK / MIXER / BAY narrow it down
        const auto bi = bayInfo (s);
        const auto pass = [&] (const Clip& c) {
            const auto& a = c.autoTarget;
            if (laneKey.isNotEmpty() && keyOf (a) != laneKey) return false;
            if (fMix >= 0 && ! owns (a, fMix)) return false;
            if (fTrack >= 0 && fTrack < (int) p.lanes.size() && c.lane != fTrack && ! owns (a, p.lanes[(size_t) fTrack].mixerTrack)) return false;
            if (fBay >= 0 && fBay < (int) bi.size())
            {
                const auto& b = bi[(size_t) fBay];
                bool hit = owns (a, b.insert);
                for (int st : b.stacks) hit = hit || owns (a, st);
                if (a.kind == AutoChanVol || a.kind == AutoChanPan || a.kind == AutoChanPitch)
                    hit = hit || b.chans.empty() || std::find (b.chans.begin(), b.chans.end(), a.channel) != b.chans.end();
                if (! hit) return false;
            }
            return true;
        };
        for (int i = 0; i < (int) p.clips.size(); ++i)
        {
            const auto& c = p.clips[(size_t) i];
            if (! c.isAuto() || ! pass (c)) continue;
            const auto k = keyOf (c.autoTarget);
            Row* hit = nullptr;
            for (auto& r : out) if (keyOf (r.t) == k) hit = &r;
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
            fitText (g, "G", gb, juce::Justification::centred, false);
        }
        g.setColour (ghost ? col::dim : col::text); g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        fitText (g, name, hb.removeFromTop (hb.getHeight() / 2), juce::Justification::bottomLeft, true);
        g.setColour (col::dim); g.setFont (juce::FontOptions (11.0f));
        fitText (g, sub, hb, juce::Justification::topLeft, true);
        if (f != nullptr) paintFx (g, *f, body, ghost);
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
                    fitText (g, d.name, cell.removeFromTop (14), juce::Justification::centredLeft, true);
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
            fitText (g, "GHOSTED - you hear the track without it (click G)", b, juce::Justification::centred, true);
        }
    }

    // automation as a mini plug-in: the effect's own look (EQ curve / knob) moving with the automated value at the playhead
    // volume / pan / pitch / send lanes: their current mixer value as 0..1, like the knob it moves
    float mixValue (const AutoTarget& a)
    {
        auto& p = s.p();
        if (a.kind == AutoChanVol || a.kind == AutoChanPan || a.kind == AutoChanPitch)
        {
            if (auto* c = p.findChannel (a.channel)) return autoFromValue (a.kind, a.kind == AutoChanVol ? c->volume : a.kind == AutoChanPan ? c->pan : c->pitch);
            return 0.5f;
        }
        if (a.track < 0 || a.track >= (int) p.mixer.size()) return 0.5f;
        auto& t = p.mixer[(size_t) a.track];
        return a.kind == AutoInsertVol ? autoFromValue (a.kind, t.volume) : a.kind == AutoInsertPan ? autoFromValue (a.kind, t.pan) : 0.5f;
    }
    void paintMiniPlugin (juce::Graphics& g, const Row& row, juce::Rectangle<int> b)
    {
        if (row.t.kind == AutoNone || b.getWidth() < 60 || b.getHeight() < 30) return;
        auto& p = s.p();
        const FxSlot* f = nullptr;
        if (row.t.kind == AutoFxParam)   // the clip's own track (old projects keep FX automation on other inserts), else any insert holding that FX
        {
            for (int tr : { row.t.track, track() })
                if (f == nullptr && tr >= 0 && tr < (int) p.mixer.size())
                    for (auto& x : p.mixer[(size_t) tr].fx) if (x.uid == row.t.fxUid) f = &x;
            for (auto& t : p.mixer) for (auto& x : t.fx) if (f == nullptr && x.uid == row.t.fxUid) f = &x;
        }
        const double pos = s.engine.positionBeats();
        float v = -1.0f;
        for (int ci : row.clips)
        {
            const auto& cl = p.clips[(size_t) ci];
            if (! cl.muted && pos >= cl.start && pos < cl.start + cl.length) v = autoValueAt (cl.autoPts, cl.patOffset + pos - cl.start);
        }
        bool muted = true; for (int ci : row.clips) muted = muted && p.clips[(size_t) ci].muted;
        auto card = b.reduced (4).toFloat();
        g.setColour (col::panel); g.fillRoundedRectangle (card, 6.0f);
        g.setColour (v >= 0.0f ? autoRed : col::accent2.withAlpha (0.8f)); g.drawRoundedRectangle (card, 6.0f, v >= 0.0f ? 2.0f : 1.2f);
        auto in = card.reduced (8.0f, 5.0f);
        g.setColour (muted ? col::dim : col::text); g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        fitText (g, f != nullptr ? slotLabel (s, *f) : row.t.name.isNotEmpty() ? row.t.name : juce::String ("Automation"), in.removeFromTop (15.0f), juce::Justification::centredLeft, false);
        g.setColour (col::dim); g.setFont (juce::FontOptions (10.5f));
        const int otr = row.t.kind == AutoFxParam ? fxTrackOf (row.t) : row.t.track;
        fitText (g, (otr >= 0 && otr < (int) p.mixer.size() && row.t.kind != AutoChanVol && row.t.kind != AutoChanPan && row.t.kind != AutoChanPitch ? mixName (otr) + "  -  " : juce::String())
                    + juce::String ((int) row.clips.size()) + (row.clips.size() == 1 ? " clip" : " clips") + (muted ? "  -  muted" : v >= 0.0f ? "  -  LIVE" : ""),
                 in.removeFromTop (13.0f), juce::Justification::centredLeft, false);
        in.removeFromTop (3.0f);
        auto* m = f != nullptr ? modelFor (*f) : nullptr;
        const ParamDef* d = nullptr;
        if (m != nullptr) for (auto& q : m->params()) if (row.t.param == q.id) d = &q;
        const float t = v >= 0.0f ? v : f == nullptr ? mixValue (row.t) : (d != nullptr ? juce::jlimit (0.0f, 1.0f, ((f->params.count (d->id) ? f->params.at (d->id) : d->def) - d->min) / juce::jmax (1.0e-6f, d->max - d->min)) : 0.5f);
        auto knob = in.removeFromLeft (juce::jmin (in.getHeight(), 54.0f)).reduced (2.0f);
        const float a0 = juce::MathConstants<float>::pi * 1.25f, a1 = a0 + juce::MathConstants<float>::pi * 1.5f, ang = a0 + (a1 - a0) * t;
        const auto cx = knob.getCentreX(), cy = knob.getCentreY(), rad = juce::jmin (knob.getWidth(), knob.getHeight()) * 0.5f - 1.0f;
        juce::Path arc; arc.addCentredArc (cx, cy, rad, rad, 0.0f, a0, a1, true);
        g.setColour (col::grid); g.strokePath (arc, juce::PathStrokeType (3.0f));
        juce::Path on; on.addCentredArc (cx, cy, rad, rad, 0.0f, a0, ang, true);
        g.setColour (v >= 0.0f ? autoRed : col::accent); g.strokePath (on, juce::PathStrokeType (3.0f));
        g.drawLine (cx, cy, cx + std::sin (ang) * rad * 0.8f, cy - std::cos (ang) * rad * 0.8f, 2.0f);
        in.removeFromLeft (4.0f);
        if (f != nullptr && m != nullptr && d != nullptr && f->type.containsIgnoreCase ("eq") && in.getWidth() > 30.0f)
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
            const bool bip = row.t.kind == AutoInsertPan || row.t.kind == AutoChanPan || row.t.kind == AutoChanPitch;
            const juce::String val = d != nullptr ? juce::String (d->min + t * (d->max - d->min), 2)
                                   : bip ? juce::String (autoToValue (row.t.kind, t), row.t.kind == AutoChanPitch ? 1 : 2) + (row.t.kind == AutoChanPitch ? " st" : "")
                                         : juce::String (juce::roundToInt (t * 100.0f)) + "%";
            fitText (g, (d != nullptr ? juce::String (d->name) + "  " : juce::String()) + val + (v >= 0.0f ? "  (automated)" : ""), in, juce::Justification::centredLeft, true);
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
        if (! fxMode()) { if (s.engine.isPlaying()) repaint (getLocalBounds().withTrimmedTop (barH)); return; }
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
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refill(); repaint(); }

    static constexpr int barH = phoneUI ? 36 : 30, headW = phoneUI ? 150 : 180;
    static constexpr int fftOrder = 11, fftN = 1 << fftOrder;
    Studio& s;
    juce::TextButton fxBtn, autoBtn, addBtn;
    juce::ComboBox laneBox, trackBox, mixBox, bayBox;
    juce::StringArray laneKeys;
    juce::String laneKey;
    int fTrack = -1, fMix = -1, fBay = -1;
    static constexpr int cardH = phoneUI ? 108 : 100;
    std::map<int, std::unique_ptr<BuiltinEffect>> models;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> snap, fftBuf, spec;
    double rate = 0;
    bool quiet = false;
    int scroll = 0;
};

std::unique_ptr<juce::Component> createLanes (Studio& s) { return std::make_unique<LanesView> (s); }
}
