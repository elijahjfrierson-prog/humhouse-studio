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
        fader.textFromValueFunction = [] (double v) { return v <= 0.0001 ? juce::String ("-inf dB") : juce::String (juce::Decibels::gainToDecibels (v), 1) + " dB"; };
        fader.setName ("Volume");
        fader.onValueChange = [this] { liveSet ([this] (MixerTrack& t) { t.volume = (float) fader.getValue(); }); if (! fader.isMouseButtonDown()) s.commit ("Volume"); };
        fader.onDragEnd = [this] { s.commit ("Volume"); };
        pan.onValueChange = [this] { liveSet ([this] (MixerTrack& t) { t.pan = (float) pan.getValue(); }); if (! pan.isMouseButtonDown()) s.commit ("Pan"); };
        pan.onDragEnd = [this] { s.commit ("Pan"); };
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
        if (index < (int) p.mixer.size() && p.mixer[(size_t) index].frozen)
        {
            g.setColour (col::accent2); g.drawText ("FROZEN", 0, 18, getWidth(), 12, juce::Justification::centred);
        }
        // meter
        auto m = meterArea();
        g.setColour (col::bg); g.fillRect (m);
        const float db = juce::Decibels::gainToDecibels (level, -60.0f);
        const float h = (float) m.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
        g.setGradientFill (juce::ColourGradient (col::rec, (float) m.getX(), (float) m.getY(), col::good, (float) m.getX(), (float) m.getBottom(), false));
        g.fillRect ((float) m.getX(), (float) m.getBottom() - h, (float) m.getWidth(), h);
        if (index < (int) p.mixer.size() && p.mixer[(size_t) index].fx.size() > 0)
        {
            g.setColour (col::accent); g.setFont (juce::FontOptions (10.0f));
            g.drawText (juce::String ((int) p.mixer[(size_t) index].fx.size()) + " FX", 0, getHeight() - 58, getWidth(), 12, juce::Justification::centred);
        }
    }
    juce::Rectangle<int> meterArea() const { return { getWidth() - 16, 64, 8, getHeight() - 64 - 96 }; }
    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        r.removeFromTop (28);
        pan.setBounds (r.removeFromTop (32).withSizeKeepingCentre (32, 32));
        auto bottom = r.removeFromBottom (90);
        fader.setBounds (r.withTrimmedRight (14));
        auto ms = bottom.removeFromTop (22);
        mute.setBounds (ms.removeFromLeft (ms.getWidth() / 2).reduced (1)); solo.setBounds (ms.reduced (1));
        bottom.removeFromTop (14);
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
            m.showMenuAsync ({}, [this] (int r) {
                if (r == 1)
                {
                    if (s.p().mixer[(size_t) index].frozen) s.edit ("Unfreeze", [this] (Project& p) { p.mixer[(size_t) index].frozen = false; });
                    else { s.engine.stop(); if (s.engine.freezeTrack (index)) s.commit ("Freeze"); }
                }
                else if (r == 2) name.showEditor();
                else if (r == 4) s.edit ("Clear FX", [this] (Project& p) { p.mixer[(size_t) index].fx.clear(); });
                else if (r == 3) juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Stem export", "Use File > Export selected mixer track.");
            });
        }
    }
    void tick()
    {
        const float pk = s.engine.trackPeak (index);
        level = juce::jmax (pk, level * 0.85f);
        repaint (meterArea());
    }
    Studio& s; int index; float level = 0;
    juce::Slider fader;
    Knob pan { "Pan", "left / right", -1.0, 1.0, 0.0 };
    juce::TextButton mute, solo, route;
    juce::Label name;
};

struct SlotButton : public juce::TextButton
{
    std::function<void (bool popup)> onPress;
    void clicked (const juce::ModifierKeys& m) override { if (onPress) onPress (m.isPopupMenu()); }
};

class FxPanel : public juce::Component
{
public:
    explicit FxPanel (Studio& st) : s (st)
    {
        title.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, col::accent);
        addAndMakeVisible (title);
        for (int i = 0; i < maxSlots; ++i)
        {
            auto* b = slots.add (new SlotButton());
            b->setTooltip ("FX slot: click empty = open Plugin House, click = open editor, right-click = bypass / replace / move / remove");
            b->onPress = [this, i] (bool popup) { slotPressed (i, popup); };
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
        sendsLabel.setText ("SENDS (click-to-route arrows on the strips)", juce::dontSendNotification);
        sendsLabel.setFont (juce::FontOptions (11.0f));
        sendsLabel.setColour (juce::Label::textColourId, col::dim);
        addAndMakeVisible (sendsLabel);
    }
    int track() const { return juce::jlimit (0, juce::jmax (0, (int) const_cast<Studio&> (s).p().mixer.size() - 1), s.selectedTrack); }
    void refresh()
    {
        auto& p = s.p();
        if (p.mixer.empty()) return;
        auto& t = p.mixer[(size_t) track()];
        title.setText ((track() == 0 ? juce::String ("Master") : t.name), juce::dontSendNotification);
        const auto& infos = builtinEffects();
        for (int i = 0; i < maxSlots; ++i)
        {
            const bool has = i < (int) t.fx.size();
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
        r.removeFromTop (8);
        sendsLabel.setBounds (r.removeFromTop (18));
        for (auto* k : sendKnobs) k->setBounds (r.removeFromTop (30).removeFromLeft (30));
    }

private:
    Studio& s;
    juce::Label title, sendsLabel;
    juce::OwnedArray<SlotButton> slots;
    juce::OwnedArray<juce::TextButton> bypass;
    juce::OwnedArray<Knob> mixes, sendKnobs;
    juce::TextButton freeze, house;

    void slotPressed (int i, bool popup)
    {
        const int t = track();
        auto& fx = s.p().mixer[(size_t) t].fx;
        if (i >= (int) fx.size()) { showPluginHouseMenu (s, t); return; }
        const int uid = fx[(size_t) i].uid;
        if (! popup) { openEffectEditor (s, t, uid); return; }
        juce::PopupMenu m;
        m.addItem (1, "Open editor"); m.addItem (2, fx[(size_t) i].bypass ? "Turn on" : "Bypass");
        m.addItem (3, "Replace (Plugin House)..."); m.addItem (4, "Move up", i > 0); m.addItem (5, "Move down", i + 1 < (int) fx.size());
        m.addSeparator(); m.addItem (6, "Remove");
        m.showMenuAsync ({}, [this, t, i, uid] (int r) {
            if (r == 1) openEffectEditor (s, t, uid);
            else if (r == 3) showPluginHouseMenu (s, t, uid);
            else if (r == 2 || r >= 4)
                s.edit ("FX slot", [&] (Project& p) {
                    auto& f = p.mixer[(size_t) t].fx;
                    if (i >= (int) f.size()) return;
                    if (r == 2) f[(size_t) i].bypass = ! f[(size_t) i].bypass;
                    else if (r == 4) std::swap (f[(size_t) i], f[(size_t) i - 1]);
                    else if (r == 5) std::swap (f[(size_t) i], f[(size_t) i + 1]);
                    else if (r == 6) f.erase (f.begin() + i);
                });
        });
    }
};

// FL-style routing cables drawn over the strips: every insert -> each target it feeds
class WireOverlay : public juce::Component
{
public:
    WireOverlay (Studio& st, int w) : s (st), stripW (w) { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics& g) override
    {
        auto& p = s.p();
        const float y0 = (float) getHeight() - 10.0f;
        auto x = [this] (int i) { return (float) i * (float) stripW + (float) stripW * 0.5f; };
        for (size_t i = 1; i < p.mixer.size(); ++i)
        {
            auto& t = p.mixer[i];
            std::vector<std::pair<int, float>> targets;
            for (auto& sd : t.sends) targets.push_back ({ sd.target, sd.level });
            if (t.routesToMaster && t.sends.empty()) targets.push_back ({ 0, 1.0f });
            else if (t.routesToMaster) targets.push_back ({ 0, 1.0f });
            const bool hot = (int) i == s.selectedTrack;
            for (auto [tg, lvl] : targets)
            {
                if (tg < 0 || tg >= (int) p.mixer.size() || tg == (int) i) continue;
                const float xa = x ((int) i), xb = x (tg);
                const float sag = 18.0f + std::abs (xb - xa) * 0.12f;
                juce::Path cable;
                cable.startNewSubPath (xa, y0);
                cable.cubicTo (xa, y0 - sag, xb, y0 - sag, xb, y0);
                auto c = tg == 0 ? col::dim : col::accent;
                g.setColour (c.withAlpha (hot ? 0.95f : 0.35f + 0.3f * juce::jlimit (0.0f, 1.0f, lvl)));
                g.strokePath (cable, juce::PathStrokeType (hot ? 2.4f : 1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.fillEllipse (xb - 3.0f, y0 - 3.0f, 6.0f, 6.0f);
            }
            g.setColour (col::text.withAlpha (0.6f));
            g.fillEllipse (x ((int) i) - 2.5f, y0 - 2.5f, 5.0f, 5.0f);
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
        refresh();
        startTimerHz (25);
    }
    ~MixerView() override { s.removeChangeListener (this); }
    void paint (juce::Graphics& g) override { g.fillAll (col::bg); }
    void resized() override
    {
        auto r = getLocalBounds();
        fx.setBounds (r.removeFromRight (fxPanelW));
        addBtn.setBounds (r.removeFromTop (30).removeFromLeft (100).reduced (4));
        viewport.setBounds (r);
        holder.setSize ((int) strips.size() * stripW, juce::jmax (300, r.getHeight() - viewport.getScrollBarThickness()));
        for (size_t i = 0; i < strips.size(); ++i) strips[i]->setBounds ((int) i * stripW, 0, stripW, holder.getHeight());
        wires.setBounds (holder.getLocalBounds());
        wires.toFront (false);
    }

private:
    Studio& s;
    juce::Viewport viewport;
    juce::Component holder;
    std::vector<std::unique_ptr<Strip>> strips;
    FxPanel fx;
    juce::TextButton addBtn;
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
    }
};
}

std::unique_ptr<juce::Component> createMixer (Studio& s) { return std::make_unique<MixerView> (s); }
}
