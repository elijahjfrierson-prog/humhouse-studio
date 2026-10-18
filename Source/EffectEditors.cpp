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
    ParamGrid (const juce::String& title, std::vector<P> ps, std::function<void (const juce::String&, float)> setLive, std::function<void()> commit,
               const juce::String& presetKind = {})
        : params (std::move (ps)), onSet (std::move (setLive)), onCommit (std::move (commit)), kind (presetKind)
    {
        if (kind.isNotEmpty())
        {
            setupButton (presetBtn, "PRESETS", "Save or load a preset for this effect / instrument");
            presetBtn.onClick = [this] { showPresetMenu(); };
            addAndMakeVisible (presetBtn);
        }
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
                cb->onChange = [this, cb, id, mn] { set (id, mn + (float) cb->getSelectedItemIndex()); onCommit(); };
                controls.add (cb);
            }
            else
            {
                auto* k = new Knob (p.name, "drag up / down, double-click resets", p.mn, p.mx, p.def);
                k->setValue (p.value, juce::dontSendNotification);
                if (p.mx - p.mn > 100 && p.mn > 0) k->setSkewFactorFromMidPoint (std::sqrt (p.mn * p.mx));
                k->onValueChange = [this, k, id] { set (id, (float) k->getValue()); if (! k->isMouseButtonDown()) onCommit(); };
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
        auto top = r.removeFromTop (28);
        if (kind.isNotEmpty()) presetBtn.setBounds (top.removeFromRight (80).reduced (0, 2));
        heading.setBounds (top);
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
    juce::String kind;
    juce::TextButton presetBtn;
    std::unique_ptr<juce::FileChooser> chooser;
    void set (const juce::String& id, float v)
    {
        for (auto& p : params) if (p.id == id) p.value = v;
        onSet (id, v);
    }
    juce::File presetDir() const
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("HumHouse Studio")
            .getChildFile ("Presets").getChildFile (juce::File::createLegalFileName (kind));
    }
    void showPresetMenu()
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Save preset...");
        auto files = presetDir().findChildFiles (juce::File::findFiles, false, "*.hhpreset");
        files.sort();
        if (! files.isEmpty()) menu.addSeparator();
        for (int i = 0; i < files.size() && i < 200; ++i) menu.addItem (100 + i, files[i].getFileNameWithoutExtension());
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetBtn), [this, files] (int r) {
            if (r == 1) savePreset();
            else if (r >= 100 && r - 100 < files.size()) loadPreset (files[r - 100]);
        });
    }
    void savePreset()
    {
        auto* aw = new juce::AlertWindow ("Save preset", "Preset name", juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor ("name", "My preset");
        aw->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<ParamGrid> safe (this);
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw] (int r) {
            const auto name = juce::File::createLegalFileName (aw->getTextEditorContents ("name").trim());
            if (r != 1 || name.isEmpty() || safe == nullptr) return;
            juce::XmlElement x ("HUMHOUSE_PRESET");
            x.setAttribute ("kind", safe->kind);
            for (auto& p : safe->params) { auto* e = x.createNewChildElement ("P"); e->setAttribute ("id", p.id); e->setAttribute ("v", (double) p.value); }
            safe->presetDir().createDirectory();
            x.writeTo (safe->presetDir().getChildFile (name + ".hhpreset"));
        }), true);
    }
    void loadPreset (const juce::File& f)
    {
        auto x = juce::XmlDocument::parse (f);
        if (x == nullptr) return;
        for (auto* e : x->getChildWithTagNameIterator ("P"))
        {
            const auto id = e->getStringAttribute ("id");
            for (size_t i = 0; i < params.size(); ++i)
            {
                if (params[i].id != id) continue;
                const float v = juce::jlimit (params[i].mn, params[i].mx, (float) e->getDoubleAttribute ("v", params[i].value));
                if (auto* cb = dynamic_cast<juce::ComboBox*> (controls[(int) i])) cb->setSelectedItemIndex ((int) std::round (v - params[i].mn), juce::dontSendNotification);
                else if (auto* k = dynamic_cast<juce::Slider*> (controls[(int) i])) k->setValue (v, juce::dontSendNotification);
                set (id, v);
            }
        }
        onCommit();
    }
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
        [&s, title] { s.commit (title); }, slot->type);
    EditorWindow::windows().add (new EditorWindow (title + " - " + trackName, grid, key,
                                                   [&s, track, slotUid] { return findSlot (s, track, slotUid) != nullptr; }));
}


// FL-style channel settings: SMP / INS / FUNC pages (the "wrench" panel)
struct ChannelTabs : juce::Component
{
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    int w = 360, h = 200;
    ChannelTabs() { tabs.setTabBarDepth (28); tabs.setOutline (0); addAndMakeVisible (tabs); }
    void add (const juce::String& name, juce::Component* page)
    {
        w = juce::jmax (w, page->getWidth()); h = juce::jmax (h, page->getHeight());
        tabs.addTab (name, col::panel, page, true);
    }
    void finish() { setSize (w, h + tabs.getTabBarDepth()); tabs.setCurrentTabIndex (0); }
    void resized() override { tabs.setBounds (getLocalBounds()); }
};

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
    const bool sampler = ch->type == ChannelType::Sampler;
    std::vector<ParamGrid::P> smp, ins, func;
    if (sampler)
    {
        smp.push_back ({ "smpNorm", "Normalize", 0, 1, 0, ch->smpNormalize ? 1.0f : 0.0f, { "Off", "On" } });
        smp.push_back ({ "smpBoost", "Boost (dB)", -24, 24, 0, ch->smpBoostDb, {} });
        smp.push_back ({ "smpRev", "Reverse", 0, 1, 0, ch->smpReverse ? 1.0f : 0.0f, { "Off", "On" } });
        smp.push_back ({ "smpInv", "Polarity", 0, 1, 0, ch->smpInvert ? 1.0f : 0.0f, { "Normal", "Inverted" } });
        smp.push_back ({ "smpDeclick", "Declick", 0, 1, 1, ch->smpDeclick ? 1.0f : 0.0f, { "Off", "On" } });
        smp.push_back ({ "smpTrim", "Trim silence", 0, 1, 0, ch->smpTrim ? 1.0f : 0.0f, { "Off", "On" } });
        smp.push_back ({ "smpFadeIn", "Fade in (s)", 0, 2, 0, ch->smpFadeIn, {} });
        smp.push_back ({ "smpFadeOut", "Fade out (s)", 0, 2, 0, ch->smpFadeOut, {} });
        smp.push_back ({ "stretchMode", "Time mode", 0, 2, 0, (float) ch->stretchMode, { "Resample", "Stretch", "Stretch X" } });
        smp.push_back ({ "stretchTime", "Time mul", 0.25f, 4, 1, ch->stretchTime, {} });
        smp.push_back ({ "pitch", "Pitch (st)", -24, 24, 0, ch->pitch, {} });
        smp.push_back ({ "root", "Root / step note", 24, 96, 60, (float) ch->rootKey, {} });
    }
    ins.push_back ({ "volume", "Volume", 0, 1.25f, 0.8f, ch->volume, {} });
    ins.push_back ({ "pan", "Pan", -1, 1, 0, ch->pan, {} });
    if (sampler)
    {
        ins.push_back ({ "oneShot", "Mode", 0, 1, 1, ch->oneShot ? 1.0f : 0.0f, { "Gated (note length)", "One-shot" } });
        ins.push_back ({ "cutSelf", "Cut itself", 0, 1, 0, ch->cutItself ? 1.0f : 0.0f, { "Off", "On (808s never overlap)" } });
        ins.push_back ({ "cutGroup", "Cut group", 0, 8, 0, (float) ch->cutGroup, { "None", "1", "2", "3", "4", "5", "6", "7", "8" } });
        ins.push_back ({ "ampA", "Attack (s)", 0, 0.5f, 0, ch->ampAttack, {} });
        ins.push_back ({ "ampH", "Hold (s, 0=all)", 0, 4, 0, ch->ampHold, {} });
        ins.push_back ({ "ampR", "Release (s)", 0.002f, 2, 0.01f, ch->ampRelease, {} });
    }
    else
    {
        auto& sp = ch->synth;
        ins.push_back ({ "wave", "Wave", 0, 4, 0, (float) sp.wave, { "Saw", "Square", "Sine", "Triangle", "808" } });
        ins.push_back ({ "attack", "Attack", 0.001f, 2, 0.002f, sp.attack, {} });
        ins.push_back ({ "decay", "Decay", 0.01f, 4, 0.3f, sp.decay, {} });
        ins.push_back ({ "sustain", "Sustain", 0, 1, 0.7f, sp.sustain, {} });
        ins.push_back ({ "release", "Release", 0.01f, 4, 0.25f, sp.release, {} });
        ins.push_back ({ "cutoff", "Cutoff", 40, 20000, 12000, sp.cutoff, {} });
        ins.push_back ({ "res", "Resonance", 0, 1, 0.2f, sp.resonance, {} });
        ins.push_back ({ "detune", "Detune", 0, 1, 0, sp.detune, {} });
        ins.push_back ({ "unison", "Unison", 1, 7, 1, (float) sp.unison, {} });
        ins.push_back ({ "drive", "Drive", 0, 1, 0, sp.drive, {} });
        func.push_back ({ "pitch", "Pitch (st)", -24, 24, 0, ch->pitch, {} });
        func.push_back ({ "root", "Step note", 24, 96, 60, (float) ch->rootKey, {} });
    }
    func.push_back ({ "glide", "Portamento / glide (s)", 0, 1, 0, ch->glide, {} });
    func.push_back ({ "mono", "Mono / legato", 0, 1, 0, ch->mono ? 1.0f : 0.0f, { "Off", "On" } });

    auto setter = [&s, channelId] (const juce::String& id, float v) {
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
                else if (id == "smpNorm") c->smpNormalize = v > 0.5f; else if (id == "smpBoost") c->smpBoostDb = v;
                else if (id == "smpRev") c->smpReverse = v > 0.5f; else if (id == "smpInv") c->smpInvert = v > 0.5f;
                else if (id == "smpDeclick") c->smpDeclick = v > 0.5f; else if (id == "smpTrim") c->smpTrim = v > 0.5f;
                else if (id == "smpFadeIn") c->smpFadeIn = v; else if (id == "smpFadeOut") c->smpFadeOut = v;
                else if (id == "stretchMode") c->stretchMode = (int) v; else if (id == "stretchTime") c->stretchTime = v;
            });
        };
    auto commit = [&s] { s.commit ("Channel settings"); };
    const juce::String kind = sampler ? "Sampler" : "Synth";
    auto* tabs = new ChannelTabs();
    if (sampler) tabs->add ("SMP", new ParamGrid (ch->name + "  |  Sample", smp, setter, commit, "Sampler SMP"));
    tabs->add ("INS", new ParamGrid (ch->name + "  |  " + kind, ins, setter, commit, kind));
    tabs->add ("FUNC", new ParamGrid (ch->name + "  |  Functions", func, setter, commit, kind + " FUNC"));
    tabs->finish();
    EditorWindow::windows().add (new EditorWindow (ch->name, tabs, key, [&s, channelId] { return s.channel (channelId) != nullptr; }));
}

void closeAllEditorWindows() { EditorWindow::windows().clear(); }

void closeEditorsForPlugin (juce::AudioProcessor* p)
{
    auto& w = EditorWindow::windows();
    for (int i = w.size(); --i >= 0;)
        if (w[i]->proc == p) w.remove (i);
}
}

namespace hhs
{
static juce::File presetsRoot() { return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("HumHouse Studio").getChildFile ("Presets"); }

static void saveChainPreset (Studio& s, int track)
{
    auto* aw = new juce::AlertWindow ("Save FX chain preset", "Name this chain (every effect on the insert, in order)", juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("name", "My vocal chain");
    aw->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([&s, track, aw] (int r) {
        const auto name = juce::File::createLegalFileName (aw->getTextEditorContents ("name").trim());
        if (r != 1 || name.isEmpty() || track < 0 || track >= (int) s.p().mixer.size()) return;
        s.engine.storePluginStates();
        juce::XmlElement x ("HUMHOUSE_CHAIN");
        for (auto& f : s.p().mixer[(size_t) track].fx)
        {
            auto* fe = x.createNewChildElement ("FX");
            fe->setAttribute ("type", f.type); fe->setAttribute ("plugin", f.pluginId);
            fe->setAttribute ("bypass", f.bypass); fe->setAttribute ("mix", (double) f.mix);
            if (f.pluginState.getSize() > 0) fe->setAttribute ("state", f.pluginState.toBase64Encoding());
            for (auto& [id, v] : f.params) { auto* pe = fe->createNewChildElement ("P"); pe->setAttribute ("id", id); pe->setAttribute ("v", (double) v); }
        }
        auto dir = presetsRoot().getChildFile ("Chains");
        dir.createDirectory();
        x.writeTo (dir.getChildFile (name + ".hhchain"));
    }), true);
}

static void loadChainPreset (Studio& s, int track, const juce::File& file, bool replace)
{
    auto x = juce::XmlDocument::parse (file);
    if (x == nullptr) return;
    s.edit ("Load FX chain", [&] (Project& p) {
        if (track < 0 || track >= (int) p.mixer.size()) return;
        if (replace) p.mixer[(size_t) track].fx.clear();
        for (auto* fe : x->getChildWithTagNameIterator ("FX"))
        {
            auto& f = p.addFx (track, fe->getStringAttribute ("type"));
            f.pluginId = fe->getStringAttribute ("plugin");
            f.bypass = fe->getBoolAttribute ("bypass");
            f.mix = (float) fe->getDoubleAttribute ("mix", 1.0);
            if (fe->hasAttribute ("state")) f.pluginState.fromBase64Encoding (fe->getStringAttribute ("state"));
            for (auto* pe : fe->getChildWithTagNameIterator ("P")) f.params[pe->getStringAttribute ("id")] = (float) pe->getDoubleAttribute ("v");
        }
    });
}

juce::PopupMenu trackPresetMenu (Studio& s, int track)
{
    juce::PopupMenu m;
    if (track < 0) { m.addItem ("(no mixer insert)", false, false, [] {}); return m; }
    juce::PopupMenu chains;
    auto chainFiles = presetsRoot().getChildFile ("Chains").findChildFiles (juce::File::findFiles, false, "*.hhchain");
    chainFiles.sort();
    for (auto& f : chainFiles)
    {
        juce::PopupMenu one;
        one.addItem ("Replace this track's FX", [&s, track, f] { loadChainPreset (s, track, f, true); });
        one.addItem ("Add after current FX", [&s, track, f] { loadChainPreset (s, track, f, false); });
        chains.addSubMenu (f.getFileNameWithoutExtension(), one);
    }
    if (chainFiles.isEmpty()) chains.addItem ("(none saved yet)", false, false, [] {});
    m.addSubMenu ("FX chain presets", chains);
    m.addItem ("Save this track's FX chain as preset...", [&s, track] { saveChainPreset (s, track); });
    m.addSeparator();
    m.addSectionHeader ("Add effect from saved preset");
    bool any = false;
    for (auto& inf : builtinEffects())
    {
        auto files = presetsRoot().getChildFile (juce::File::createLegalFileName (inf.id)).findChildFiles (juce::File::findFiles, false, "*.hhpreset");
        if (files.isEmpty()) continue;
        files.sort();
        any = true;
        juce::PopupMenu sub;
        const juce::String type = inf.id;
        for (auto& f : files)
            sub.addItem (f.getFileNameWithoutExtension(), [&s, track, type, f] {
                auto x = juce::XmlDocument::parse (f);
                if (x == nullptr) return;
                int uid = 0;
                s.edit ("Add preset", [&] (Project& p) {
                    if (track >= (int) p.mixer.size()) return;
                    auto& slot = p.addFx (track, type);
                    uid = slot.uid;
                    for (auto* e : x->getChildWithTagNameIterator ("P")) slot.params[e->getStringAttribute ("id")] = (float) e->getDoubleAttribute ("v");
                });
                if (uid != 0) openEffectEditor (s, track, uid);
            });
        m.addSubMenu (inf.name, sub);
    }
    if (! any) m.addItem ("(save presets from any effect's PRESETS button)", false, false, [] {});
    return m;
}
}
