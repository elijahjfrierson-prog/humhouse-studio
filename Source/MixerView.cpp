#include "UI.h"

namespace hhs
{
namespace
{
constexpr int stripW = 78, fxPanelW = 280, maxSlots = 10;

struct Strip : public juce::Component
{
    Strip (Studio& st, int i) : s (st), index (i)
    {
        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        fader.setRange (0.0, 1.25, 0.0);
        fader.setDoubleClickReturnValue (true, 0.8);
        fader.setTooltip ("Volume (double-click = default)");
        fader.setPopupDisplayEnabled (true, true, nullptr, 1500);
        fader.textFromValueFunction = [] (double v) { return v <= 0.0001 ? juce::String ("-inf dB") : juce::String (juce::Decibels::gainToDecibels ((v / 0.8) * (v / 0.8)), 1) + " dB"; };
        fader.setName ("Volume");
        fader.onValueChange = [this] { liveSet ([this] (MixerTrack& t) { t.volume = (float) fader.getValue(); }); if (! fader.isMouseButtonDown()) s.commit ("Volume"); };
        fader.onDragEnd = [this] { s.commit ("Volume"); };
        pan.onValueChange = [this] { liveSet ([this] (MixerTrack& t) { t.pan = (float) pan.getValue(); }); if (! pan.isMouseButtonDown()) s.commit ("Pan"); };
        pan.onDragEnd = [this] { s.commit ("Pan"); };
        duck.onValueChange = [this] { liveSet ([this] (MixerTrack& t) { t.duck = (float) duck.getValue(); }); if (! duck.isMouseButtonDown()) s.commit ("Duck"); };
        duck.onDragEnd = [this] { s.commit ("Duck"); };
        sep.onValueChange = [this] { liveSet ([this] (MixerTrack& t) { t.stereoSep = (float) sep.getValue(); }); if (! sep.isMouseButtonDown()) s.commit ("Stereo separation"); };
        sep.onDragEnd = [this] { s.commit ("Stereo separation"); };
        addAndMakeVisible (sep);
        setupButton (scBtn, "SC", "Sidechain: pick the source (default = armed vocal) + ratio / threshold / attack / release");
        scBtn.onClick = [this] { scMenu(); };
        addAndMakeVisible (duck); addAndMakeVisible (scBtn);
        setupButton (mute, "M", "Mute insert", true);
        mute.setColour (juce::TextButton::buttonOnColourId, col::dim);
        mute.onClick = [this] { const bool on = mute.getToggleState(); s.edit ("Mute", [&] (Project& p) { p.mixer[(size_t) index].muted = on; }); };
        setupButton (solo, "S", "Solo insert", true);
        solo.setColour (juce::TextButton::buttonOnColourId, col::good);
        solo.onClick = [this] { const bool on = solo.getToggleState(); s.edit ("Solo", [&] (Project& p) { p.mixer[(size_t) index].solo = on; }); };
        setupButton (route, juce::String::fromUTF8 ("\xe2\x96\xb2"), "Click-to-route: send the SELECTED insert into this one (click again to remove)");
        route.onClick = [this] {
            const int from = s.selectedTrack;
            bool ok = true;
            s.edit ("Route", [&] (Project& p) { ok = p.toggleRoute (from, index); });
            if (! ok) juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Routing", "That route would create a feedback loop.");
        };
        name.setJustificationType (juce::Justification::centred);
        name.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        name.setEditable (false, true);
        name.setTooltip ("Insert name (double-click to rename). Click the strip to select it and edit its FX");
        name.onTextChange = [this] { const auto t = name.getText(); s.edit ("Rename insert", [&] (Project& p) { p.mixer[(size_t) index].name = t; }); };
        for (juce::Component* c : { (juce::Component*) &fader, (juce::Component*) &pan, (juce::Component*) &mute, (juce::Component*) &solo,
                                    (juce::Component*) &route, (juce::Component*) &name })
            addAndMakeVisible (c);
        if (index == 0) { route.setVisible (false); }
        refresh();
    }
    void liveSet (std::function<void (MixerTrack&)> f) { s.live ([&] (Project& p) { if (index < (int) p.mixer.size()) f (p.mixer[(size_t) index]); }); }
    void refresh()
    {
        auto& p = s.p();
        if (index >= (int) p.mixer.size()) return;
        auto& t = p.mixer[(size_t) index];
        fader.setValue (t.volume, juce::dontSendNotification);
        pan.setValue (t.pan, juce::dontSendNotification);
        sep.setValue (t.stereoSep, juce::dontSendNotification);
        duck.setValue (t.duck, juce::dontSendNotification);
        duck.setVisible (index > 0); scBtn.setVisible (index > 0);
        mute.setToggleState (t.muted, juce::dontSendNotification);
        solo.setToggleState (t.solo, juce::dontSendNotification);
        name.setText (index == 0 ? juce::String ("Master") : t.name, juce::dontSendNotification);
        bool routed = false;
        if (s.selectedTrack > 0 && s.selectedTrack < (int) p.mixer.size() && s.selectedTrack != index)
        {
            auto& sel = p.mixer[(size_t) s.selectedTrack];
            routed = index == 0 ? sel.routesToMaster : std::any_of (sel.sends.begin(), sel.sends.end(), [this] (const Send& x) { return x.target == index; });
        }
        route.setToggleState (routed, juce::dontSendNotification);
        route.setEnabled (index != s.selectedTrack);
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        auto& p = s.p();
        const bool sel = s.selectedTrack == index;
        g.setColour (sel ? col::panel2.brighter (0.1f) : col::panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (2), 4);
        if (sel) { g.setColour (col::accent); g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (2), 4, 1.5f); }
        g.setColour (col::dim); g.setFont (juce::FontOptions (11.0f));
        g.drawText (index == 0 ? juce::String ("M") : juce::String (index), 0, 4, getWidth(), 14, juce::Justification::centred);
        if (index < (int) p.mixer.size() && p.mixer[(size_t) index].colour != juce::Colour (0xff2a2f36))
        {
            g.setColour (p.mixer[(size_t) index].colour); g.fillRoundedRectangle (6.0f, 2.0f, (float) getWidth() - 12.0f, 3.0f, 1.5f);
        }
        if (index < (int) p.mixer.size() && p.mixer[(size_t) index].frozen)
        {
            g.setColour (col::accent2); g.drawText ("FROZEN", 0, 18, getWidth(), 12, juce::Justification::centred);
        }
        paintScope (g);
        // meter
        auto m = meterArea();
        g.setColour (col::bg); g.fillRect (m);
        const float db = juce::Decibels::gainToDecibels (level, -60.0f);
        const float h = (float) m.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
        g.setGradientFill (col::levelGradient ((float) m.getX(), (float) m.getY(), (float) m.getBottom()));
        g.fillRect ((float) m.getX(), (float) m.getBottom() - h, (float) m.getWidth(), h);
        const int nfx = index < (int) p.mixer.size() ? (int) std::count_if (p.mixer[(size_t) index].fx.begin(), p.mixer[(size_t) index].fx.end(), [] (const FxSlot& f) { return f.type.isNotEmpty(); }) : 0;
        if (nfx > 0)
        {
            g.setColour (col::accent); g.setFont (juce::FontOptions (10.0f));
            g.drawText (juce::String (nfx) + " FX", fxLabel, juce::Justification::centred);
        }
    }
    juce::Rectangle<int> meterArea() const { return { getWidth() - 16, 96, 8, getHeight() - 96 - 96 }; }
    juce::Rectangle<int> scopeArea, fxLabel;
    void paintScope (juce::Graphics& g)
    {
        auto a = scopeArea.toFloat();
        g.setColour (col::bg); g.fillRoundedRectangle (a, 3);
        auto* tap = s.engine.trackScope (index);
        if (tap == nullptr || a.getWidth() < 8) return;
        float buf[512];
        tap->read (buf, 512, tap->pos.load (std::memory_order_acquire));
        juce::Path p; const float mid = a.getCentreY(), hh = a.getHeight() * 0.48f;
        for (int i = 0; i < 512; ++i)
        {
            const float x = a.getX() + a.getWidth() * (float) i / 511.0f, y = mid - juce::jlimit (-1.0f, 1.0f, buf[i]) * hh;
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setGradientFill (col::mirrorGradient (0, mid - hh, mid + hh, 0.25f)); g.strokePath (p, juce::PathStrokeType (3.0f));
        g.setGradientFill (col::mirrorGradient (0, mid - hh, mid + hh)); g.strokePath (p, juce::PathStrokeType (1.0f));
    }
    void scMenu()
    {
        auto& p = s.p();
        if (index <= 0 || index >= (int) p.mixer.size()) return;
        const auto& t = p.mixer[(size_t) index];
        juce::PopupMenu m, src, ratio, thr, att, rel;
        src.addItem (1, "Auto (armed vocal track)", true, t.duckSource < 0);
        for (int i = 1; i < (int) p.mixer.size(); ++i) if (i != index) src.addItem (100 + i, p.mixer[(size_t) i].name, true, t.duckSource == i);
        m.addSubMenu ("SC from", src);
        for (int r : { 2, 4, 8, 20 }) ratio.addItem (200 + r, juce::String (r) + ":1", true, (int) std::round (t.duckRatio) == r);
        m.addSubMenu ("Ratio", ratio);
        for (int d : { 50, 40, 30, 20 }) thr.addItem (2000 + d, "-" + juce::String (d) + " dB", true, (int) std::round (-t.duckThreshold) == d);
        m.addSubMenu ("Threshold", thr);
        for (int a : { 1, 5, 10, 30 }) att.addItem (300 + a, juce::String (a) + " ms", true, (int) std::round (t.duckAttack) == a);
        m.addSubMenu ("Attack", att);
        for (int r : { 50, 150, 300, 600 }) rel.addItem (1000 + r, juce::String (r) + " ms", true, (int) std::round (t.duckRelease) == r);
        m.addSubMenu ("Release", rel);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&scBtn), [this] (int r) {
            if (r <= 0) return;
            s.edit ("Sidechain", [&] (Project& pr) {
                if (index >= (int) pr.mixer.size()) return;
                auto& tr = pr.mixer[(size_t) index];
                if (r == 1) tr.duckSource = -1;
                else if (r < 200) tr.duckSource = r - 100;
                else if (r < 300) tr.duckRatio = (float) (r - 200);
                else if (r < 1000) tr.duckAttack = (float) (r - 300);
                else if (r < 2000) tr.duckRelease = (float) (r - 1000);
                else tr.duckThreshold = (float) -(r - 2000);
            });
        });
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (scopeArea.contains (e.getPosition())) openTrackScopeWindow (s, index);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        r.removeFromTop (28);
        pan.setBounds (r.removeFromTop (32).withSizeKeepingCentre (32, 32));
        { auto row = r.removeFromTop (30); duck.setBounds (row.removeFromLeft (row.getWidth() / 2).withSizeKeepingCentre (28, 28)); scBtn.setBounds (row.reduced (2, 5)); }
        scopeArea = r.removeFromTop (28).reduced (0, 2);
        auto bottom = r.removeFromBottom (120);
        fader.setBounds (r.withTrimmedRight (14));
        auto ms = bottom.removeFromTop (22);
        mute.setBounds (ms.removeFromLeft (ms.getWidth() / 2).reduced (1)); solo.setBounds (ms.reduced (1));
        fxLabel = bottom.removeFromTop (14);
        sep.setBounds (bottom.removeFromTop (30).withSizeKeepingCentre (28, 28));
        name.setBounds (bottom.removeFromTop (22));
        route.setBounds (bottom.removeFromTop (24).reduced (12, 2));
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        s.selectedTrack = index;
        s.sendChangeMessage();
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            auto& t = s.p().mixer[(size_t) index];
            m.addItem (1, t.frozen ? "Unfreeze" : "Freeze (render to audio, frees CPU)", index != 0);
            m.addItem (2, "Rename...");
            m.addItem (3, "Export this insert as a stem...");
            m.addSeparator();
            m.addItem (4, "Clear all FX");
            m.addSubMenu ("Presets", trackPresetMenu (s, index));
            m.addSubMenu ("Color", colourMenu (30));
            m.showMenuAsync ({}, [this] (int r) {
                if (r == 1)
                {
                    if (s.p().mixer[(size_t) index].frozen) s.edit ("Unfreeze", [this] (Project& p) { p.mixer[(size_t) index].frozen = false; });
                    else { s.engine.stop(); if (s.engine.freezeTrack (index)) s.commit ("Freeze"); }
                }
                else if (r == 2) name.showEditor();
                else if (r == 4) s.edit ("Clear FX", [this] (Project& p) { p.mixer[(size_t) index].fx.clear(); });
                else if (r == 3) juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Stem export", "Use File > Export and pick \"Selected track only\" or \"All tracks as stems\".");
                else if (r >= 30 && r <= 42) { const auto c = pickedColour (r - 30); s.edit ("Insert color", [this, c] (Project& p) { p.mixer[(size_t) index].colour = c; }); }
            });
        }
    }
    void tick()
    {
        const float pk = s.engine.trackPeak (index);
        level = juce::jmax (pk, level * 0.85f);
        repaint (meterArea()); repaint (scopeArea);
    }
    Studio& s; int index; float level = 0;
    juce::Slider fader;
    Knob pan { "Pan", "left / right", -1.0, 1.0, 0.0 };
    Knob sep { "M/S", "stereo separation (FL): left = merge to mono, right = separate / wider. Double-click = 0", -1.0, 1.0, 0.0 };
    Knob duck { "Duck", "sidechain: dips this insert under the SC source (e.g. beat under vocals). 0 = off", 0.0, 1.0, 0.0 };
    juce::TextButton scBtn;
    juce::TextButton mute, solo, route;
    juce::Label name;
};

struct SlotButton : public juce::TextButton
{
    std::function<void (bool popup)> onPress;
    float load = 0.0f;   // share of the audio block (CPU) this slot uses
    std::function<void (juce::Point<int>)> onDragMove;   // parent coords
    std::function<void (juce::Point<int>)> onDrop;
    bool dragging = false;
    void clicked (const juce::ModifierKeys& m) override { if (! dragging && onPress) onPress (m.isPopupMenu()); }
    void mouseDown (const juce::MouseEvent& e) override { dragging = false; juce::TextButton::mouseDown (e); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging && e.getDistanceFromDragStart() > 6 && ! e.mods.isPopupMenu() && onDragMove) dragging = true;
        if (dragging) onDragMove (e.getEventRelativeTo (getParentComponent()).getPosition());
        else juce::TextButton::mouseDrag (e);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseUp (e);
        if (dragging && onDrop) onDrop (e.getEventRelativeTo (getParentComponent()).getPosition());
        dragging = false;
    }
    void paint (juce::Graphics& g) override
    {
        juce::TextButton::paint (g);
        if (load < 0.005f) return;
        g.setColour ((load > 0.25f ? col::rec : load > 0.1f ? col::accent : col::good).withAlpha (0.95f));
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText (juce::String (juce::roundToInt (load * 100.0f)) + "%", getLocalBounds().reduced (5, 0), juce::Justification::centredRight);
    }
};

class FxPanel : public juce::Component
{
public:
    void updateLoads()
    {
        auto& p = s.p();
        const int t = track();
        if (t < 0 || t >= (int) p.mixer.size()) return;
        auto& fx = p.mixer[(size_t) t].fx;
        for (int i = 0; i < slots.size(); ++i)
        {
            const float l = i < (int) fx.size() ? s.engine.fxLoad (t, fx[(size_t) i].uid) : 0.0f;
            if (std::abs (l - slots[i]->load) > 0.005f) { slots[i]->load = l; slots[i]->repaint(); }
        }
    }
    explicit FxPanel (Studio& st) : s (st)
    {
        title.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, col::accent);
        addAndMakeVisible (title);
        for (int i = 0; i < maxSlots; ++i)
        {
            auto* b = slots.add (new SlotButton());
            b->setTooltip ("FX slot: click empty = open Plugin House, click = open editor, drag = move between slots (drop on an effect = replace it), right-click = bypass / replace / move / remove");
            b->onPress = [this, i] (bool popup) { slotPressed (i, popup); };
            b->onDragMove = [this, i] (juce::Point<int> pt) { if (i < (int) s.p().mixer[(size_t) track()].fx.size()) { dragFrom = i; dropAt = dropTarget (pt.y); repaint(); } };
            b->onDrop = [this, i] (juce::Point<int> pt) { const auto d = dropTarget (pt.y); dragFrom = -1; repaint(); dropEffect (i, d); };
            addAndMakeVisible (b);
            auto* byp = bypass.add (new juce::TextButton());
            setupButton (*byp, {}, "Slot on / bypass", true);
            byp->setColour (juce::TextButton::buttonOnColourId, col::good);
            byp->onClick = [this, i, byp] {
                const bool on = byp->getToggleState();
                s.edit ("Bypass", [&] (Project& p) { auto& fx = p.mixer[(size_t) track()].fx; if (i < (int) fx.size()) fx[(size_t) i].bypass = ! on; });
            };
            addAndMakeVisible (byp);
            auto* mx = mixes.add (new Knob ("Mix", "dry / wet for this slot", 0.0, 1.0, 1.0));
            mx->onValueChange = [this, i, mx] {
                s.live ([&] (Project& p) { auto& fx = p.mixer[(size_t) track()].fx; if (i < (int) fx.size()) fx[(size_t) i].mix = (float) mx->getValue(); });
                if (! mx->isMouseButtonDown()) s.commit ("Mix");
            };
            mx->onDragEnd = [this] { s.commit ("Mix"); };
            addAndMakeVisible (mx);
        }
        setupButton (freeze, "FREEZE", "Render this insert to audio and switch its plugins off - saves CPU in big sessions", true);
        freeze.setColour (juce::TextButton::buttonOnColourId, col::accent2);
        freeze.onClick = [this] {
            const int t = track();
            if (! freeze.getToggleState()) s.edit ("Unfreeze", [t] (Project& p) { p.mixer[(size_t) t].frozen = false; });
            else { s.engine.stop(); if (s.engine.freezeTrack (t)) s.commit ("Freeze"); else freeze.setToggleState (false, juce::dontSendNotification); }
        };
        addAndMakeVisible (freeze);
        setupButton (house, "PLUGIN HOUSE", "Add an effect or plugin to this insert");
        house.onClick = [this] { showPluginHouseMenu (s, track()); };
        addAndMakeVisible (house);
        eqLabel.setText ("MINI EQ  (5 bands: freq / gain / Q)", juce::dontSendNotification);
        eqLabel.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        eqLabel.setColour (juce::Label::textColourId, col::accent);
        addAndMakeVisible (eqLabel);
        static const char* bandName[] { "Low", "Lo-Mid", "Mid", "Hi-Mid", "High" };
        for (int b = 0; b < 5; ++b)
            for (int k = 0; k < 3; ++k)
            {
                auto* kn = eqKnobs.add (k == 0 ? new Knob ("Freq", juce::String (bandName[b]) + " band frequency (Hz)", 20.0, 20000.0, 1000.0, " Hz")
                                      : k == 1 ? new Knob ("Gain", juce::String (bandName[b]) + " band boost / cut (dB)", -18.0, 18.0, 0.0, " dB")
                                               : new Knob ("Q", juce::String (bandName[b]) + " band width: low = wide, high = narrow", 0.1, 18.0, b == 0 || b == 4 ? 0.7 : 1.0));
                if (k == 0) kn->setSkewFactorFromMidPoint (630.0);
                if (k == 2) kn->setSkewFactorFromMidPoint (1.5);
                kn->onValueChange = [this, b, k, kn] {
                    s.live ([&] (Project& p) {
                        auto& mt = p.mixer[(size_t) track()];
                        (k == 0 ? mt.meqFreq : k == 1 ? mt.meqGain : mt.meqQ)[(size_t) b] = (float) kn->getValue(); });
                    if (! kn->isMouseButtonDown()) s.commit ("Mini EQ");
                };
                kn->onDragEnd = [this] { s.commit ("Mini EQ"); };
                addAndMakeVisible (kn);
            }
        sendsLabel.setText ("SENDS (click-to-route arrows on the strips)", juce::dontSendNotification);
        sendsLabel.setFont (juce::FontOptions (11.0f));
        sendsLabel.setColour (juce::Label::textColourId, col::dim);
        addAndMakeVisible (sendsLabel);
    }
    int fixedTrack = -1;   // Patch Method: always this insert
    int track() const { return juce::jlimit (0, juce::jmax (0, (int) const_cast<Studio&> (s).p().mixer.size() - 1), fixedTrack >= 0 ? fixedTrack : s.selectedTrack); }
    void refresh()
    {
        auto& p = s.p();
        if (p.mixer.empty()) return;
        auto& t = p.mixer[(size_t) track()];
        title.setText ((track() == 0 ? juce::String ("Master") : t.name), juce::dontSendNotification);
        const auto& infos = builtinEffects();
        for (int i = 0; i < maxSlots; ++i)
        {
            const bool has = i < (int) t.fx.size() && t.fx[(size_t) i].type.isNotEmpty();
            juce::String label = "(empty)";
            if (has)
            {
                auto& f = t.fx[(size_t) i];
                if (f.type == "plugin")
                {
                    label = f.pluginId.upToFirstOccurrenceOf ("-", false, false);
                    for (auto& d : s.engine.knownPlugins.getTypes()) if (d.createIdentifierString() == f.pluginId) label = d.name;
                }
                else for (auto& inf : infos) if (f.type == inf.id) label = inf.name;
            }
            slots[i]->setButtonText (juce::String (i + 1) + "  " + label);
            slots[i]->setColour (juce::TextButton::textColourOffId, has ? col::text : col::dim);
            bypass[i]->setVisible (has); mixes[i]->setVisible (has);
            if (has)
            {
                bypass[i]->setToggleState (! t.fx[(size_t) i].bypass, juce::dontSendNotification);
                mixes[i]->setValue (t.fx[(size_t) i].mix, juce::dontSendNotification);
            }
        }
        freeze.setToggleState (t.frozen, juce::dontSendNotification);
        freeze.setEnabled (track() != 0);
        for (int b = 0; b < 5; ++b)
        {
            eqKnobs[b * 3]->setValue (t.meqFreq[(size_t) b], juce::dontSendNotification);
            eqKnobs[b * 3 + 1]->setValue (t.meqGain[(size_t) b], juce::dontSendNotification);
            eqKnobs[b * 3 + 2]->setValue (t.meqQ[(size_t) b], juce::dontSendNotification);
        }
        sendKnobs.clear();
        for (size_t k = 0; k < t.sends.size(); ++k)
        {
            const int target = t.sends[k].target;
            auto* kn = sendKnobs.add (new Knob ("Send", "level into " + (target < (int) p.mixer.size() ? p.mixer[(size_t) target].name : juce::String()), 0.0, 1.0, 1.0));
            kn->setValue (t.sends[k].level, juce::dontSendNotification);
            kn->onValueChange = [this, k, kn] {
                s.live ([&] (Project& pr) { auto& sends = pr.mixer[(size_t) track()].sends; if (k < sends.size()) sends[k].level = (float) kn->getValue(); });
                if (! kn->isMouseButtonDown()) s.commit ("Send level");
            };
            kn->onDragEnd = [this] { s.commit ("Send level"); };
            addAndMakeVisible (kn);
        }
        resized();
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::panel);
        auto& p = s.p();
        if (p.mixer.empty()) return;
        auto& t = p.mixer[(size_t) track()];
        g.setColour (col::text); g.setFont (juce::FontOptions (11.0f));
        for (int k = 0; k < sendKnobs.size(); ++k)
        {
            const int target = t.sends[(size_t) k].target;
            g.drawText ("> " + (target < (int) p.mixer.size() ? p.mixer[(size_t) target].name : juce::String()),
                        sendKnobs[k]->getRight() + 6, sendKnobs[k]->getY(), 180, sendKnobs[k]->getHeight(), juce::Justification::centredLeft);
        }
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        title.setBounds (r.removeFromTop (24));
        auto hb = r.removeFromTop (28);
        house.setBounds (hb.removeFromLeft (120)); hb.removeFromLeft (6); freeze.setBounds (hb.removeFromLeft (80));
        r.removeFromTop (6);
        for (int i = 0; i < maxSlots; ++i)
        {
            auto row = r.removeFromTop (30);
            bypass[i]->setBounds (row.removeFromLeft (14).reduced (0, 8));
            row.removeFromLeft (4);
            mixes[i]->setBounds (row.removeFromRight (28));
            slots[i]->setBounds (row.reduced (0, 2));
        }
        r.removeFromTop (4);
        eqLabel.setBounds (r.removeFromTop (16));
        {
            const int w = r.getWidth() / 5;
            for (int k = 0; k < 3; ++k)
            {
                auto row = r.removeFromTop (32);
                for (int b = 0; b < 5; ++b) eqKnobs[b * 3 + k]->setBounds (row.removeFromLeft (w).withSizeKeepingCentre (30, 30));
            }
        }
        r.removeFromTop (4);
        sendsLabel.setBounds (r.removeFromTop (18));
        for (auto* k : sendKnobs) k->setBounds (r.removeFromTop (30).removeFromLeft (30));
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        if (dragFrom < 0 || dropAt.first < 0) return;
        g.setColour (col::accent);
        if (dropAt.second)
            g.drawRect (slots[dropAt.first]->getBounds().expanded (2), 2);
        else
        {
            const int y = dropAt.first < slots.size() ? slots[dropAt.first]->getY() - 2 : slots.getLast()->getBottom() + 2;
            g.fillRect (slots[0]->getX() - 16, y - 1, slots[0]->getWidth() + 48, 3);
        }
    }

private:
    int dragFrom = -1;
    std::pair<int, bool> dropAt { -1, false };   // slot index, true = replace it, false = insert before it
    // middle of a slot = replace that effect; near its edges = drop in the line between slots
    std::pair<int, bool> dropTarget (int y) const
    {
        const int used = (int) s.p().mixer[(size_t) track()].fx.size();
        for (int k = 0; k < slots.size(); ++k)
        {
            const auto b = slots[k]->getBounds();
            if (k < used && y < b.getY() + b.getHeight() / 4) return { k, false };
            if (y < b.getBottom() - b.getHeight() / 4 || k >= used) return { k, true };   // any slot 1-10, empty or not
        }
        return { used, false };
    }
    void dropEffect (int from, std::pair<int, bool> to)
    {
        if (to.first < 0 || (to.second && to.first == from) || (! to.second && (to.first == from || to.first == from + 1))) return;
        const int t = track();
        s.edit (to.second ? "Replace effect" : "Move effect", [t, from, to] (Project& p) {
            auto& f = p.mixer[(size_t) t].fx;
            if (from >= (int) f.size() || (! to.second && to.first > (int) f.size())) return;
            const auto moved = f[(size_t) from];
            if (to.second)
            {
                while ((int) f.size() <= to.first) { FxSlot e; e.uid = p.newUid(); f.push_back (e); }
                f[(size_t) to.first] = moved;
                FxSlot e; e.uid = p.newUid(); f[(size_t) from] = e;
                while (! f.empty() && f.back().type.isEmpty()) f.pop_back();
            }
            else
            {
                f.erase (f.begin() + from);
                const int at = to.first > from ? to.first - 1 : to.first;
                f.insert (f.begin() + juce::jlimit (0, (int) f.size(), at), moved);
            }
        });
    }
    Studio& s;
    juce::Label title, sendsLabel, eqLabel;
    juce::OwnedArray<Knob> eqKnobs;   // band b: [b*3] freq, [b*3+1] gain, [b*3+2] Q
    juce::OwnedArray<SlotButton> slots;
    juce::OwnedArray<juce::TextButton> bypass;
    juce::OwnedArray<Knob> mixes, sendKnobs;
    juce::TextButton freeze, house;

    void slotPressed (int i, bool popup)
    {
        const int t = track();
        auto& fx = s.p().mixer[(size_t) t].fx;
        if (i >= (int) fx.size() || fx[(size_t) i].type.isEmpty())   // any slot, even with nothing above it: the chain runs in slot order
        {
            int uid = -1;
            s.edit ("FX slot", [&] (Project& p) {
                auto& f = p.mixer[(size_t) t].fx;
                while ((int) f.size() <= i) { FxSlot e; e.uid = p.newUid(); f.push_back (e); }
                uid = f[(size_t) i].uid; });
            showPluginHouseMenu (s, t, uid);
            return;
        }
        const int uid = fx[(size_t) i].uid;
        if (! popup) { openEffectEditor (s, t, uid); return; }
        juce::PopupMenu m;
        m.addItem (1, "Open editor"); m.addItem (2, fx[(size_t) i].bypass ? "Turn on" : "Bypass");
        m.addItem (3, "Replace (Plugin House)..."); m.addItem (4, "Move up", i > 0); m.addItem (5, "Move down", i + 1 < (int) fx.size());
        std::vector<AutoTarget> targets;
        m.addSubMenu ("Automate (new track below)", automateMenu (s, targets, 100, t, uid, -1));
        m.addSeparator(); m.addItem (6, "Remove");
        m.showMenuAsync ({}, [this, t, i, uid, targets] (int r) {
            if (r >= 100)
            {
                if (r - 100 >= (int) targets.size()) return;
                const auto& ln = s.p().lanes;
                int below = (int) ln.size() - 1;
                for (int k = 0; k < (int) ln.size(); ++k) if (ln[(size_t) k].mixerTrack == t) { below = k; break; }
                createAutomationClip (s, targets[(size_t) (r - 100)], below);
                return;
            }
            if (r == 1) openEffectEditor (s, t, uid);
            else if (r == 3) showPluginHouseMenu (s, t, uid);
            else if (r == 2 || r >= 4)
                s.edit ("FX slot", [&] (Project& p) {
                    auto& f = p.mixer[(size_t) t].fx;
                    if (i >= (int) f.size()) return;
                    if (r == 2) f[(size_t) i].bypass = ! f[(size_t) i].bypass;
                    else if (r == 4) std::swap (f[(size_t) i], f[(size_t) i - 1]);
                    else if (r == 5) std::swap (f[(size_t) i], f[(size_t) i + 1]);
                    else if (r == 6) { FxSlot e; e.uid = p.newUid(); f[(size_t) i] = e; }   // the slot empties; the others keep their place
                    while (! f.empty() && f.back().type.isEmpty()) f.pop_back();
                });
        });
    }
};

// FL-style routing cables in a lane under the strips (selected insert only unless WIRES is on; master is implicit)
class WireOverlay : public juce::Component
{
public:
    WireOverlay (Studio& st, int w) : s (st), stripW (w) { setInterceptsMouseClicks (false, false); }
    bool showAll = false;
    void paint (juce::Graphics& g) override
    {
        g.setColour (col::bg.darker (0.25f)); g.fillRect (getLocalBounds());
        auto& p = s.p();
        const float y0 = 5.0f, maxSag = (float) getHeight() - 9.0f;
        auto x = [this] (int i) { return (float) i * (float) stripW + (float) stripW * 0.5f; };
        for (size_t i = 1; i < p.mixer.size(); ++i)
        {
            auto& t = p.mixer[i];
            for (auto& sd : t.sends)
            {
                const int tg = sd.target;
                if (tg <= 0 || tg >= (int) p.mixer.size() || tg == (int) i) continue;
                const bool hot = (int) i == s.selectedTrack || tg == s.selectedTrack;
                if (! showAll && ! hot) continue;
                const float xa = x ((int) i), xb = x (tg);
                const float sag = juce::jmin (maxSag, 10.0f + std::abs (xb - xa) * 0.08f);
                juce::Path cable;
                cable.startNewSubPath (xa, y0);
                cable.cubicTo (xa, y0 + sag, xb, y0 + sag, xb, y0);
                g.setColour (col::accent.withAlpha (hot ? 0.9f : 0.3f + 0.25f * juce::jlimit (0.0f, 1.0f, sd.level)));
                g.strokePath (cable, juce::PathStrokeType (hot ? 1.8f : 1.1f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.fillEllipse (xb - 2.5f, y0 - 2.5f, 5.0f, 5.0f);
                g.setColour (col::text.withAlpha (0.6f));
                g.fillEllipse (xa - 2.0f, y0 - 2.0f, 4.0f, 4.0f);
            }
        }
    }
private:
    Studio& s;
    int stripW;
};

class MixerView : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit MixerView (Studio& st) : s (st), fx (st)
    {
        s.addChangeListener (this);
        viewport.setViewedComponent (&holder, false);
        viewport.setScrollBarsShown (false, true);
        addAndMakeVisible (viewport);
        addAndMakeVisible (fx);
        setupButton (addBtn, "+ Insert", "Add a mixer insert (bus / group / FX return)");
        addBtn.onClick = [this] { s.edit ("Add insert", [this] (Project& p) { s.selectedTrack = p.addMixerTrack ("Track " + juce::String (p.mixer.size())); }); };
        addAndMakeVisible (addBtn);
        setupButton (wiresBtn, "WIRES", "Show every routing wire (off = only the selected insert's wires)", true);
        wiresBtn.onClick = [this] { wires.showAll = wiresBtn.getToggleState(); wires.repaint(); };
        addAndMakeVisible (wiresBtn);
        refresh();
        startTimerHz (25);
    }
    ~MixerView() override { s.removeChangeListener (this); }
    void paint (juce::Graphics& g) override { g.fillAll (col::bg); }
    void resized() override
    {
        auto r = getLocalBounds();
        fx.setBounds (r.removeFromRight (fxPanelW));
        auto top = r.removeFromTop (30);
        addBtn.setBounds (top.removeFromLeft (100).reduced (4));
        wiresBtn.setBounds (top.removeFromLeft (80).reduced (4));
        viewport.setBounds (r);
        holder.setSize ((int) strips.size() * stripW, juce::jmax (300, r.getHeight() - viewport.getScrollBarThickness()));
        constexpr int wireLaneH = 40;
        for (size_t i = 0; i < strips.size(); ++i) strips[i]->setBounds ((int) i * stripW, 0, stripW, holder.getHeight() - wireLaneH);
        wires.setBounds (0, holder.getHeight() - wireLaneH, holder.getWidth(), wireLaneH);
        wires.toFront (false);
    }

private:
    Studio& s;
    juce::Viewport viewport;
    juce::Component holder;
    std::vector<std::unique_ptr<Strip>> strips;
    FxPanel fx;
    juce::TextButton addBtn, wiresBtn;
    WireOverlay wires { s, stripW };

    void refresh()
    {
        auto& p = s.p();
        if (strips.size() != p.mixer.size())
        {
            strips.clear();
            for (size_t i = 0; i < p.mixer.size(); ++i) { strips.push_back (std::make_unique<Strip> (s, (int) i)); holder.addAndMakeVisible (*strips.back()); }
            resized();
        }
        for (auto& st : strips) st->refresh();
        if (wires.getParentComponent() == nullptr) holder.addAndMakeVisible (wires);
        wires.toFront (false);
        wires.repaint();
        fx.refresh();
    }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override
    {
        if (! isShowing()) return;
        const auto vis = viewport.getViewArea();
        for (auto& st : strips) if (st->getBounds().intersects (vis)) st->tick();
        fx.updateLoads();
    }
};
}

// ---------------------------------------------------------------- Patch Method (HumHouse patcher)
namespace
{
struct PatchChain { const char* name; std::vector<const char*> fx; };
const std::vector<PatchChain>& patchChains()
{
    static const std::vector<PatchChain> c {
        { "Punchy drum (comp > clip > EQ)", { "comp", "clip", "eq" } },
        { "Hard 808 (saturate > clip > limit)", { "satur", "clip", "limiter" } },
        { "Lo-fi one-shot (crush > filter > room)", { "crush", "filter", "verb" } },
        { "Wide synth (chorus > wide > echo)", { "chorus", "wide", "delay" } },
        { "Space pad (phase > echo > room)", { "phaser", "delay", "verb" } },
        { "Pumping (pump > comp)", { "pump", "comp" } },
        { "Glitch stutter (glitch > crush > echo)", { "glitch", "crush", "delay" } },
        { "Grit lead (grit > flanger > EQ)", { "grit", "flanger", "eq" } },
        { "Peak-driven filter (Peak Controller > sweep)", { "peakctl", "filter" } },
        { "Mix bus polish (strip > EQ > Purify)", { "strip", "steq7", "purify" } },
        { "Vocal lead (EQ > 1176 > comp > echo)", { "steq7", "fet76", "comp", "delay" } },
        { "Vocal space (EQ > chorus > room)", { "eq", "chorus", "verb" } },
        { "Ad-lib phone (filter > crush > echo)", { "filter", "crush", "delay" } },
        { "Rage lead (grit > wide > room)", { "grit", "wide", "verb" } },
        { "Dirty 808 (grit > satur > limit)", { "grit", "satur", "limiter" } },
        { "Clean 808 glue (EQ > comp > clip)", { "eq", "comp", "clip" } },
        { "Hat sparkle (EQ > wide)", { "steq7", "wide" } },
        { "Snare crack (gate > comp > clip)", { "stgate", "comp", "clip" } },
        { "Dream keys (chorus > phase > room)", { "chorus", "phaser", "verb" } },
        { "Glitter pad (flanger > echo > room)", { "flanger", "delay", "verb" } },
        { "Tape lo-fi (satur > filter > echo)", { "satur", "filter", "delay" } },
        { "Sidechain pump pad (pump > wide > room)", { "pump", "wide", "verb" } },
        { "Master loud (dyn > max > limit)", { "stdyn", "stmax", "stlimit" } } };
    return c;
}

class PatchMethodView : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    PatchMethodView (Studio& st, int channelId, int track) : s (st), fx (st), chId (channelId)
    {
        fx.fixedTrack = track;
        title.setFont (juce::FontOptions (17.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, col::accent);
        addAndMakeVisible (title);
        setupButton (chains, "CHAINS", "Add a ready-made stack of effects in one click");
        chains.onClick = [this] { chainMenu(); };
        addAndMakeVisible (chains);
        setupButton (clear, "CLEAR", "Remove every effect in this patch");
        clear.onClick = [this] { s.edit ("Patch Method clear", [&] (Project& p) { p.mixer[(size_t) fx.track()].fx.clear(); }); };
        addAndMakeVisible (clear);
        setupButton (closeBtn, "X  CLOSE", "Close Patch Method (your effects stay on this sound)");
        closeBtn.onClick = [this] { if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) w->closeButtonPressed(); };
        addAndMakeVisible (closeBtn);
        addAndMakeVisible (fx);
        s.addChangeListener (this);
        setSize (320, 700);
        refresh();
        startTimer (200);
    }
    ~PatchMethodView() override { s.removeChangeListener (this); }
    void paint (juce::Graphics& g) override { g.fillAll (col::panel); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        title.setBounds (r.removeFromTop (24));
        auto b = r.removeFromTop (28);
        chains.setBounds (b.removeFromLeft (100)); b.removeFromLeft (6); clear.setBounds (b.removeFromLeft (70)); closeBtn.setBounds (b.removeFromRight (90));
        fx.setBounds (r);
    }
    bool valid() const { auto* c = const_cast<Studio&> (s).channel (chId); return c != nullptr && c->mixerTrack == fx.fixedTrack; }
private:
    Studio& s;
    FxPanel fx;
    int chId;
    juce::Label title;
    juce::TextButton chains, clear, closeBtn;
    void refresh()
    {
        auto* c = s.channel (chId);
        title.setText ("PATCH METHOD  |  " + (c != nullptr ? c->name : juce::String()), juce::dontSendNotification);
        if (fx.fixedTrack < (int) s.p().mixer.size()) fx.refresh();
    }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override { if (fx.fixedTrack < (int) s.p().mixer.size()) fx.updateLoads(); }
    void chainMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader ("Add a chain (stacks after what's there)");
        for (size_t i = 0; i < patchChains().size(); ++i) m.addItem ((int) i + 1, patchChains()[i].name);
        m.addSeparator();
        m.addItem (999, "Single effect / plugin (Plugin House)...");
        m.showMenuAsync ({}, [this] (int r) {
            const int t = fx.track();
            if (r == 999) { showPluginHouseMenu (s, t); return; }
            if (r < 1 || r > (int) patchChains().size()) return;
            const auto& ch = patchChains()[(size_t) r - 1];
            s.edit ("Patch Method chain", [&] (Project& p) {
                int ctl = 0, flt = 0;
                for (auto* id : ch.fx)
                {
                    if (p.mixer[(size_t) t].fx.size() >= (size_t) maxSlots) break;
                    auto& f = p.addFx (t, id);
                    if (juce::String (id) == "peakctl") ctl = f.uid;
                    if (juce::String (id) == "filter") flt = f.uid;
                }
                if (ctl != 0 && flt != 0)
                {
                    CtlLink L; L.srcTrack = t; L.srcUid = ctl; L.dstTrack = t; L.dstUid = flt; L.param = "cutoff"; L.lo = 0.2f; L.hi = 0.9f;
                    p.ctlLinks.push_back (L);
                }
            });
        });
    }
};

struct PatchWindow : juce::DocumentWindow, private juce::Timer
{
    PatchWindow (const juce::String& t, PatchMethodView* v, int id) : juce::DocumentWindow (t, col::panel, juce::DocumentWindow::closeButton), view (v), chId (id)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (v, true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        makeCaptureFriendly (*this);
        markFloating (*this);
        startTimer (500);
    }
    void closeButtonPressed() override { close (this); }
    void timerCallback() override { if (! view->valid()) { stopTimer(); close (this); } }
    static void close (PatchWindow* w) { juce::MessageManager::callAsync ([w] { all().removeObject (w); }); }
    static juce::OwnedArray<PatchWindow>& all() { static juce::OwnedArray<PatchWindow> w; return w; }
    PatchMethodView* view; int chId;
};
}

void openPatchMethod (Studio& s, int channelId)
{
    auto* c = s.channel (channelId);
    if (c == nullptr) return;
    for (auto* w : PatchWindow::all()) if (w->chId == channelId) { w->toFront (true); return; }
    bool shared = c->mixerTrack <= 0 || c->mixerTrack >= (int) s.p().mixer.size();
    for (auto& o : s.p().channels) shared |= o.id != channelId && o.mixerTrack == c->mixerTrack;
    if (shared)
        s.edit ("Patch Method", [&] (Project& p) { if (auto* ch = s.channel (channelId)) ch->mixerTrack = p.addMixerTrack ("PM " + ch->name); });
    c = s.channel (channelId);
    s.selectedTrack = c->mixerTrack;
    PatchWindow::all().add (new PatchWindow ("Patch Method - " + c->name, new PatchMethodView (s, channelId, c->mixerTrack), channelId));
}

std::unique_ptr<juce::Component> createMixer (Studio& s) { return std::make_unique<MixerView> (s); }
}
