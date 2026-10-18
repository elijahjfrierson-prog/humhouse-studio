#include "UI.h"

namespace hhs
{
namespace
{
constexpr int toolbarH = 32, headerW = 220, rulerH = 24;

juce::AudioFormatManager& formats()
{
    static juce::AudioFormatManager f;
    if (f.getNumKnownFormats() == 0) f.registerBasicFormats();
    return f;
}

double audioSeconds (const juce::File& f)
{
    std::unique_ptr<juce::AudioFormatReader> r (formats().createReaderFor (f));
    return r && r->sampleRate > 0 ? (double) r->lengthInSamples / r->sampleRate : 0.0;
}

void autoCrossfade (Project& p)
{
    for (auto& a : p.clips)
        for (auto& b : p.clips)
        {
            if (&a == &b || ! a.isAudio() || ! b.isAudio() || a.hidden || b.hidden || a.lane != b.lane) continue;
            if (a.start < b.start && a.start + a.length > b.start && a.start + a.length < b.start + b.length)
            {
                const double ov = a.start + a.length - b.start;
                a.fadeOut = juce::jmax (a.fadeOut, ov);
                b.fadeIn = juce::jmax (b.fadeIn, ov);
            }
        }
}

static void deleteLanes (Project& p, juce::SortedSet<int> which)
{
    for (int k = which.size(); --k >= 0;)
    {
        const int i = which[k];
        if (i < 0 || i >= (int) p.lanes.size()) continue;
        p.clips.erase (std::remove_if (p.clips.begin(), p.clips.end(), [i] (const Clip& c) { return c.lane == i; }), p.clips.end());
        for (auto& c : p.clips) if (c.lane > i) --c.lane;
        p.lanes.erase (p.lanes.begin() + i);
    }
}

static void insertLane (Project& p, int at)
{
    at = juce::jlimit (0, (int) p.lanes.size(), at);
    for (auto& c : p.clips) if (c.lane >= at) ++c.lane;
    Lane l; l.name = "Track " + juce::String (p.lanes.size() + 1); l.mixerTrack = p.addMixerTrack (l.name);
    p.lanes.insert (p.lanes.begin() + at, l);
}

struct LaneHeader : public juce::Component
{
    juce::SortedSet<int> targets() const
    {
        if (s.selectedLanes.contains (index)) return s.selectedLanes;
        juce::SortedSet<int> one; one.add (index); return one;
    }
    LaneHeader (Studio& st, int i) : s (st), index (i)
    {
        name.setEditable (false, true);
        name.setTooltip ("Track name (double-click to rename). Right-click the header for routing / delete");
        name.onTextChange = [this] { const auto t = name.getText(); s.edit ("Rename track", [&] (Project& p) { if (index < (int) p.lanes.size()) p.lanes[(size_t) index].name = t; }); };
        setupButton (arm, "R", "Arm for recording - every armed track records at once to its own insert (R then Space)", true);
        arm.setColour (juce::TextButton::buttonOnColourId, col::rec);
        arm.onClick = [this] { const bool on = arm.getToggleState(); const auto t = targets(); s.edit ("Arm", [&] (Project& p) { for (int i : t) if (i < (int) p.lanes.size()) p.lanes[(size_t) i].armed = on; }); };
        setupButton (mute, "M", "Mute this track", true);
        mute.setColour (juce::TextButton::buttonOnColourId, col::dim);
        mute.onClick = [this] { const bool on = mute.getToggleState(); const auto t = targets(); s.edit ("Mute track", [&] (Project& p) { for (int i : t) if (i < (int) p.lanes.size()) p.lanes[(size_t) i].muted = on; }); };
        setupButton (mon, "MON", "Hear the input live through this track's insert FX (AutoPitch, EQ, comp...)", true);
        mon.setColour (juce::TextButton::buttonOnColourId, col::accent2);
        mon.onClick = [this] { const bool on = mon.getToggleState(); s.edit ("Monitor", [&] (Project& p) { p.lanes[(size_t) index].monitor = on; }); };
        input.setTooltip ("Recording input: MONO puts one mic in the centre (both ears), STEREO records an input pair");
        input.onChange = [this] { const int id = input.getSelectedId(); if (id <= 0) return;
            s.edit ("Input", [&] (Project& p) { auto& l = p.lanes[(size_t) index];
                if (id >= 1000) { l.stereoIn = true; l.inputPair = id - 1000; } else { l.stereoIn = false; l.inputCh = id - 1; } }); };
        insert.setTooltip ("Mixer insert this track plays and records through - every take stays on it");
        insert.onChange = [this] { const int v = insert.getSelectedItemIndex(); s.edit ("Route track", [&] (Project& p) { p.lanes[(size_t) index].mixerTrack = v; }); };
        for (juce::Component* c : { (juce::Component*) &name, (juce::Component*) &arm, (juce::Component*) &mute, (juce::Component*) &mon,
                                    (juce::Component*) &input, (juce::Component*) &insert })
            addAndMakeVisible (c);
        refresh();
    }
    void refresh()
    {
        auto& p = s.p();
        if (index >= (int) p.lanes.size()) return;
        auto& l = p.lanes[(size_t) index];
        name.setText (l.name, juce::dontSendNotification);
        arm.setToggleState (l.armed, juce::dontSendNotification);
        mute.setToggleState (l.muted, juce::dontSendNotification);
        mon.setToggleState (l.monitor, juce::dontSendNotification);
        input.clear (juce::dontSendNotification);
        int ins = 2;
        if (auto* d = s.engine.deviceManager.getCurrentAudioDevice()) ins = juce::jmax (2, d->getInputChannelNames().size());
        input.addSectionHeading ("Mono (both ears)");
        for (int i = 0; i < juce::jmax (1, ins); ++i) input.addItem ("In " + juce::String (i + 1) + " Mono", i + 1);
        input.addSectionHeading ("Stereo");
        for (int i = 0; i < juce::jmax (1, ins / 2); ++i) input.addItem ("In " + juce::String (i * 2 + 1) + "/" + juce::String (i * 2 + 2) + " Stereo", 1000 + i);
        input.setSelectedId (l.stereoIn ? 1000 + l.inputPair : 1 + l.inputCh, juce::dontSendNotification);
        insert.clear (juce::dontSendNotification);
        for (size_t i = 0; i < p.mixer.size(); ++i) insert.addItem ((i == 0 ? juce::String ("Master") : juce::String ((int) i) + " " + p.mixer[i].name), (int) i + 1);
        insert.setSelectedItemIndex (juce::jlimit (0, (int) p.mixer.size() - 1, l.mixerTrack), juce::dontSendNotification);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::panel);
        if (s.selectedLanes.contains (index))
        {
            g.setColour (col::accent.withAlpha (0.16f)); g.fillRect (getLocalBounds());
            g.setColour (col::accent); g.fillRect (getLocalBounds().removeFromLeft (3));
        }
        g.setColour (col::gridBar); g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
        if (arm.getToggleState() && s.engine.isRecording()) { g.setColour (col::rec.withAlpha (0.25f)); g.fillRect (getLocalBounds().removeFromLeft (4)); }
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (4, 3);
        auto top = r.removeFromTop (r.getHeight() / 2);
        arm.setBounds (top.removeFromLeft (24)); top.removeFromLeft (3);
        mute.setBounds (top.removeFromLeft (24)); top.removeFromLeft (3);
        name.setBounds (top);
        r.removeFromTop (2);
        mon.setBounds (r.removeFromLeft (38)); r.removeFromLeft (3);
        input.setBounds (r.removeFromLeft (70)); r.removeFromLeft (3);
        insert.setBounds (r);
    }
    std::function<void (int lane, int height)> onHeight;
    std::function<void (int size)> onAllHeights;
    bool edgeDrag = false; int dragStartH = 0;
    bool nearBottomEdge (const juce::MouseEvent& e) const { return e.y >= getHeight() - 5; }
    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (nearBottomEdge (e) ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (edgeDrag && onHeight) onHeight (index, dragStartH + e.getDistanceFromDragStartY());
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && nearBottomEdge (e)) { edgeDrag = true; dragStartH = getHeight(); return; }
        auto& sel = s.selectedLanes;
        if (e.mods.isPopupMenu()) { if (! sel.contains (index)) { sel.clear(); sel.add (index); s.laneAnchor = index; } }
        else if (e.mods.isShiftDown() && s.laneAnchor >= 0)
        {
            sel.clear();
            for (int i = juce::jmin (s.laneAnchor, index); i <= juce::jmax (s.laneAnchor, index); ++i) sel.add (i);
        }
        else if (e.mods.isCommandDown()) { if (sel.contains (index)) sel.removeValue (index); else sel.add (index); s.laneAnchor = index; }
        else { sel.clear(); sel.add (index); s.laneAnchor = index; }
        if (getParentComponent() != nullptr) getParentComponent()->repaint();
        for (auto* c : getParentComponent()->getChildren()) c->repaint();
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (edgeDrag) { edgeDrag = false; return; }
        if (! e.mods.isPopupMenu()) return;
        juce::PopupMenu m;
        const int n = targets().size();
        m.addItem (1, "Rename..."); m.addItem (2, "New mixer insert for this track"); m.addItem (3, "Open insert in Mixer");
        if (index < (int) s.p().lanes.size()) m.addSubMenu ("Presets", trackPresetMenu (s, s.p().lanes[(size_t) index].mixerTrack));
        m.addSeparator(); m.addItem (5, "Insert track above"); m.addItem (6, "Insert track below");
        {
            juce::PopupMenu hm;
            hm.addItem (20, "Small"); hm.addItem (21, "Medium"); hm.addItem (22, "Large");
            m.addSubMenu ("Make all tracks", hm);
        }
        m.addSeparator(); m.addItem (4, n > 1 ? "Delete " + juce::String (n) + " selected tracks" : juce::String ("Delete track"));
        m.showMenuAsync ({}, [this] (int r) {
            const int i = index;
            if (r == 1) name.showEditor();
            else if (r == 2) s.edit ("New insert", [i] (Project& p) { p.lanes[(size_t) i].mixerTrack = p.addMixerTrack (p.lanes[(size_t) i].name); });
            else if (r == 3) { s.selectedTrack = s.p().lanes[(size_t) i].mixerTrack; if (s.showView) s.showView (2); s.sendChangeMessage(); }
            else if (r >= 20 && r <= 22) { if (onAllHeights) onAllHeights (r - 20); }
            else if (r == 4) { const auto t = targets(); s.selectedLanes.clear(); s.edit ("Delete track", [t] (Project& p) { deleteLanes (p, t); }); }
            else if (r == 5) { s.selectedLanes.clear(); s.edit ("Insert track", [i] (Project& p) { insertLane (p, i); }); }
            else if (r == 6) { s.selectedLanes.clear(); s.edit ("Insert track", [i] (Project& p) { insertLane (p, i + 1); }); }
        });
    }
    Studio& s; int index;
    juce::Label name; juce::TextButton arm, mute, mon; juce::ComboBox input, insert;
};

class Playlist : public juce::Component, public juce::FileDragAndDropTarget, public juce::TooltipClient,
                 private juce::ChangeListener, private juce::Timer
{
public:
    explicit Playlist (Studio& st) : s (st)
    {
        s.addChangeListener (this);
        setupButton (addTrack, "+ Track", "New audio track with its own mixer insert (vocals, guitar...)");
        addTrack.onClick = [this] {
            s.edit ("Add track", [] (Project& p) {
                Lane l; l.name = "Track " + juce::String (p.lanes.size() + 1); l.mixerTrack = p.addMixerTrack (l.name); p.lanes.push_back (l);
            });
        };
        {
            const char* labels[] { "SEL", "PAINT", "DEL", "MUTE", "SLICE" };
            const char* tips[] { "Select / move (E) - click a clip to select, drag to move", "Paint (B) - click empty space to paint copies of the last clip",
                                 "Delete (D) - click clips to delete them", "Mute (T) - click a clip to mute / unmute it",
                                 "Slice (C) - click an audio clip to cut it at the snap line" };
            for (int i = 0; i < 5; ++i)
            {
                auto* b = toolBtns.add (new juce::TextButton());
                setupButton (*b, labels[i], tips[i], true);
                b->setRadioGroupId (4301);
                b->onClick = [this, i] { tool = (PTool) i; };
                addAndMakeVisible (b);
            }
            toolBtns[0]->setToggleState (true, juce::dontSendNotification);
            setupButton (groupBtn, "GROUP", "Group the selected clips so they select and move together (Ctrl/Cmd+G, ungroup Ctrl/Cmd+Shift+G)");
            groupBtn.onClick = [this] { groupSelected (true); };
            addAndMakeVisible (groupBtn);
        }
        setupButton (loopRec, "LOOP REC", "Loop recording: the punch range loops and every pass becomes a take", true);
        loopRec.onClick = [this] { const bool on = loopRec.getToggleState(); s.edit ("Loop record", [on] (Project& p) { p.loopRecord = on; }); };
        setupButton (rts, "RETURN", "On stop, jump back to where playback started (FL style)", true);
        rts.onClick = [this] { const bool on = rts.getToggleState(); s.edit ("Return to start", [on] (Project& p) { p.returnToStart = on; }); };
        setupButton (preroll, "PRE-ROLL", "Start 1 bar before the punch-in so you can catch the vibe", true);
        preroll.onClick = [this] { const bool on = preroll.getToggleState(); s.edit ("Pre-roll", [on] (Project& p) { p.preRollBars = on ? 1.0 : 0.0; }); };
        setupButton (clearPunch, "CLEAR PUNCH", "Remove the punch-in / out range (or double-click the ruler)");
        clearPunch.onClick = [this] { s.edit ("Clear punch", [] (Project& p) { p.punchIn = p.punchOut = -1.0; }); };
        for (auto* b : { &addTrack, &loopRec, &rts, &preroll, &clearPunch }) addAndMakeVisible (b);
        thumbCache = std::make_unique<juce::AudioThumbnailCache> (256);
        setWantsKeyboardFocus (true);
        refresh();
        startTimerHz (30);
    }
    ~Playlist() override { s.removeChangeListener (this); }

    juce::String getTooltip() override
    {
        if (lastMouse.y >= toolbarH && lastMouse.y < toolbarH + rulerH && lastMouse.x > headerW)
            return "Ruler: click = move playhead, drag = set punch-in / out range, double-click = clear punch";
        if (lastMouse.x > headerW && lastMouse.y > toolbarH + rulerH)
            return "Double-click = place pattern | drag clip = move | drag edges = trim | top corners = fades (Alt = curve) | right-click = takes, routing, split";
        return {};
    }

    void resized() override
    {
        laneH = juce::jmax (60, (getHeight() - toolbarH - rulerH - 8) / juce::jmax (1, (int) s.p().lanes.size()));
        auto t = getLocalBounds().removeFromTop (toolbarH).reduced (6, 4);
        addTrack.setBounds (t.removeFromLeft (80)); t.removeFromLeft (12);
        for (auto* b : { &loopRec, &rts, &preroll, &clearPunch }) { b->setBounds (t.removeFromLeft (b == &clearPunch ? 100 : 78)); t.removeFromLeft (4); }
        t.removeFromLeft (10);
        for (auto* b : toolBtns) { b->setBounds (t.removeFromLeft (50)); t.removeFromLeft (2); }
        t.removeFromLeft (6);
        groupBtn.setBounds (t.removeFromLeft (60));
        layoutHeaders();
    }

    void paint (juce::Graphics& g) override
    {
        auto& p = s.p();
        g.fillAll (col::bg);
        g.setColour (col::panel); g.fillRect (0, 0, getWidth(), toolbarH);
        auto area = clipArea();
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (area);
            for (size_t i = 0; i < p.lanes.size(); ++i)
            {
                const int y = laneY ((int) i);
                g.setColour (i % 2 ? col::panel.darker (0.2f) : col::bg.brighter (0.03f));
                g.fillRect (area.getX(), y, area.getWidth(), laneHOf ((int) i));
                g.setColour (col::grid); g.drawHorizontalLine (y + laneHOf ((int) i) - 1, (float) area.getX(), (float) area.getRight());
            }
            const double b1 = xToBeat ((float) area.getRight());
            double step = 1.0;
            while (step * pxPerBeat < 10) step *= 2;
            for (double b = std::floor (scrollX / step) * step; b <= b1; b += step)
            {
                const bool bar = std::fmod (b + 1e-9, (double) p.beatsPerBar) < 1e-6;
                g.setColour (bar ? col::gridBar : col::grid);
                g.drawVerticalLine ((int) beatToX (b), (float) area.getY(), (float) area.getBottom());
            }
            if (p.hasPunch())
            {
                g.setColour (col::rec.withAlpha (0.07f));
                g.fillRect (beatToX (p.punchIn), (float) area.getY(), (float) ((p.punchOut - p.punchIn) * pxPerBeat), (float) area.getHeight());
            }
            for (auto& c : p.clips) if (! c.hidden) drawClip (g, c);
            const float px = beatToX (s.engine.positionBeats());
            const auto phc = s.engine.isRecording() ? col::rec : col::accent2;
            g.setColour (phc.withAlpha (0.22f)); g.fillRect (px - 2.5f, (float) area.getY(), 5.0f, (float) area.getHeight());
            g.setColour (phc); g.fillRect (px - 0.75f, (float) area.getY(), 1.5f, (float) area.getHeight());
        }
        // ruler
        juce::Rectangle<int> ruler (headerW, toolbarH, getWidth() - headerW, rulerH);
        g.setColour (col::panel2); g.fillRect (ruler);
        g.setFont (juce::FontOptions (11.0f));
        for (int bar = (int) (scrollX / p.beatsPerBar); ; ++bar)
        {
            const float x = beatToX (bar * p.beatsPerBar);
            if (x > getWidth()) break;
            if (x < headerW) continue;
            g.setColour (col::dim);
            g.drawText (juce::String (bar + 1), (int) x + 3, toolbarH, 40, rulerH, juce::Justification::centredLeft);
            g.drawVerticalLine ((int) x, (float) toolbarH + 12, (float) (toolbarH + rulerH));
        }
        if (p.hasPunch())
        {
            const float x0 = juce::jmax ((float) headerW, beatToX (p.punchIn)), x1 = beatToX (p.punchOut);
            g.setColour (col::rec.withAlpha (0.45f));
            g.fillRect (x0, (float) toolbarH + 2, x1 - x0, (float) rulerH - 4);
            g.setColour (col::text); g.drawText ("PUNCH", (int) x0 + 4, toolbarH, 60, rulerH, juce::Justification::centredLeft);
        }
        const float px = beatToX (s.engine.positionBeats());
        if (px >= headerW)
        {
            juce::Path tri; tri.addTriangle (px - 6, (float) toolbarH, px + 6, (float) toolbarH, px, (float) (toolbarH + 10));
            g.setColour (col::accent2); g.fillPath (tri);
        }
        g.setColour (col::panel); g.fillRect (0, toolbarH, headerW, rulerH);
        g.setColour (col::dim); g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText ("PLAYLIST", 8, toolbarH, headerW - 16, rulerH, juce::Justification::centredLeft);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        lastMouse = e.getPosition();
        Part part; const int i = hitClip (e.position, part);
        setMouseCursor (i < 0 ? juce::MouseCursor::NormalCursor
                        : part == Part::FadeIn || part == Part::FadeOut ? juce::MouseCursor::PointingHandCursor
                        : part == Part::Right || part == Part::Left ? juce::MouseCursor::LeftRightResizeCursor
                        : juce::MouseCursor::DraggingHandCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        auto& p = s.p();
        changed = false;
        dragStart = e.position;
        if (e.y >= toolbarH && e.y < toolbarH + rulerH && e.x > headerW) { drag = Drag::Ruler; return; }
        if (! clipArea().contains (e.getPosition())) return;
        Part part; const int i = hitClip (e.position, part);
        if (e.mods.isPopupMenu()) { if (i >= 0) clipMenu (i); return; }
        if (i < 0 && tool == PTool::Paint)
        {
            const int lane = laneAt (e.position.y);
            if (lane < 0 || lane >= (int) p.lanes.size()) return;
            const double at = juce::jmax (0.0, std::floor (xToBeat (e.position.x) / gridSnap()) * gridSnap());
            s.edit ("Paint clip", [&] (Project& pr) {
                Clip c;
                auto src = std::find_if (pr.clips.begin(), pr.clips.end(), [this] (const Clip& o) { return o.uid == lastClipUid; });
                if (src != pr.clips.end()) c = *src;
                else if (! pr.patterns.empty()) { c.patternIndex = juce::jlimit (0, (int) pr.patterns.size() - 1, pr.currentPattern); c.length = pr.patterns[(size_t) c.patternIndex].lengthBeats(); }
                else return;
                c.uid = pr.newUid(); c.lane = lane; c.start = at; c.selected = false; c.group = 0; c.takeGroup = 0; c.hidden = false;
                pr.clips.push_back (c);
            });
            return;
        }
        if (i >= 0 && tool == PTool::Delete)
        {
            const int g = p.clips[(size_t) i].group, uid = p.clips[(size_t) i].uid;
            s.edit ("Delete clip", [&] (Project& pr) {
                pr.clips.erase (std::remove_if (pr.clips.begin(), pr.clips.end(), [&] (const Clip& c) { return c.uid == uid || (g > 0 && c.group == g); }), pr.clips.end());
            });
            return;
        }
        if (i >= 0 && tool == PTool::Mute)
        {
            const int uid = p.clips[(size_t) i].uid;
            s.edit ("Mute clip", [&] (Project& pr) { for (auto& c : pr.clips) if (c.uid == uid) c.muted = ! c.muted; });
            return;
        }
        if (i >= 0 && tool == PTool::Slice)
        {
            const int uid = p.clips[(size_t) i].uid;
            const double at = snap (xToBeat (e.position.x));
            s.edit ("Slice clip", [&] (Project& pr) {
                auto it = std::find_if (pr.clips.begin(), pr.clips.end(), [uid] (const Clip& c) { return c.uid == uid; });
                if (it == pr.clips.end() || ! it->isAudio() || at <= it->start || at >= it->start + it->length) return;
                Clip b = *it; b.uid = pr.newUid();
                const double shortFade = secondsToBeats (0.005, pr.bpm);
                b.start = at; b.length = it->start + it->length - at; b.sourceOffset += beatsToSeconds (at - it->start, pr.bpm);
                b.fadeIn = shortFade; b.selected = false;
                it->length = at - it->start; it->fadeOut = shortFade;
                pr.clips.push_back (b);
            });
            return;
        }
        if (i < 0)
        {
            s.live ([] (Project& pr) { for (auto& c : pr.clips) c.selected = false; });
            s.engine.setPositionBeats (juce::jmax (0.0, snap (xToBeat (e.position.x))));
            repaint();
            return;
        }
        s.live ([&] (Project&) {
            if (! p.clips[(size_t) i].selected && ! e.mods.isShiftDown()) for (auto& c : p.clips) c.selected = false;
            p.clips[(size_t) i].selected = true;
            if (const int g = p.clips[(size_t) i].group; g > 0) for (auto& c : p.clips) if (c.group == g) c.selected = true;
        });
        lastClipUid = p.clips[(size_t) i].uid;
        dragClip = i; dragOrig = p.clips;
        drag = part == Part::FadeIn ? Drag::FadeIn : part == Part::FadeOut ? Drag::FadeOut : part == Part::Right ? Drag::Resize
             : part == Part::Left ? Drag::Trim : Drag::Move;
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto& p = s.p();
        lastMouse = e.getPosition();
        if (drag == Drag::Ruler)
        {
            if (std::abs (e.position.x - dragStart.x) < 4) return;
            const double a = juce::jmax (0.0, snap (xToBeat (dragStart.x))), b = juce::jmax (0.0, snap (xToBeat (e.position.x)));
            s.live ([&] (Project& pr) { pr.punchIn = juce::jmin (a, b); pr.punchOut = juce::jmax (a, b); });
            changed = true; repaint(); return;
        }
        if (drag == Drag::None || dragClip < 0 || dragOrig.size() != p.clips.size()) return;
        const bool noSnap = e.mods.isAltDown();
        double d = xToBeat (e.position.x) - xToBeat (dragStart.x);
        if (! noSnap && drag != Drag::FadeIn && drag != Drag::FadeOut) d = std::round (d / gridSnap()) * gridSnap();
        const int dLane = drag == Drag::Move ? laneAt (e.position.y) - laneAt (dragStart.y) : 0;
        s.live ([&] (Project&) {
            for (size_t i = 0; i < p.clips.size(); ++i)
            {
                auto o = dragOrig[i];
                if (drag == Drag::FadeIn || drag == Drag::FadeOut)
                {
                    if ((int) i != dragClip) continue;
                    if (e.mods.isAltDown()) o.fadeCurve = juce::jlimit (-1.0f, 1.0f, o.fadeCurve - (e.position.y - dragStart.y) / 80.0f);
                    else if (drag == Drag::FadeIn) o.fadeIn = juce::jlimit (0.0, o.length - o.fadeOut, o.fadeIn + d);
                    else o.fadeOut = juce::jlimit (0.0, o.length - o.fadeIn, o.fadeOut - d);
                }
                else if (! o.selected) continue;
                else if (drag == Drag::Move)
                {
                    o.start = juce::jmax (0.0, o.start + d);
                    o.lane = juce::jlimit (0, juce::jmax (0, (int) p.lanes.size() - 1), o.lane + dLane);
                }
                else if (drag == Drag::Resize) o.length = juce::jmax (gridSnap() * 0.25, o.length + d);
                else if (drag == Drag::Trim && o.isAudio())
                {
                    const double dd = juce::jlimit (-beatsFromSeconds (o.sourceOffset), o.length - 0.0625, d);
                    o.start += dd; o.length -= dd; o.sourceOffset += beatsToSeconds (dd, p.bpm);
                }
                p.clips[i] = o;
            }
        });
        changed = true;
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        auto& p = s.p();
        if (drag == Drag::Ruler && ! changed) s.engine.setPositionBeats (juce::jmax (0.0, snap (xToBeat (e.position.x))));
        else if (changed)
        {
            s.edit (drag == Drag::Ruler ? "Punch range" : drag == Drag::FadeIn || drag == Drag::FadeOut ? "Fade" : "Edit clips", [&] (Project&) {
                // routing follows the recording: a moved audio clip keeps playing through the insert it came from
                if (drag == Drag::Move && dragOrig.size() == p.clips.size())
                    for (size_t i = 0; i < p.clips.size(); ++i)
                    {
                        auto& c = p.clips[i];
                        if (c.isAudio() && c.lane != dragOrig[i].lane && c.mixerTrack < 0 && dragOrig[i].lane < (int) p.lanes.size())
                            c.mixerTrack = p.lanes[(size_t) dragOrig[i].lane].mixerTrack;
                    }
                autoCrossfade (p);
            });
        }
        drag = Drag::None; dragClip = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        auto& p = s.p();
        if (e.y >= toolbarH && e.y < toolbarH + rulerH) { s.edit ("Clear punch", [] (Project& pr) { pr.punchIn = pr.punchOut = -1.0; }); return; }
        if (! clipArea().contains (e.getPosition())) return;
        Part part; const int i = hitClip (e.position, part);
        if (i >= 0)
        {
            if (! p.clips[(size_t) i].isAudio())
            {
                const int pat = p.clips[(size_t) i].patternIndex;
                s.edit ("Open pattern", [pat] (Project& pr) { pr.currentPattern = pat; });
                if (s.showView) s.showView (0);
            }
            return;
        }
        const int lane = laneAt (e.position.y);
        if (lane < 0 || p.patterns.empty()) return;
        const double at = juce::jmax (0.0, std::floor (xToBeat (e.position.x) / p.beatsPerBar) * p.beatsPerBar);
        s.edit ("Place pattern", [&] (Project& pr) {
            Clip c; c.uid = pr.newUid(); c.lane = lane; c.start = at; c.patternIndex = pr.currentPattern;
            c.length = pr.patterns[(size_t) pr.currentPattern].lengthBeats();
            pr.clips.push_back (c);
        });
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isAltDown())   // Alt+wheel: all tracks taller / thinner
        {
            zoomH = juce::jlimit (0.4f, 4.0f, zoomH * (w.deltaY > 0 ? 1.1f : 1.0f / 1.1f));
            layoutHeaders(); repaint();
            return;
        }
        if (e.mods.isCommandDown())
        {
            const double at = xToBeat (e.position.x);
            pxPerBeat = juce::jlimit (2.0f, 400.0f, pxPerBeat * (w.deltaY > 0 ? 1.15f : 1.0f / 1.15f));
            scrollX = juce::jmax (0.0, at - (e.position.x - headerW) / pxPerBeat);
        }
        else if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
            scrollX = juce::jmax (0.0, scrollX - (w.deltaX != 0 ? w.deltaX : w.deltaY) * 600.0 / pxPerBeat);
        else
        {
            scrollY = juce::jlimit (0, juce::jmax (0, totalLaneH() - clipArea().getHeight() + 40), scrollY - (int) (w.deltaY * 200));
            layoutHeaders();
        }
        repaint();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (isTypingNoteKey (k)) return false;
        const int cmd = juce::ModifierKeys::commandModifier;
        if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
        {
            const bool anyClip = std::any_of (s.p().clips.begin(), s.p().clips.end(), [] (const Clip& c) { return c.selected; });
            if (! anyClip && ! s.selectedLanes.isEmpty())
            {
                const auto t = s.selectedLanes; s.selectedLanes.clear();
                s.edit ("Delete tracks", [t] (Project& p) { deleteLanes (p, t); });
                return true;
            }
            s.edit ("Delete clips", [] (Project& p) { p.clips.erase (std::remove_if (p.clips.begin(), p.clips.end(), [] (const Clip& c) { return c.selected; }), p.clips.end()); });
            return true;
        }
        if (k == juce::KeyPress ('g', cmd, 0)) { groupSelected (true); return true; }
        if (k == juce::KeyPress ('g', cmd | juce::ModifierKeys::shiftModifier, 0)) { groupSelected (false); return true; }
        if (k.getModifiers().isAnyModifierKeyDown() == false)
        {
            const std::pair<int, PTool> keys[] { { 'e', PTool::Select }, { 'b', PTool::Paint }, { 'd', PTool::Delete }, { 't', PTool::Mute }, { 'c', PTool::Slice } };
            for (auto& [key, tl] : keys)
                if (k.getKeyCode() == key || k.getKeyCode() == juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) key))
                { tool = tl; toolBtns[(int) tl]->setToggleState (true, juce::dontSendNotification); return true; }
        }
        if (k == juce::KeyPress ('b', cmd, 0))
        {
            s.edit ("Duplicate clips", [] (Project& p) {
                double lo = 1e9, hi = 0;
                for (auto& c : p.clips) if (c.selected) { lo = std::min (lo, c.start); hi = std::max (hi, c.start + c.length); }
                const auto n = p.clips.size();
                for (size_t i = 0; i < n; ++i)
                    if (p.clips[i].selected) { auto c = p.clips[i]; c.uid = p.newUid(); c.start += hi - lo; p.clips[i].selected = false; p.clips.push_back (c); }
            });
            return true;
        }
        if (k == juce::KeyPress ('a', cmd, 0)) { s.live ([] (Project& p) { for (auto& c : p.clips) c.selected = true; }); repaint(); return true; }
        return false;
    }

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        const double at = juce::jmax (0.0, snap (xToBeat ((float) x)));
        int lane = laneAt ((float) y);
        s.edit ("Add audio", [&] (Project& p) {
            for (auto& path : files)
            {
                juce::File f (path);
                const double secs = audioSeconds (f);
                if (secs <= 0) continue;
                if (lane < 0 || lane >= (int) p.lanes.size())
                {
                    Lane l; l.name = f.getFileNameWithoutExtension(); l.mixerTrack = p.addMixerTrack (l.name);
                    p.lanes.push_back (l); lane = (int) p.lanes.size() - 1;
                }
                Clip c; c.uid = p.newUid(); c.lane = lane; c.start = at; c.audioFile = f; c.patternIndex = -1;
                c.length = secondsToBeats (secs, p.bpm);
                p.clips.push_back (c);
                ++lane;
            }
        });
    }

private:
    enum class Drag { None, Ruler, Move, Resize, Trim, FadeIn, FadeOut };
    enum class PTool { Select, Paint, Delete, Mute, Slice };
    PTool tool = PTool::Select;
    juce::OwnedArray<juce::TextButton> toolBtns;
    juce::TextButton groupBtn;
    int lastClipUid = 0;
    void groupSelected (bool on)
    {
        s.edit (on ? "Group clips" : "Ungroup clips", [on] (Project& pr) {
            int g = 0;
            if (on) { for (auto& c : pr.clips) g = juce::jmax (g, c.group); ++g; }
            int n = 0;
            for (auto& c : pr.clips) if (c.selected) { c.group = on ? g : 0; ++n; }
            if (on && n < 2) for (auto& c : pr.clips) if (c.group == g) c.group = 0;
        });
    }
    enum class Part { Body, Left, Right, FadeIn, FadeOut };
    Studio& s;
    std::vector<std::unique_ptr<LaneHeader>> headers;
    juce::TextButton addTrack, loopRec, rts, preroll, clearPunch;
    std::unique_ptr<juce::AudioThumbnailCache> thumbCache;
    std::map<juce::String, std::unique_ptr<juce::AudioThumbnail>> thumbs;
    double scrollX = 0; int scrollY = 0; float pxPerBeat = 24.0f;
    int laneH = 60;   // even height: tracks fill the view, never thin lines
    Drag drag = Drag::None; int dragClip = -1; bool changed = false;
    std::vector<Clip> dragOrig;
    juce::Point<float> dragStart; juce::Point<int> lastMouse;

    juce::Rectangle<int> clipArea() const { return getLocalBounds().withTrimmedTop (toolbarH + rulerH).withTrimmedLeft (headerW); }
    float beatToX (double b) const { return (float) headerW + (float) ((b - scrollX) * pxPerBeat); }
    double xToBeat (float x) const { return scrollX + (x - (float) headerW) / pxPerBeat; }
    std::map<int, float> laneMul;   // per-track height (drag a header's bottom edge)
    float zoomH = 1.0f;             // all tracks (Alt+wheel, Make all tracks)
    int numLanes() const { return (int) const_cast<Studio&> (s).p().lanes.size(); }
    int laneHOf (int lane) const
    {
        auto it = laneMul.find (lane);
        return juce::jlimit (22, 800, (int) ((float) laneH * zoomH * (it != laneMul.end() ? it->second : 1.0f)));
    }
    int totalLaneH() const { int t = 0; for (int i = 0; i < numLanes(); ++i) t += laneHOf (i); return t; }
    int laneY (int lane) const
    {
        int y = toolbarH + rulerH - scrollY;
        for (int i = 0; i < lane; ++i) y += laneHOf (i);
        return y;
    }
    int laneAt (float y) const
    {
        int top = toolbarH + rulerH - scrollY;
        for (int i = 0; i < numLanes(); ++i) { const int h = laneHOf (i); if (y >= (float) top && y < (float) (top + h)) return i; top += h; }
        return -1;
    }
    double gridSnap() const
    {
        auto& p = const_cast<Studio&> (s).p();
        double g = 1.0;
        while (g * pxPerBeat < 12) g *= 2;
        while (g * pxPerBeat > 80 && g > p.snap && g > 0.0625) g *= 0.5;
        return g;
    }
    double snap (double b) const { const double g = gridSnap(); return std::round (b / g) * g; }
    double beatsFromSeconds (double secs) const { return secondsToBeats (secs, const_cast<Studio&> (s).p().bpm); }
    juce::Rectangle<float> clipRect (const Clip& c) const
    {
        return { beatToX (c.start), (float) laneY (c.lane) + 2.0f, juce::jmax (4.0f, (float) (c.length * pxPerBeat)), (float) laneHOf (c.lane) - 4.0f };
    }
    int hitClip (juce::Point<float> pt, Part& part) const
    {
        auto& p = const_cast<Studio&> (s).p();
        part = Part::Body;
        for (int i = (int) p.clips.size() - 1; i >= 0; --i)
        {
            auto& c = p.clips[(size_t) i];
            if (c.hidden) continue;
            auto r = clipRect (c);
            if (! r.contains (pt)) continue;
            if (c.isAudio() && pt.y < r.getY() + 12)
            {
                const float fi = r.getX() + (float) (c.fadeIn * pxPerBeat), fo = r.getRight() - (float) (c.fadeOut * pxPerBeat);
                if (std::abs (pt.x - fi) < 8) { part = Part::FadeIn; return i; }
                if (std::abs (pt.x - fo) < 8) { part = Part::FadeOut; return i; }
            }
            if (pt.x > r.getRight() - 6) part = Part::Right;
            else if (c.isAudio() && pt.x < r.getX() + 6) part = Part::Left;
            return i;
        }
        return -1;
    }
    juce::AudioThumbnail* thumb (const juce::File& f)
    {
        auto key = f.getFullPathName();
        auto it = thumbs.find (key);
        if (it != thumbs.end()) return it->second.get();
        if (! f.existsAsFile()) return nullptr;
        auto t = std::make_unique<juce::AudioThumbnail> (512, formats(), *thumbCache);
        t->setSource (new juce::FileInputSource (f));
        t->addChangeListener (this);
        return (thumbs[key] = std::move (t)).get();
    }
    void drawClip (juce::Graphics& g, const Clip& c)
    {
        auto& p = s.p();
        auto r = clipRect (c);
        if (r.getRight() < headerW || r.getX() > getWidth()) return;
        juce::Colour colour = c.isAudio() ? juce::Colour (0xff3d9bff) : (c.patternIndex < (int) p.patterns.size() ? p.patterns[(size_t) c.patternIndex].colour : col::accent);
        if (c.muted) colour = col::dim;
        g.setColour (colour.withAlpha (0.28f)); g.fillRoundedRectangle (r, 4);
        g.setColour (colour.withAlpha (0.9f)); g.fillRoundedRectangle (r.withHeight (14), 4);
        auto body = r.withTrimmedTop (14).reduced (1, 2);
        if (c.isAudio())
        {
            if (auto* t = thumb (c.audioFile))
            {
                const double secs = beatsToSeconds (c.length, p.bpm);
                g.setColour (colour.brighter (0.4f));
                t->drawChannels (g, body.toNearestInt(), c.sourceOffset, c.sourceOffset + secs, c.gain);
            }
            // fades
            const float fi = (float) (c.fadeIn * pxPerBeat), fo = (float) (c.fadeOut * pxPerBeat);
            g.setColour (juce::Colours::black.withAlpha (0.45f));
            juce::Path pin, pout;
            const float bend = c.fadeCurve * 0.5f;
            pin.startNewSubPath (r.getX(), r.getBottom()); pin.quadraticTo (r.getX() + fi * (0.5f - bend), r.getY() + r.getHeight() * (0.5f + bend), r.getX() + fi, r.getY());
            pin.lineTo (r.getX(), r.getY()); pin.closeSubPath();
            pout.startNewSubPath (r.getRight(), r.getBottom()); pout.quadraticTo (r.getRight() - fo * (0.5f - bend), r.getY() + r.getHeight() * (0.5f + bend), r.getRight() - fo, r.getY());
            pout.lineTo (r.getRight(), r.getY()); pout.closeSubPath();
            if (fi > 1) g.fillPath (pin);
            if (fo > 1) g.fillPath (pout);
            g.setColour (col::text);
            g.fillRect (r.getX() + fi - 3, r.getY(), 6.0f, 6.0f);
            g.fillRect (r.getRight() - fo - 3, r.getY(), 6.0f, 6.0f);
        }
        else if (c.patternIndex >= 0 && c.patternIndex < (int) p.patterns.size())
        {
            auto& pat = p.patterns[(size_t) c.patternIndex];
            const double len = pat.lengthBeats();
            int lo = 127, hi = 0;
            for (auto& [id, list] : pat.notes) for (auto& n : list) { lo = std::min (lo, n.key); hi = std::max (hi, n.key); }
            if (hi >= lo)
            {
                g.setColour (colour.brighter (0.5f));
                for (double off = 0; off < c.length; off += len)
                    for (auto& [id, list] : pat.notes)
                        for (auto& n : list)
                        {
                            if (off + n.start >= c.length) continue;
                            const float y = body.getBottom() - 3 - (body.getHeight() - 6) * (float) (n.key - lo) / (float) juce::jmax (1, hi - lo);
                            g.fillRect (body.getX() + (float) ((off + n.start) * pxPerBeat), y, juce::jmax (1.5f, (float) (n.length * pxPerBeat)), 2.0f);
                        }
            }
        }
        juce::String label = c.isAudio() ? c.audioFile.getFileNameWithoutExtension()
                                         : (c.patternIndex < (int) p.patterns.size() ? p.patterns[(size_t) c.patternIndex].name : juce::String ("?"));
        if (c.isAudio())
        {
            const int ins = p.clipInsert (c);
            if (ins >= 0 && ins < (int) p.mixer.size()) label << "  > " << p.mixer[(size_t) ins].name;
            if (c.takeGroup > 0)
            {
                int n = 0, k = 0;
                for (auto& o : p.clips) if (o.takeGroup == c.takeGroup) { ++n; if (o.uid == c.uid) k = n; }
                if (n > 1) label << "  [take " << k << "/" << n << "]";
            }
        }
        g.setColour (juce::Colours::black); g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (label, r.withHeight (14).reduced (5, 0), juce::Justification::centredLeft, true);
        if (c.selected) { g.setColour (juce::Colours::white); g.drawRoundedRectangle (r, 4, 1.5f); }
    }
    void clipMenu (int index)
    {
        auto& p = s.p();
        const auto clip = p.clips[(size_t) index];
        juce::PopupMenu m, route, takes, fades;
        m.addItem (1, "Delete");
        m.addItem (2, "Split at playhead", clip.isAudio());
        m.addItem (3, clip.muted ? "Unmute" : "Mute");
        fades.addItem (10, "Short fades (click-free)"); fades.addItem (11, "Fade in 1 beat"); fades.addItem (12, "Fade out 1 beat");
        fades.addItem (13, "Fade in + out 1 bar"); fades.addItem (14, "Clear fades");
        m.addSubMenu ("Fades (selected clips)", fades, clip.isAudio());
        for (size_t i = 0; i < p.mixer.size(); ++i)
            route.addItem (100 + (int) i, i == 0 ? juce::String ("Master") : juce::String ((int) i) + " " + p.mixer[i].name, true, p.clipInsert (clip) == (int) i);
        route.addSeparator(); route.addItem (99, "Follow track insert");
        m.addSubMenu ("Route to insert", route, clip.isAudio());
        std::vector<int> group;
        if (clip.takeGroup > 0)
            for (auto& o : p.clips) if (o.takeGroup == clip.takeGroup) group.push_back (o.uid);
        for (size_t k = 0; k < group.size(); ++k) takes.addItem (1000 + group[k], "Take " + juce::String ((int) k + 1), true, group[k] == clip.uid);
        m.addSubMenu ("Takes", takes, group.size() > 1);
        m.addItem (4, "Reverse", clip.isAudio(), clip.reversed);
        m.addItem (5, "Split by channel (one clip per instrument)", ! clip.isAudio());
        m.addItem (6, "Group selected clips"); m.addItem (7, "Ungroup", clip.group > 0);
        m.showMenuAsync ({}, [this, uid = clip.uid] (int r) {
            if (r == 0) return;
            s.edit ("Clip edit", [&] (Project& pr) {
                auto it = std::find_if (pr.clips.begin(), pr.clips.end(), [uid] (const Clip& c) { return c.uid == uid; });
                if (it == pr.clips.end()) return;
                auto& c = *it;
                auto forSel = [&] (std::function<void (Clip&)> f) { for (auto& o : pr.clips) if ((o.selected || o.uid == uid) && o.isAudio()) f (o); };
                const double shortFade = secondsToBeats (0.005, pr.bpm);
                if (r == 1) pr.clips.erase (it);
                else if (r == 2)
                {
                    const double at = s.engine.positionBeats();
                    if (at > c.start && at < c.start + c.length)
                    {
                        Clip b = c; b.uid = pr.newUid();
                        b.start = at; b.length = c.start + c.length - at; b.sourceOffset += beatsToSeconds (at - c.start, pr.bpm);
                        b.fadeIn = shortFade; c.length = at - c.start; c.fadeOut = shortFade; b.selected = false;
                        pr.clips.push_back (b);
                    }
                }
                else if (r == 3) c.muted = ! c.muted;
                else if (r == 4) c.reversed = ! c.reversed;
                else if (r == 6 || r == 7)
                {
                    int g = 0;
                    if (r == 6) { for (auto& o : pr.clips) g = juce::jmax (g, o.group); ++g; }
                    const int old = c.group;
                    for (auto& o : pr.clips) if (o.selected || o.uid == uid || (r == 7 && old > 0 && o.group == old)) o.group = g;
                }
                else if (r == 5 && c.patternIndex >= 0 && c.patternIndex < (int) pr.patterns.size())
                {
                    const Clip orig = c;
                    const Pattern src = pr.patterns[(size_t) orig.patternIndex];
                    std::vector<Clip> made;
                    int lane = orig.lane;
                    for (auto& ch : pr.channels)
                    {
                        auto nit = src.notes.find (ch.id);
                        if (nit == src.notes.end() || nit->second.empty()) continue;
                        Pattern one; one.name = src.name + " - " + ch.name; one.colour = ch.colour;
                        one.notes[ch.id] = nit->second;
                        pr.patterns.push_back (one);
                        while (lane >= (int) pr.lanes.size())
                        {
                            Lane l; l.name = "Track " + juce::String (pr.lanes.size() + 1); l.mixerTrack = pr.addMixerTrack (l.name); pr.lanes.push_back (l);
                        }
                        Clip nc = orig; nc.uid = pr.newUid(); nc.lane = lane++; nc.patternIndex = (int) pr.patterns.size() - 1; nc.group = 0; nc.selected = true;
                        made.push_back (nc);
                    }
                    if (made.size() > 1)
                    {
                        pr.clips.erase (std::remove_if (pr.clips.begin(), pr.clips.end(), [uid] (const Clip& o) { return o.uid == uid; }), pr.clips.end());
                        for (auto& o : pr.clips) o.selected = false;
                        for (auto& nc : made) pr.clips.push_back (nc);
                    }
                    else if (! made.empty()) pr.patterns.pop_back();
                    return;
                }
                else if (r == 10) forSel ([&] (Clip& o) { o.fadeIn = o.fadeOut = shortFade; });
                else if (r == 11) forSel ([] (Clip& o) { o.fadeIn = juce::jmin (1.0, o.length * 0.5); });
                else if (r == 12) forSel ([] (Clip& o) { o.fadeOut = juce::jmin (1.0, o.length * 0.5); });
                else if (r == 13) forSel ([&] (Clip& o) { o.fadeIn = o.fadeOut = juce::jmin ((double) pr.beatsPerBar, o.length * 0.5); });
                else if (r == 14) forSel ([] (Clip& o) { o.fadeIn = o.fadeOut = 0; });
                else if (r == 99) c.mixerTrack = -1;
                else if (r >= 100 && r < 1000) forSel ([r] (Clip& o) { o.mixerTrack = r - 100; });
                else if (r >= 1000) { const int keep = r - 1000; for (auto& o : pr.clips) if (o.takeGroup == c.takeGroup) o.hidden = o.uid != keep; }
            });
        });
    }
    void layoutHeaders()
    {
        for (size_t i = 0; i < headers.size(); ++i)
        {
            const int y = laneY ((int) i);
            headers[i]->setBounds (0, y, headerW, laneHOf ((int) i));
            headers[i]->setVisible (y + laneHOf ((int) i) > toolbarH + rulerH);
        }
    }
    void refresh()
    {
        auto& p = s.p();
        if (headers.size() != p.lanes.size())
        {
            headers.clear();
            for (size_t i = 0; i < p.lanes.size(); ++i) 
            {
                headers.push_back (std::make_unique<LaneHeader> (s, (int) i));
                headers.back()->onHeight = [this] (int lane, int h)
                {
                    laneMul[lane] = juce::jlimit (0.3f, 8.0f, (float) h / (float) juce::jmax (1.0f, (float) laneH * zoomH));
                    layoutHeaders(); repaint();
                };
                headers.back()->onAllHeights = [this] (int size)
                {
                    laneMul.clear();
                    zoomH = size == 0 ? 0.6f : size == 1 ? 1.0f : 1.8f;
                    layoutHeaders(); repaint();
                };
                addAndMakeVisible (*headers.back());
            }
            layoutHeaders();
        }
        for (auto& h : headers) h->refresh();
        loopRec.setToggleState (p.loopRecord, juce::dontSendNotification);
        rts.setToggleState (p.returnToStart, juce::dontSendNotification);
        preroll.setToggleState (p.preRollBars > 0, juce::dontSendNotification);
        repaint();
    }
    void changeListenerCallback (juce::ChangeBroadcaster* src) override
    {
        if (src == &s) refresh(); else repaint();
    }
    void timerCallback() override
    {
        if (! s.engine.isPlaying()) return;
        const float px = beatToX (s.engine.positionBeats());
        if (px > getWidth() - 40) { scrollX = s.engine.positionBeats() - 2.0; }
        repaint();
        for (auto& h : headers) if (h->arm.getToggleState()) h->repaint();
    }
};
}

std::unique_ptr<juce::Component> createPlaylist (Studio& s) { return std::make_unique<Playlist> (s); }
}
