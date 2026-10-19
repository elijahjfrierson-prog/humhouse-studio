#include "UI.h"

namespace hhs
{
namespace
{
const char* rootNames[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
const double* const snapValues = kSnapValues;
const char* const* const snapNames = kSnapNames;
bool isBlack (int key) { const int k = key % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }

class PianoRoll : public juce::Component, public juce::TooltipClient, private juce::ChangeListener, private juce::Timer
{
public:
    explicit PianoRoll (Studio& st) : s (st)
    {
        s.addChangeListener (this);
        for (int i = 0; i < 12; ++i) rootBox.addItem (rootNames[i], i + 1);
        rootBox.setTooltip ("Project key - highlighted rows follow it");
        rootBox.onChange = [this] { const int v = rootBox.getSelectedItemIndex(); s.edit ("Key", [v] (Project& p) { p.keyRoot = v; }); };
        int id = 1;
        for (auto& sc : allScales()) scaleBox.addItem (sc.name, id++);
        scaleBox.setTooltip ("Scale - in-scale rows are highlighted; Alt+K folds notes into it");
        scaleBox.onChange = [this] { const int v = scaleBox.getSelectedItemIndex(); s.edit ("Scale", [v] (Project& p) { p.scaleIndex = v; }); };
        setupButton (highlightBtn, "SCALE", "Highlight the notes of the key / scale", true);
        highlightBtn.onClick = [this] { const bool on = highlightBtn.getToggleState(); s.edit ("Scale highlight", [on] (Project& p) { p.highlightScale = on; }); };
        for (int i = 0; i < kNumSnaps; ++i) snapBox.addItem (snapNames[i], i + 1);
        snapBox.setTooltip ("Snap grid for drawing and moving notes (hold Alt while dragging to bypass)");
        snapBox.onChange = [this] { const double v = snapValues[juce::jmax (0, snapBox.getSelectedItemIndex())]; s.live ([v] (Project& p) { p.snap = v; }); repaint(); };
        setupButton (toolsBtn, "TOOLS", "Piano roll tools: arp/rolls, strum, chop, flam, quantize, randomize, limit...");
        toolsBtn.onClick = [this] { toolsMenu(); };
        setupButton (slideBtn, "SLIDE", "Draw slide (glide) notes - the pitch glides from the previous note, great for 808s", true);
        setupButton (stampBtn, "STAMP", "Chord stamp (Alt/Opt+T): every note you draw lays down the chosen chord", true);
        stampBtn.onClick = [this] { stampBtn.setToggleState (stampChord >= 0, juce::dontSendNotification); stampMenu(); };
        setupButton (ghostBtn, "GHOST", "Show other channels' notes faintly (Alt+V)", true);
        {
            const char* labels[] { "DRAW", "PAINT", "DEL", "MUTE", "SLICE", "SEL", "ZOOM" };
            const char* tips[] { "Draw (P) - click to add, drag to move / resize", "Paint (B) - drag to paint a run of notes",
                                 "Delete (D) - click or drag over notes to erase", "Mute (T) - click a note to mute / unmute it",
                                 "Slice (C) - click a note to cut it at the snap line", "Select (E) - drag a box to select notes",
                                 "Zoom (Z) - click to zoom in, Shift-click to zoom out" };
            for (int i = 0; i < 7; ++i)
            {
                auto* b = toolBtns.add (new juce::TextButton());
                setupButton (*b, labels[i], tips[i], true);
                b->setRadioGroupId (4201);
                b->onClick = [this, i] { editTool = (EditTool) i; };
                addAndMakeVisible (b);
            }
            toolBtns[0]->setToggleState (true, juce::dontSendNotification);
        }
        ghostBtn.onClick = [this] { repaint(); };
        setupButton (muteBtn, "MUTE", "Mute note previews (M): clicking / drawing notes stays silent, playback still sounds", true);
        muteBtn.setToggleState (s.settings != nullptr && s.settings->getBoolValue ("prMutePreview", false), juce::dontSendNotification);
        muteBtn.onClick = [this] { if (s.settings) s.settings->setValue ("prMutePreview", muteBtn.getToggleState()); };
        setupButton (lockBtn, "LOCK", "Scale lock: notes you draw, drag or stamp snap to the chosen key and scale", true);
        lockBtn.setToggleState (s.settings != nullptr && s.settings->getBoolValue ("prScaleLock", false), juce::dontSendNotification);
        lockBtn.onClick = [this] { if (s.settings) s.settings->setValue ("prScaleLock", lockBtn.getToggleState()); };
        glide.setSkewFactorFromMidPoint (0.15);   // short 808 slides (20-150 ms) get most of the knob's travel
        glide.onValueChange = [this] {
            s.live ([this] (Project&) { if (auto* c = s.channel (channelId)) c->glide = (float) glide.getValue(); });
            if (! glide.isMouseButtonDown()) s.commit ("Glide");
        };
        glide.onDragEnd = [this] { s.commit ("Glide"); };
        chLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        for (juce::Component* c : { (juce::Component*) &rootBox, (juce::Component*) &scaleBox, (juce::Component*) &highlightBtn, (juce::Component*) &snapBox,
                                    (juce::Component*) &toolsBtn, (juce::Component*) &slideBtn, (juce::Component*) &stampBtn, (juce::Component*) &ghostBtn, (juce::Component*) &muteBtn, (juce::Component*) &lockBtn, (juce::Component*) &glide, (juce::Component*) &chLabel })
            addAndMakeVisible (c);
        setWantsKeyboardFocus (true);
        scrollY = (127 - 84) * rowH;
        refresh();
        startTimerHz (30);
    }
    ~PianoRoll() override { s.removeChangeListener (this); }

    juce::String getTooltip() override
    {
        auto g = gridArea();
        if (lastMouse.x < keyW && lastMouse.y > g.getY()) return "Piano keys - click to hear a note";
        if (lastMouse.y > g.getBottom()) return "Velocity - drag the bars up / down";
        if (g.contains (lastMouse))
            return "Click = add note | drag = move | drag right edge = length | right-click = delete | Ctrl-drag = select | double-click = slide | Alt+A rolls, Alt+S strum";
        if (lastMouse.y > barH && lastMouse.y < g.getY()) return "Ruler - click or drag the playhead (Alt = no snap)";
        return {};
    }

    void resized() override
    {
        // flow layout: controls wrap onto as many rows as the width needs, so nothing is ever offscreen or hidden
        struct Item { juce::Component* c; int w, gap; };
        std::vector<Item> items { { &chLabel, 150, 6 }, { &rootBox, 60, 4 }, { &scaleBox, 150, 4 }, { &highlightBtn, 60, 10 },
                                  { &snapBox, 100, 10 }, { &toolsBtn, 70, 4 }, { &slideBtn, 60, 4 }, { &stampBtn, stampChord >= 0 ? 110 : 64, 4 },
                                  { &ghostBtn, 60, 4 }, { &muteBtn, 56, 4 }, { &lockBtn, 52, 10 }, { &glide, 28, 48 } };   // 48: room for the GLIDE label
        for (auto* b : toolBtns) items.push_back ({ b, 46, 2 });
        const int avail = juce::jmax (100, getWidth() - 12);
        auto layout = [&] (bool apply)
        {
            int x = 0, row = 0;
            for (auto& it : items)
            {
                if (x > 0 && x + it.w > avail) { x = 0; ++row; }
                if (apply) it.c->setBounds (6 + x, 4 + row * 30, it.w, 26);
                x += it.w + it.gap;
            }
            return row + 1;
        };
        const int wantH = layout (false) * 30 + 4;
        if (wantH != barH) { barH = wantH; repaint(); }
        layout (true);
    }

    void paint (juce::Graphics& g) override
    {
        auto& p = s.p();
        g.fillAll (col::bg);
        g.setColour (col::panel); g.fillRect (0, 0, getWidth(), barH);
        g.setColour (col::dim); g.setFont (juce::FontOptions (11.0f));
        g.drawText ("GLIDE", glide.getRight() + 2, glide.getY(), 46, glide.getHeight(), juce::Justification::centredLeft);
        auto ga = gridArea();
        auto* ch = s.channel (channelId);
        if (ch == nullptr)
        {
            g.setColour (col::dim);
            g.drawText ("Add a channel in the Channel Rack to start writing notes", ga, juce::Justification::centred);
            return;
        }
        // rows
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (ga);
            const int top = juce::jlimit (0, 127, yToKey ((float) ga.getY())), bottom = juce::jlimit (0, 127, yToKey ((float) ga.getBottom()));
            for (int k = top; k >= bottom; --k)
            {
                const float y = keyToY (k);
                juce::Colour c = isBlack (k) ? col::bg : col::panel;
                if (p.highlightScale && inScale (k, p.keyRoot, p.scaleIndex)) c = ((k - p.keyRoot) % 12 == 0) ? col::root : col::scale;
                g.setColour (c); g.fillRect ((float) ga.getX(), y, (float) ga.getWidth(), rowH);
                g.setColour (col::grid); g.drawHorizontalLine ((int) (y + rowH) - 1, (float) ga.getX(), (float) ga.getRight());
            }
            // vertical lines
            const double b0 = scrollX, b1 = xToBeat ((float) ga.getRight());
            double step = p.snap > 0 ? p.snap : 0.25;
            while (step * pxPerBeat < 6) step *= 2;
            for (double b = std::floor (b0 / step) * step; b <= b1; b += step)
            {
                const bool bar = std::fmod (b + 1e-9, (double) p.beatsPerBar) < 1e-6, beat = std::fmod (b + 1e-9, 1.0) < 1e-6;
                g.setColour (bar ? col::gridBar : beat ? col::gridBeat : col::grid);
                g.drawVerticalLine ((int) beatToX (b), (float) ga.getY(), (float) ga.getBottom());
            }
            const double patLen = p.patterns[(size_t) p.currentPattern].lengthBeats();
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRect (beatToX (patLen), (float) ga.getY(), (float) getWidth(), (float) ga.getHeight());
            // ghost notes
            if (ghostBtn.getToggleState())
                for (auto& [id, list] : p.patterns[(size_t) p.currentPattern].notes)
                    if (id != channelId)
                        for (auto& n : list) { g.setColour (col::dim.withAlpha (0.25f)); g.fillRect (noteRect (n)); }
            // notes
            double playLocal = -1.0;
            if (s.engine.isPlaying())
            {
                const double pos = p.songMode ? s.engine.positionBeats() : std::fmod (s.engine.positionBeats(), patLen);
                playLocal = p.songMode ? -1.0 : pos;
                if (p.songMode)
                    for (auto& c : p.clips)
                        if (c.patternIndex == p.currentPattern && pos >= c.start && pos < c.start + c.length) playLocal = std::fmod (pos - c.start, patLen);
            }
            for (auto& n : s.notes (channelId))
            {
                auto r = noteRect (n);
                if (r.getRight() < ga.getX() || r.getX() > ga.getRight()) continue;
                const bool playing = ! n.muted && playLocal >= n.start && playLocal < n.start + n.length;
                auto c = n.muted ? col::dim : ch->colour.withMultipliedBrightness (0.55f + 0.45f * n.velocity);
                if (playing) { g.setColour (ch->colour.brighter (0.9f).withAlpha (0.35f)); g.fillRoundedRectangle (r.expanded (2.0f), 3.5f); c = c.brighter (0.8f); }
                g.setColour (c); g.fillRoundedRectangle (r, 2.5f);
                g.setColour (n.selected ? juce::Colours::white : juce::Colours::black.withAlpha (0.5f));
                g.drawRoundedRectangle (r, 2.5f, n.selected ? 1.6f : 1.0f);
                if (n.slide)
                {
                    juce::Path tri; tri.addTriangle (r.getRight() - 9, r.getY() + 2, r.getRight() - 2, r.getCentreY(), r.getRight() - 9, r.getBottom() - 2);
                    g.setColour (juce::Colours::white.withAlpha (0.85f)); g.fillPath (tri);
                }
                if (r.getWidth() > 30 && rowH >= 11)
                {
                    g.setColour (juce::Colours::black.withAlpha (0.75f)); g.setFont (juce::FontOptions (10.0f));
                    g.drawText (noteName (n.key), r.reduced (3, 0), juce::Justification::centredLeft);
                }
            }
            if (drag == Drag::Select) { g.setColour (col::accent2.withAlpha (0.15f)); g.fillRect (selRect); g.setColour (col::accent2); g.drawRect (selRect); }
            if (timeSelB > timeSelA)
            {
                const float x0 = juce::jmax ((float) keyW, (float) beatToX (timeSelA)), x1 = (float) beatToX (timeSelB);
                if (x1 > x0) { g.setColour (col::accent.withAlpha (0.08f)); g.fillRect (x0, (float) ga.getY(), x1 - x0, (float) ga.getHeight()); }
            }
            drawPlayhead (g, ga);
        }
        // keyboard
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (0, ga.getY(), keyW, ga.getHeight());
            for (int k = 127; k >= 0; --k)
            {
                const float y = keyToY (k);
                if (y > ga.getBottom() || y + rowH < ga.getY()) continue;
                const bool sel = k == previewKey;
                g.setColour (sel ? col::accent : isBlack (k) ? juce::Colour (0xff202329) : juce::Colour (0xffcfd4dc));
                g.fillRect (0.0f, y, (float) (isBlack (k) ? keyW - 18 : keyW), rowH - 1);
                if (p.highlightScale && inScale (k, p.keyRoot, p.scaleIndex)) { g.setColour (col::good); g.fillRect ((float) keyW - 4, y + 1, 3.0f, rowH - 3); }
                if (k % 12 == 0) { g.setColour (juce::Colours::black); g.setFont (juce::FontOptions (10.0f)); g.drawText (noteName (k), 2, (int) y, keyW - 8, (int) rowH, juce::Justification::centredRight); }
            }
        }
        // ruler
        juce::Rectangle<int> ruler (keyW, barH, getWidth() - keyW, rulerH);
        g.setColour (col::panel2); g.fillRect (ruler);
        g.setColour (col::dim); g.setFont (juce::FontOptions (11.0f));
        for (int bar = (int) (scrollX / p.beatsPerBar); ; ++bar)
        {
            const float x = beatToX (bar * p.beatsPerBar);
            if (x > getWidth()) break;
            g.drawText (juce::String (bar + 1), (int) x + 3, barH, 40, rulerH, juce::Justification::centredLeft);
            g.drawVerticalLine ((int) x, (float) barH, (float) (barH + rulerH));
        }
        if (timeSelB > timeSelA)
        {
            const float x0 = juce::jmax ((float) keyW, (float) beatToX (timeSelA)), x1 = (float) beatToX (timeSelB);
            if (x1 > x0) { g.setColour (col::accent.withAlpha (0.45f)); g.fillRect (x0, (float) barH, x1 - x0, (float) rulerH); }
        }
        if (const double ph = playheadLocal(); ph >= 0.0 && beatToX (ph) >= keyW)   // FL handle: grab it on the bar numbers
        {
            const float px = (float) beatToX (ph);
            juce::Path tri; tri.addTriangle (px - 7, (float) barH + 1, px + 7, (float) barH + 1, px, (float) (barH + 13));
            g.setColour (col::accent2); g.fillPath (tri);
            g.fillRect (px - 0.75f, (float) barH + 10, 1.5f, (float) rulerH - 10);
        }
        // velocity lane
        juce::Rectangle<int> vel (keyW, ga.getBottom(), getWidth() - keyW, velH);
        g.setColour (col::panel); g.fillRect (vel);
        g.setColour (col::gridBar); g.drawHorizontalLine (vel.getY(), 0.0f, (float) getWidth());
        for (auto& n : s.notes (channelId))
        {
            const float x = beatToX (n.start);
            if (x < keyW || x > getWidth()) continue;
            const float h = (velH - 8) * n.velocity;
            g.setColour (n.selected ? juce::Colours::white : ch->colour);
            g.fillRect (x, (float) vel.getBottom() - 4 - h, 3.0f, h);
            g.fillEllipse (x - 2.5f, (float) vel.getBottom() - 6 - h, 8.0f, 5.0f);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        lastMouse = e.getPosition();
        int hit; bool edge;
        hitNote (e.position, hit, edge);
        setMouseCursor (hit >= 0 && edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        auto ga = gridArea();
        dragStart = e.position; changed = false;
        if (s.channel (channelId) == nullptr) return;
        if (e.x < keyW && e.y > ga.getY() && e.y < ga.getBottom()) { drag = Drag::Keys; preview (yToKey (e.position.y)); return; }
        if (e.y > barH && e.y < ga.getY())   // bar numbers: drag = select bars (FL), click or the handle = playhead
        {
            const double ph = playheadLocal();
            if (ph >= 0.0 && std::abs (beatToX (ph) - e.position.x) <= 8.0f) { drag = Drag::Scrub; scrubTo (e); return; }
            drag = Drag::TimeSel; timeMoved = false; return;
        }
        if (e.y >= ga.getBottom()) { drag = Drag::Velocity; setVelocityAt (e.position); return; }
        if (! ga.contains (e.getPosition())) return;
        if (timeSelB > timeSelA) { timeSelA = timeSelB = -1; repaint(); }

        auto& notes = s.notes (channelId);
        int hit; bool edge;
        hitNote (e.position, hit, edge);
        if (e.mods.isPopupMenu()) { drag = Drag::Delete; deleteAt (e.position); return; }
        if (editTool == EditTool::Delete) { drag = Drag::Delete; deleteAt (e.position); return; }
        if (editTool == EditTool::Mute)
        {
            if (hit >= 0) s.edit ("Mute note", [&] (Project&) { auto& n = s.notes (channelId)[(size_t) hit]; n.muted = ! n.muted; });
            return;
        }
        if (editTool == EditTool::Slice)
        {
            const double at = floorSnap (xToBeat (e.position.x));
            if (hit >= 0)
                s.edit ("Slice note", [&] (Project&) {
                    auto& ns = s.notes (channelId);
                    auto n = ns[(size_t) hit];
                    if (at <= n.start + 1e-6 || at >= n.start + n.length - 1e-6) return;
                    auto right = n; right.start = at; right.length = n.start + n.length - at;
                    ns[(size_t) hit].length = at - n.start;
                    ns.push_back (right);
                    tools::sortNotes (ns);
                });
            return;
        }
        if (editTool == EditTool::Zoom)
        {
            const double at = xToBeat (e.position.x);
            pxPerBeat = juce::jlimit (12.0f, 800.0f, pxPerBeat * (e.mods.isShiftDown() ? 1.0f / 1.5f : 1.5f));
            scrollX = juce::jmax (0.0, at - (e.position.x - gridArea().getX()) / pxPerBeat);
            repaint();
            return;
        }
        if (e.mods.isCommandDown() || editTool == EditTool::Select)
        {
            drag = Drag::Select;
            if (! e.mods.isShiftDown()) s.live ([&] (Project&) { tools::selectAll (notes, false); });
            selRect = { e.position, e.position };
            return;
        }
        s.live ([&] (Project&) {
            if (hit >= 0)
            {
                if (! notes[(size_t) hit].selected && ! e.mods.isShiftDown()) tools::selectAll (notes, false);
                notes[(size_t) hit].selected = true;
            }
            else
            {
                tools::selectAll (notes, false);
                Note n; n.key = juce::jlimit (0, 127, yToKey (e.position.y)); n.start = juce::jmax (0.0, floorSnap (xToBeat (e.position.x)));
                n.length = lastLength; n.velocity = lastVelocity; n.slide = slideBtn.getToggleState(); n.selected = true;
                const size_t first = notes.size();
                stamp (notes, n);
                hit = (int) first; edge = false; changed = true;
            }
        });
        if (editTool == EditTool::Paint && changed)
        {
            drag = Drag::Paint; paintLast = notes.back().start;
            preview (notes.back().key);
            repaint();
            return;
        }
        dragNote = hit;
        dragOrig = notes;
        drag = edge ? Drag::Resize : Drag::Move;
        preview (notes[(size_t) hit].key);
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        lastMouse = e.getPosition();
        auto& p = s.p();
        if (drag == Drag::Keys) { const int k = yToKey (e.position.y); if (k != previewKey) preview (k); return; }
        if (drag == Drag::Scrub) { scrubTo (e); return; }
        if (drag == Drag::TimeSel)
        {
            if (! timeMoved && std::abs (e.position.x - dragStart.x) < 4) return;
            timeMoved = true;
            auto at = [&] (float x) { const double b = juce::jmax (0.0, xToBeat (juce::jmax ((float) keyW, x)));
                                      const double sn = s.p().snap > 0 ? s.p().snap : 0.25;
                                      return e.mods.isAltDown() ? b : std::round (b / sn) * sn; };
            const double a = at (dragStart.x), b = at (e.position.x);
            timeSelA = juce::jmin (a, b); timeSelB = juce::jmax (a, b);
            selectTimeRange();
            repaint(); return;
        }
        if (drag == Drag::Velocity) { setVelocityAt (e.position); return; }
        if (drag == Drag::Delete) { deleteAt (e.position); return; }
        if (drag == Drag::Paint)
        {
            const double step = juce::jmax (1.0 / 64, p.snap > 0 ? p.snap : lastLength);
            const double b = juce::jmax (0.0, floorSnap (xToBeat (e.position.x)));
            if (std::abs (b - paintLast) < step - 1e-9) return;
            const int key = juce::jlimit (0, 127, yToKey (e.position.y));
            s.live ([&] (Project&) {
                auto& ns = s.notes (channelId);
                for (auto& n : ns) if (n.key == key && b < n.start + n.length - 1e-9 && b + lastLength > n.start + 1e-9) return;
                Note n; n.key = key; n.start = b; n.length = lastLength; n.velocity = lastVelocity; n.slide = slideBtn.getToggleState();
                stamp (ns, n);
            });
            paintLast = b; changed = true;
            repaint();
            return;
        }
        if (drag == Drag::Select)
        {
            selRect = juce::Rectangle<float> (dragStart, e.position);
            s.live ([&] (Project&) {
                for (auto& n : s.notes (channelId)) if (noteRect (n).intersects (selRect)) n.selected = true; else if (! e.mods.isShiftDown()) n.selected = false;
            });
            repaint();
            return;
        }
        if (drag != Drag::Move && drag != Drag::Resize) return;
        if (dragNote < 0 || dragNote >= (int) dragOrig.size()) return;
        const bool noSnap = e.mods.isAltDown() || p.snap <= 0;
        double dBeat = xToBeat (e.position.x) - xToBeat (dragStart.x);
        if (! noSnap) dBeat = std::round (dBeat / p.snap) * p.snap;
        const int dKey = drag == Drag::Move ? yToKey (e.position.y) - yToKey (dragStart.y) : 0;
        s.live ([&] (Project&) {
            auto& notes = s.notes (channelId);
            if (notes.size() != dragOrig.size()) return;
            for (size_t i = 0; i < notes.size(); ++i)
            {
                if (! dragOrig[i].selected) continue;
                auto n = dragOrig[i];
                if (drag == Drag::Move) { n.start = juce::jmax (0.0, n.start + dBeat); n.key = lockKey (juce::jlimit (0, 127, n.key + dKey)); }
                else n.length = juce::jmax (noSnap ? 1.0 / 64 : p.snap, n.length + dBeat);
                notes[i] = n;
            }
            lastLength = notes[(size_t) dragNote].length;
        });
        changed = true;
        if (drag == Drag::Move && s.notes (channelId)[(size_t) dragNote].key != previewKey) preview (s.notes (channelId)[(size_t) dragNote].key);
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (drag == Drag::TimeSel && ! timeMoved) { timeSelA = timeSelB = -1; scrubTo (e); }
        if (previewKey >= 0) { s.engine.previewNote (channelId, previewKey, 0, false); previewKey = -1; }
        if (changed || drag == Drag::Velocity || drag == Drag::Delete)
            s.edit (drag == Drag::Velocity ? "Velocity" : drag == Drag::Delete ? "Delete notes" : "Edit notes", [this] (Project&) { tools::sortNotes (s.notes (channelId)); });
        drag = Drag::None;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        int hit; bool edge;
        hitNote (e.position, hit, edge);
        if (hit >= 0) s.edit ("Slide note", [&] (Project&) { auto& n = s.notes (channelId)[(size_t) hit]; n.slide = ! n.slide; });
    }

    void mouseMagnify (const juce::MouseEvent& e, float scale) override   // trackpad pinch (Mac / Windows precision touchpads)
    {
        const double at = xToBeat (e.position.x);
        pxPerBeat = juce::jlimit (12.0f, 800.0f, pxPerBeat * scale);
        scrollX = juce::jmax (0.0, at - (e.position.x - gridArea().getX()) / pxPerBeat);
        repaint();
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCommandDown() || e.mods.isCtrlDown())
        {
            const double at = xToBeat (e.position.x);
            pxPerBeat = juce::jlimit (12.0f, 800.0f, pxPerBeat * std::pow (1.15f, juce::jlimit (-4.0f, 4.0f, w.deltaY * 8.0f)));
            scrollX = juce::jmax (0.0, at - (e.position.x - gridArea().getX()) / pxPerBeat);
        }
        else if (e.mods.isAltDown()) rowH = juce::jlimit (6.0f, 30.0f, rowH + (w.deltaY > 0 ? 1.0f : -1.0f));
        else if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
            scrollX = juce::jmax (0.0, scrollX - (w.deltaX != 0 ? w.deltaX : w.deltaY) * 600.0 / pxPerBeat);
        else scrollY = juce::jlimit (0.0f, 128 * rowH - (float) gridArea().getHeight(), scrollY - w.deltaY * 300.0f);
        repaint();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (isTypingNoteKey (k)) return false;
        if (s.channel (channelId) == nullptr) return false;
        const int alt = juce::ModifierKeys::altModifier, cmd = juce::ModifierKeys::commandModifier, shift = juce::ModifierKeys::shiftModifier;
        auto is = [&k] (int key, int mods) { return k == juce::KeyPress (key, mods, 0); };
        const std::pair<int, Tool> altTools[] { { 'a', Tool::Arp }, { 's', Tool::Strum }, { 'u', Tool::Chop }, { 'f', Tool::Flam },
                                                 { 'q', Tool::Quantize }, { 'r', Tool::Randomize }, { 'k', Tool::Limit }, { 'l', Tool::Articulate },
                                                 { 'x', Tool::ScaleLevels }, { 'y', Tool::Flip }, { 'w', Tool::Claw }, { 'o', Tool::Lfo } };
        for (auto& [key, tool] : altTools) if (is (key, alt)) { showToolDialog (s, tool, channelId); return true; }
        if (is ('t', alt)) { stampMenu(); return true; }
        {
            const std::pair<int, EditTool> keys[] { { 'p', EditTool::Draw }, { 'b', EditTool::Paint }, { 'd', EditTool::Delete }, { 't', EditTool::Mute },
                                                    { 'c', EditTool::Slice }, { 'e', EditTool::Select }, { 'z', EditTool::Zoom } };
            for (auto& [key, tool] : keys) if (is (key, 0)) { setTool (tool); return true; }
        }
        const double snap = s.p().snap > 0 ? s.p().snap : 0.25;
        if (is ('m', alt)) return op ("Mute notes", [] (NoteList& n) { tools::muteSelected (n, true); });
        if (is ('m', alt | shift)) return op ("Unmute notes", [] (NoteList& n) { tools::muteSelected (n, false); });
        if (is ('m', 0)) { muteBtn.setToggleState (! muteBtn.getToggleState(), juce::dontSendNotification); muteBtn.onClick(); return true; }
        if (is ('v', alt)) { ghostBtn.setToggleState (! ghostBtn.getToggleState(), juce::dontSendNotification); repaint(); return true; }
        if (is ('b', cmd) && timeSelB > timeSelA)   // duplicate the selected bars right after themselves
        {
            const double a = timeSelA, b = timeSelB;
            timeSelA = b; timeSelB = b + (b - a);
            return op ("Duplicate bars", [a, b] (NoteList& n) {
                NoteList add;
                for (auto& x : n) if (x.start >= a - 1e-9 && x.start < b - 1e-9) { Note c = x; c.start += b - a; c.selected = true; add.push_back (c); }
                tools::selectAll (n, false);
                n.insert (n.end(), add.begin(), add.end());
            });
        }
        if (is ('b', cmd)) return op ("Duplicate", [] (NoteList& n) { tools::duplicateRight (n); });
        if (is ('g', cmd)) return op ("Glue", [] (NoteList& n) { tools::glue (n); });
        if (is ('l', cmd)) return op ("Legato", [] (NoteList& n) { tools::legato (n); });
        if (is ('u', cmd)) return op ("Quick chop", [snap] (NoteList& n) { tools::quickChop (n, snap); });
        if (is ('q', cmd)) return op ("Quick quantize", [snap] (NoteList& n) { tools::QuantizeSettings q; q.grid = snap; tools::quantize (n, q); });
        if (is ('q', shift)) return op ("Quantize starts", [snap] (NoteList& n) { tools::QuantizeSettings q; q.grid = snap; q.ends = false; tools::quantize (n, q); });
        if (is ('a', cmd)) return op ("Select all", [] (NoteList& n) { tools::selectAll (n, true); });
        if (is ('d', cmd)) return op ("Deselect", [] (NoteList& n) { tools::selectAll (n, false); });
        if (is ('d', shift)) return op ("Reset lengths", [snap] (NoteList& n) { tools::discardLengths (n, snap); });
        if (is (juce::KeyPress::upKey, shift)) return op ("Transpose", [] (NoteList& n) { tools::transpose (n, 1); });
        if (is (juce::KeyPress::downKey, shift)) return op ("Transpose", [] (NoteList& n) { tools::transpose (n, -1); });
        if (is (juce::KeyPress::upKey, cmd)) return op ("Octave up", [] (NoteList& n) { tools::transpose (n, 12); });
        if (is (juce::KeyPress::downKey, cmd)) return op ("Octave down", [] (NoteList& n) { tools::transpose (n, -12); });
        if (is (juce::KeyPress::leftKey, shift)) return op ("Move", [snap] (NoteList& n) { tools::move (n, -snap); });
        if (is (juce::KeyPress::rightKey, shift)) return op ("Move", [snap] (NoteList& n) { tools::move (n, snap); });
        if (is (juce::KeyPress::leftKey, alt)) return op ("Nudge", [snap] (NoteList& n) { tools::move (n, -snap / 4); });
        if (is (juce::KeyPress::rightKey, alt)) return op ("Nudge", [snap] (NoteList& n) { tools::move (n, snap / 4); });
        if (is (juce::KeyPress::deleteKey, 0) || is (juce::KeyPress::backspaceKey, 0)) return op ("Delete notes", [] (NoteList& n) { tools::deleteSelected (n); });
        if (is ('c', cmd))
        {
            clipboard().clear();
            for (auto& n : s.notes (channelId)) if (n.selected) clipboard().push_back (n);
            clipFrom() = timeSelB > timeSelA ? timeSelA : -1.0;   // bars copied by number keep their spot in the bar
            clipLen() = timeSelB > timeSelA ? timeSelB - timeSelA : 0.0;
            return true;
        }
        if (is ('v', cmd) && ! clipboard().empty())
        {
            double at = floorSnap (s.engine.isPlaying() ? 0.0 : std::fmod (s.engine.positionBeats(), s.p().patterns[(size_t) s.p().currentPattern].lengthBeats()));
            double first = 1e9;
            for (auto& n : clipboard()) first = std::min (first, n.start);
            if (clipFrom() >= 0.0)
            {
                first = clipFrom();
                if (timeSelB > timeSelA) at = timeSelB;   // highlighted bars: paste right after them
                timeSelA = at; timeSelB = at + clipLen();
            }
            return op ("Paste", [this, at, first] (NoteList& n) {
                tools::selectAll (n, false);
                for (auto c : clipboard()) { c.start = c.start - first + at; c.selected = true; n.push_back (c); }
            });
        }
        return false;
    }

private:
    enum class Drag { None, Move, Resize, Select, Velocity, Delete, Keys, Paint, Scrub, TimeSel };
    enum class EditTool { Draw, Paint, Delete, Mute, Slice, Select, Zoom };
    EditTool editTool = EditTool::Draw;
    juce::OwnedArray<juce::TextButton> toolBtns;
    double paintLast = 0;
    void setTool (EditTool t) { editTool = t; if (auto* b = toolBtns[(int) t]) b->setToggleState (true, juce::dontSendNotification); }
    Studio& s;
    int channelId = -1;
    double scrollX = 0;
    float scrollY = 0, pxPerBeat = 96.0f, rowH = 14.0f;
    static constexpr int keyW = 64, rulerH = 18, velH = 70;
    int barH = 34;   // 64 when the toolbar wraps to two rows
    double lastLength = 0.25; float lastVelocity = 0.78f;
    Drag drag = Drag::None;
    juce::Point<float> dragStart; juce::Point<int> lastMouse;
    NoteList dragOrig; int dragNote = -1; bool changed = false;
    juce::Rectangle<float> selRect;
    int previewKey = -1;
    juce::ComboBox rootBox, scaleBox, snapBox;
    juce::TextButton highlightBtn, toolsBtn, slideBtn, stampBtn, ghostBtn, muteBtn, lockBtn;
    int scaleKey (int k) const { int r = snapToScale (k, s.p().keyRoot, s.p().scaleIndex); if (r < 0) r += 12; if (r > 127) r -= 12; return r; }   // octave-wrap at the edges stays in scale
    int lockKey (int k) const { return lockBtn.getToggleState() ? scaleKey (k) : k; }
    Knob glide { "Glide", "slide time between notes for this channel; 0 = off", 0.0, 1.0, 0.0, " s" };
    juce::Label chLabel;

    static NoteList& clipboard() { static NoteList c; return c; }
    static double& clipFrom() { static double f = -1.0; return f; }
    static double& clipLen() { static double l = 0.0; return l; }
    double timeSelA = -1, timeSelB = -1; bool timeMoved = false;
    void selectTimeRange()
    {
        s.live ([this] (Project&) { for (auto& n : s.notes (channelId)) n.selected = n.start >= timeSelA - 1e-9 && n.start < timeSelB - 1e-9; });
    }

    juce::Rectangle<int> gridArea() const { return getLocalBounds().withTrimmedTop (barH + rulerH).withTrimmedLeft (keyW).withTrimmedBottom (velH); }
    float beatToX (double b) const { return (float) gridArea().getX() + (float) ((b - scrollX) * pxPerBeat); }
    double xToBeat (float x) const { return scrollX + (x - (float) gridArea().getX()) / pxPerBeat; }
    float keyToY (int key) const { return (float) gridArea().getY() + (127 - key) * rowH - scrollY; }
    int yToKey (float y) const { return 127 - (int) std::floor ((y - (float) gridArea().getY() + scrollY) / rowH); }
    double floorSnap (double b) const { const double g = const_cast<Studio&> (s).p().snap; return g > 0 ? std::floor (b / g + 1e-9) * g : b; }
    juce::Rectangle<float> noteRect (const Note& n) const
    {
        return { beatToX (n.start), keyToY (n.key) + 1.0f, juce::jmax (3.0f, (float) (n.length * pxPerBeat) - 1.0f), rowH - 2.0f };
    }
    void hitNote (juce::Point<float> pt, int& hit, bool& edge)
    {
        hit = -1; edge = false;
        if (s.channel (channelId) == nullptr || ! gridArea().toFloat().contains (pt)) return;
        auto& notes = s.notes (channelId);
        for (int i = (int) notes.size() - 1; i >= 0; --i)
        {
            auto r = noteRect (notes[(size_t) i]);
            if (r.contains (pt)) { hit = i; edge = pt.x > r.getRight() - juce::jmin (8.0f, r.getWidth() * 0.3f); return; }
        }
    }
    void preview (int key)
    {
        if (previewKey >= 0) s.engine.previewNote (channelId, previewKey, 0, false);
        previewKey = -1;
        if (muteBtn.getToggleState()) return;
        previewKey = juce::jlimit (0, 127, key);
        s.engine.previewNote (channelId, previewKey, 0.8f, true);
        repaint (0, gridArea().getY(), keyW, gridArea().getHeight());
    }
    void deleteAt (juce::Point<float> pt)
    {
        int hit; bool edge;
        hitNote (pt, hit, edge);
        if (hit < 0) return;
        s.live ([&] (Project&) { auto& n = s.notes (channelId); n.erase (n.begin() + hit); });
        changed = true; repaint();
    }
    void setVelocityAt (juce::Point<float> pt)
    {
        const auto ga = gridArea();
        const float v = juce::jlimit (0.0f, 1.0f, ((float) (ga.getBottom() + velH - 4) - pt.y) / (float) (velH - 8));
        s.live ([&] (Project&) {
            auto& notes = s.notes (channelId);
            const bool anySel = tools::anySelected (notes);
            for (auto& n : notes)
            {
                const float x = beatToX (n.start);
                if (std::abs (x - pt.x) < 5.0f && (! anySel || n.selected)) { n.velocity = v; lastVelocity = v; }
            }
        });
        repaint();
    }
    bool op (const juce::String& name, std::function<void (NoteList&)> f)
    {
        s.edit (name, [&] (Project&) { auto& n = s.notes (channelId); f (n); tools::sortNotes (n); });
        return true;
    }
    void scrubTo (const juce::MouseEvent& e)   // Alt = free, otherwise snaps to the grid (FL)
    {
        const double b = juce::jmax (0.0, xToBeat (juce::jmax ((float) keyW, e.position.x)));
        const double sn = s.p().snap > 0 ? s.p().snap : 0.25;
        s.engine.setPositionBeats (e.mods.isAltDown() ? b : std::round (b / sn) * sn);
        repaint();
    }
    double playheadLocal() const
    {
        auto& p = s.p();
        const double len = p.patterns[(size_t) p.currentPattern].lengthBeats();
        const double pos = p.songMode ? s.engine.positionBeats() : std::fmod (s.engine.positionBeats(), len);
        double local = p.songMode ? -1.0 : pos;
        if (p.songMode)
            for (auto& c : p.clips)
                if (c.patternIndex == p.currentPattern && pos >= c.start && pos < c.start + c.length) local = std::fmod (pos - c.start, len);
        return local;
    }
    void drawPlayhead (juce::Graphics& g, juce::Rectangle<int> ga)
    {
        auto& p = s.p();
        const double len = p.patterns[(size_t) p.currentPattern].lengthBeats();
        const double pos = p.songMode ? s.engine.positionBeats() : std::fmod (s.engine.positionBeats(), len);
        double local = p.songMode ? -1.0 : pos;
        if (p.songMode)
            for (auto& c : p.clips)
                if (c.patternIndex == p.currentPattern && pos >= c.start && pos < c.start + c.length) local = std::fmod (pos - c.start, len);
        if (local < 0) return;
        const float x = (float) beatToX (local);
        g.setColour (col::accent2.withAlpha (0.22f)); g.fillRect (x - 2.5f, (float) ga.getY(), 5.0f, (float) ga.getHeight());
        g.setColour (col::accent2); g.fillRect (x - 0.75f, (float) ga.getY(), 1.5f, (float) ga.getHeight());
    }
    // FL-style chord stamp
    struct ChordDef { const char* name; std::vector<int> iv; };
    static const std::vector<ChordDef>& chords()
    {
        static const std::vector<ChordDef> c {
            { "Major", { 0, 4, 7 } }, { "Minor", { 0, 3, 7 } }, { "Dim", { 0, 3, 6 } }, { "Aug", { 0, 4, 8 } },
            { "Sus2", { 0, 2, 7 } }, { "Sus4", { 0, 5, 7 } }, { "6", { 0, 4, 7, 9 } }, { "m6", { 0, 3, 7, 9 } },
            { "7", { 0, 4, 7, 10 } }, { "Maj7", { 0, 4, 7, 11 } }, { "m7", { 0, 3, 7, 10 } }, { "m7b5", { 0, 3, 6, 10 } },
            { "Dim7", { 0, 3, 6, 9 } }, { "mMaj7", { 0, 3, 7, 11 } }, { "add9", { 0, 4, 7, 14 } }, { "m(add9)", { 0, 3, 7, 14 } },
            { "9", { 0, 4, 7, 10, 14 } }, { "Maj9", { 0, 4, 7, 11, 14 } }, { "m9", { 0, 3, 7, 10, 14 } }, { "11", { 0, 4, 7, 10, 14, 17 } },
            { "m11", { 0, 3, 7, 10, 14, 17 } }, { "13", { 0, 4, 7, 10, 14, 21 } }, { "Power (5)", { 0, 7, 12 } }, { "Octave", { 0, 12 } } };
        return c;
    }
    int stampChord = -1, stampInv = 0;
    bool stampLock = false;
    void stamp (NoteList& ns, const Note& n)
    {
        if (stampChord < 0 || stampChord >= (int) chords().size()) { Note c = n; c.key = lockKey (c.key); ns.push_back (c); return; }
        auto iv = chords()[(size_t) stampChord].iv;
        for (int i = 0; i < stampInv && i < (int) iv.size() - 1; ++i) iv[(size_t) i] += 12;
        std::sort (iv.begin(), iv.end());
        std::vector<int> used;
        for (int d : iv)
        {
            int k = n.key + d;
            k = juce::jlimit (0, 127, k);
            if (stampLock || lockBtn.getToggleState()) k = scaleKey (k);
            if (std::find (used.begin(), used.end(), k) != used.end()) continue;
            used.push_back (k);
            Note c = n; c.key = k; c.slide = c.slide && d == iv.front();
            ns.push_back (c);
        }
    }
    void stampMenu()
    {
        auto& p = s.p();
        juce::PopupMenu m, inv;
        m.addItem (1, "Off (single notes)", true, stampChord < 0);
        m.addSectionHeader ("Chord stamp");
        for (int i = 0; i < (int) chords().size(); ++i) m.addItem (100 + i, chords()[(size_t) i].name, true, stampChord == i);
        m.addSeparator();
        for (int i = 0; i < 4; ++i) inv.addItem (10 + i, i == 0 ? juce::String ("Root position") : juce::String (i) + (i == 1 ? "st" : i == 2 ? "nd" : "rd") + " inversion", true, stampInv == i);
        m.addSubMenu ("Inversion", inv);
        const auto sc = p.scaleIndex >= 0 && p.scaleIndex < (int) allScales().size() ? juce::String (allScales()[(size_t) p.scaleIndex].name) : juce::String();
        m.addItem (2, "Lock to scale (" + juce::String (rootNames[juce::jlimit (0, 11, p.keyRoot)]) + " " + sc + ")", true, stampLock);
        juce::Component::SafePointer<PianoRoll> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&stampBtn), [safe] (int r) {
            if (safe == nullptr || r <= 0) return;
            if (r == 1) safe->stampChord = -1;
            else if (r == 2) safe->stampLock = ! safe->stampLock;
            else if (r >= 10 && r < 14) safe->stampInv = r - 10;
            else if (r >= 100) safe->stampChord = r - 100;
            const bool on = safe->stampChord >= 0;
            safe->stampBtn.setButtonText (on ? "STAMP: " + juce::String (chords()[(size_t) safe->stampChord].name) : juce::String ("STAMP"));
            safe->stampBtn.setToggleState (on, juce::dontSendNotification);
            safe->resized();
        });
    }
    void toolsMenu()
    {
        juce::PopupMenu m;
        const std::pair<Tool, const char*> items[] { { Tool::Arp, "Arpeggiator / snare + hi-hat rolls\tAlt+A" }, { Tool::Strum, "Strum (offset)\tAlt+S" },
            { Tool::Chop, "Chop\tAlt+U" }, { Tool::Flam, "Flam\tAlt+F" }, { Tool::Quantize, "Quantize\tAlt+Q" }, { Tool::Randomize, "Randomize in key\tAlt+R" },
            { Tool::Limit, "Limit / fold to scale\tAlt+K" }, { Tool::Articulate, "Articulate\tAlt+L" }, { Tool::ScaleLevels, "Scale velocities\tAlt+X" },
            { Tool::Flip, "Flip\tAlt+Y" }, { Tool::Claw, "Claw machine\tAlt+W" }, { Tool::Lfo, "LFO velocity\tAlt+O" } };
        for (auto& [t, name] : items)
        {
            const juce::StringArray parts = juce::StringArray::fromTokens (name, "\t", {});
            juce::PopupMenu::Item it (parts[0]); it.itemID = (int) t + 1; it.shortcutKeyDescription = parts[1]; m.addItem (it);
        }
        m.addSeparator();
        m.addItem (100, "Glue (Ctrl+G)"); m.addItem (101, "Legato (Ctrl+L)"); m.addItem (102, "Quick chop (Ctrl+U)"); m.addItem (103, "Duplicate (Ctrl+B)");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&toolsBtn), [this] (int r) {
            if (r >= 1 && r <= 12) showToolDialog (s, (Tool) (r - 1), channelId);
            else if (r == 100) op ("Glue", [] (NoteList& n) { tools::glue (n); });
            else if (r == 101) op ("Legato", [] (NoteList& n) { tools::legato (n); });
            else if (r == 102) op ("Quick chop", [this] (NoteList& n) { tools::quickChop (n, s.p().snap > 0 ? s.p().snap : 0.25); });
            else if (r == 103) op ("Duplicate", [] (NoteList& n) { tools::duplicateRight (n); });
        });
    }
    void refresh()
    {
        auto& p = s.p();
        if (s.channel (s.selectedChannel) != nullptr) channelId = s.selectedChannel;
        else if (s.channel (channelId) == nullptr) channelId = p.channels.empty() ? -1 : p.channels.front().id;
        rootBox.setSelectedItemIndex (p.keyRoot, juce::dontSendNotification);
        scaleBox.setSelectedItemIndex (p.scaleIndex, juce::dontSendNotification);
        highlightBtn.setToggleState (p.highlightScale, juce::dontSendNotification);
        int si = 5;
        for (int i = 0; i < kNumSnaps; ++i) if (std::abs (snapValues[i] - p.snap) < 1e-9) si = i;
        snapBox.setSelectedItemIndex (si, juce::dontSendNotification);
        if (auto* c = s.channel (channelId))
        {
            chLabel.setText (c->name, juce::dontSendNotification);
            chLabel.setColour (juce::Label::textColourId, c->colour);
            glide.setValue (c->glide, juce::dontSendNotification);
        }
        repaint();
    }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override
    {
        if (! isShowing()) return;
        const bool playing = s.engine.isPlaying();
        if (playing || wasPlaying) repaint (keyW, barH, getWidth() - keyW, getHeight() - barH);   // grid + ruler + velocity only, not the toolbar / keyboard
        wasPlaying = playing;
    }
    bool wasPlaying = false;
};
}

std::unique_ptr<juce::Component> createPianoRoll (Studio& s) { return std::make_unique<PianoRoll> (s); }
}
