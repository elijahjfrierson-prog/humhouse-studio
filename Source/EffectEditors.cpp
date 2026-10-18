#include "UI.h"

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
    ParamGrid (const juce::String& title, std::vector<P> ps, std::function<void (const juce::String&, float)> setLive, std::function<void()> commit)
        : params (std::move (ps)), onSet (std::move (setLive)), onCommit (std::move (commit))
    {
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
                cb->onChange = [this, cb, id, mn] { onSet (id, mn + (float) cb->getSelectedItemIndex()); onCommit(); };
                controls.add (cb);
            }
            else
            {
                auto* k = new Knob (p.name, "drag up / down, double-click resets", p.mn, p.mx, p.def);
                k->setValue (p.value, juce::dontSendNotification);
                if (p.mx - p.mn > 100 && p.mn > 0) k->setSkewFactorFromMidPoint (std::sqrt (p.mn * p.mx));
                k->onValueChange = [this, k, id] { onSet (id, (float) k->getValue()); if (! k->isMouseButtonDown()) onCommit(); };
                k->onDragEnd = [this] { onCommit(); };
                controls.add (k);
            }
            addAndMakeVisible (controls.getLast());
        }
        const int cols = juce::jlimit (1, 6, (int) params.size());
        const int rows = juce::jmax (1, ((int) params.size() + cols - 1) / cols);
        setSize (juce::jmax (300, cols * 86 + 20), rows * 92 + 50);
    }
    void paint (juce::Graphics& g) override { g.fillAll (col::panel); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        heading.setBounds (r.removeFromTop (28));
        const int cols = juce::jlimit (1, 6, (int) params.size());
        for (int i = 0; i < controls.size(); ++i)
        {
            juce::Rectangle<int> cell (r.getX() + (i % cols) * 86, r.getY() + (i / cols) * 92, 82, 88);
            labels[i]->setBounds (cell.removeFromTop (16));
            if (dynamic_cast<juce::ComboBox*> (controls[i]) != nullptr) controls[i]->setBounds (cell.withSizeKeepingCentre (80, 24));
            else controls[i]->setBounds (cell.reduced (12, 2));
        }
    }

private:
    std::vector<P> params;
    std::function<void (const juce::String&, float)> onSet;
    std::function<void()> onCommit;
    juce::Label heading;
    juce::OwnedArray<juce::Label> labels;
    juce::OwnedArray<juce::Component> controls;
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
        [&s, title] { s.commit (title); });
    EditorWindow::windows().add (new EditorWindow (title + " - " + trackName, grid, key,
                                                   [&s, track, slotUid] { return findSlot (s, track, slotUid) != nullptr; }));
}

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
    std::vector<ParamGrid::P> ps;
    ps.push_back ({ "volume", "Volume", 0, 1.25f, 0.8f, ch->volume, {} });
    ps.push_back ({ "pan", "Pan", -1, 1, 0, ch->pan, {} });
    ps.push_back ({ "pitch", "Pitch (st)", -24, 24, 0, ch->pitch, {} });
    ps.push_back ({ "glide", "Glide (s)", 0, 1, 0, ch->glide, {} });
    ps.push_back ({ "mono", "Mono / legato", 0, 1, 0, ch->mono ? 1.0f : 0.0f, { "Off", "On" } });
    ps.push_back ({ "root", "Step note", 24, 96, 60, (float) ch->rootKey, {} });
    if (ch->type == ChannelType::Sampler)
    {
        ps.push_back ({ "oneShot", "Mode", 0, 1, 1, ch->oneShot ? 1.0f : 0.0f, { "Gated (note length)", "One-shot" } });
        ps.push_back ({ "cutSelf", "Cut itself", 0, 1, 0, ch->cutItself ? 1.0f : 0.0f, { "Off", "On (808s never overlap)" } });
        ps.push_back ({ "cutGroup", "Cut group", 0, 8, 0, (float) ch->cutGroup, { "None", "1", "2", "3", "4", "5", "6", "7", "8" } });
        ps.push_back ({ "ampA", "Attack (s)", 0, 0.5f, 0, ch->ampAttack, {} });
        ps.push_back ({ "ampH", "Hold (s, 0=all)", 0, 4, 0, ch->ampHold, {} });
        ps.push_back ({ "ampR", "Release (s)", 0.002f, 2, 0.01f, ch->ampRelease, {} });
    }
    if (ch->type == ChannelType::Synth)
    {
        auto& sp = ch->synth;
        ps.push_back ({ "wave", "Wave", 0, 4, 0, (float) sp.wave, { "Saw", "Square", "Sine", "Triangle", "808" } });
        ps.push_back ({ "attack", "Attack", 0.001f, 2, 0.002f, sp.attack, {} });
        ps.push_back ({ "decay", "Decay", 0.01f, 4, 0.3f, sp.decay, {} });
        ps.push_back ({ "sustain", "Sustain", 0, 1, 0.7f, sp.sustain, {} });
        ps.push_back ({ "release", "Release", 0.01f, 4, 0.25f, sp.release, {} });
        ps.push_back ({ "cutoff", "Cutoff", 40, 20000, 12000, sp.cutoff, {} });
        ps.push_back ({ "res", "Resonance", 0, 1, 0.2f, sp.resonance, {} });
        ps.push_back ({ "detune", "Detune", 0, 1, 0, sp.detune, {} });
        ps.push_back ({ "unison", "Unison", 1, 7, 1, (float) sp.unison, {} });
        ps.push_back ({ "drive", "Drive", 0, 1, 0, sp.drive, {} });
    }
    auto* grid = new ParamGrid (ch->name + (ch->type == ChannelType::Synth ? "  |  Synth" : "  |  Sampler"), ps,
        [&s, channelId] (const juce::String& id, float v) {
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
                else if (id == "oneShot") c->oneShot = v > 0.5f; else if (id == "cutSelf") c->cutItself = v > 0.5f;
                else if (id == "cutGroup") c->cutGroup = (int) v; else if (id == "ampA") c->ampAttack = v;
                else if (id == "ampH") c->ampHold = v; else if (id == "ampR") c->ampRelease = v;
            });
        },
        [&s] { s.commit ("Channel settings"); });
    EditorWindow::windows().add (new EditorWindow (ch->name, grid, key, [&s, channelId] { return s.channel (channelId) != nullptr; }));
}

void closeAllEditorWindows() { EditorWindow::windows().clear(); }

void closeEditorsForPlugin (juce::AudioProcessor* p)
{
    auto& w = EditorWindow::windows();
    for (int i = w.size(); --i >= 0;)
        if (w[i]->proc == p) w.remove (i);
}
}
