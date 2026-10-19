#include "UI.h"
#include <complex>
#include <map>

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
        makeCaptureFriendly (*this);
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

struct FactoryPreset { const char* name; std::vector<std::pair<const char*, float>> values; };
const std::vector<FactoryPreset>& factoryPresets (const juce::String& type)
{
    static const std::map<juce::String, std::vector<FactoryPreset>> all {
        { "steq7", { { "Flat (reset)", {  } }, { "Vocal Clarity", { { "e1F", 90.0f }, { "e3F", 300.0f }, { "e3G", -3.0f }, { "e5F", 4000.0f }, { "e5G", 2.5f }, { "e7F", 10000.0f }, { "e7G", 3.0f } } }, { "Tight 808", { { "e1F", 25.0f }, { "e2F", 60.0f }, { "e2G", 2.0f }, { "e3F", 220.0f }, { "e3G", -4.0f }, { "e3Q", 1.4f } } }, { "Air", { { "e6F", 9000.0f }, { "e6G", 1.5f }, { "e7F", 12000.0f }, { "e7G", 4.0f } } }, { "Mud Cut", { { "e1F", 40.0f }, { "e3F", 300.0f }, { "e3G", -5.0f }, { "e3Q", 1.2f } } }, { "De-Harsh (dynamic)", { { "e5F", 3500.0f }, { "e5Dyn", 1.0f }, { "e5DTh", -30.0f }, { "e5DRg", -6.0f } } }, { "Master Polish", { { "e1F", 25.0f }, { "e2F", 80.0f }, { "e2G", 1.0f }, { "e3F", 300.0f }, { "e3G", -1.5f }, { "e7F", 14000.0f }, { "e7G", 1.5f } } } } },
        { "stdyn", { { "Vocal Leveler", { { "threshold", -24.0f }, { "ratio", 3.0f }, { "attack", 10.0f }, { "release", 120.0f }, { "gain", 4.0f }, { "knee", 8.0f } } }, { "Drum Bus", { { "threshold", -18.0f }, { "ratio", 4.0f }, { "attack", 30.0f }, { "release", 100.0f }, { "gain", 3.0f } } }, { "Mix Glue", { { "threshold", -16.0f }, { "ratio", 2.0f }, { "attack", 30.0f }, { "release", 300.0f }, { "gain", 2.0f }, { "knee", 12.0f } } }, { "Brickwall", { { "threshold", -6.0f }, { "ratio", 20.0f }, { "attack", 0.5f }, { "release", 50.0f } } } } },
        { "comp", { { "Vocal", { { "threshold", -22.0f }, { "ratio", 3.0f }, { "attack", 8.0f }, { "release", 120.0f } } }, { "Drums", { { "threshold", -18.0f }, { "ratio", 4.0f }, { "attack", 25.0f }, { "release", 90.0f } } }, { "Glue", { { "threshold", -14.0f }, { "ratio", 2.0f }, { "attack", 30.0f }, { "release", 300.0f } } }, { "Parallel Smash", { { "threshold", -30.0f }, { "ratio", 8.0f }, { "mix", 0.5f } } } } },
        { "fet76", { { "Vocal Grab", { { "input", 28.0f }, { "ratio", 0.0f }, { "attack", 4.0f }, { "release", 5.0f } } }, { "All Buttons Smash", { { "input", 36.0f }, { "ratio", 4.0f }, { "mix", 0.5f } } }, { "Drum Crunch", { { "input", 32.0f }, { "ratio", 2.0f } } } } },
        { "stgate", { { "Vocal Gate", { { "threshold", -45.0f }, { "range", -30.0f }, { "release", 150.0f } } }, { "Tight Drums", { { "threshold", -35.0f }, { "range", -60.0f }, { "hold", 10.0f }, { "release", 60.0f } } }, { "Soft Expander", { { "mode", 1.0f }, { "threshold", -40.0f }, { "ratio", 2.0f } } } } },
        { "stlimit", { { "Master Loud", { { "gain", 6.0f }, { "ceiling", -0.3f } } }, { "Safe", { { "gain", 0.0f }, { "ceiling", -1.0f } } }, { "Transparent", { { "gain", 3.0f }, { "ceiling", -1.0f }, { "release", 100.0f } } } } },
        { "limiter", { { "Master Loud", { { "gain", 6.0f }, { "ceiling", -0.3f } } }, { "Safe", { { "gain", 0.0f }, { "ceiling", -1.0f } } }, { "Transparent", { { "gain", 3.0f }, { "ceiling", -1.0f }, { "release", 100.0f } } } } },
        { "stvocal", { { "Smooth Rap Lead", { { "smooth", 0.6f }, { "deess", 0.5f }, { "silk", 0.3f }, { "warmth", 0.25f } } },
                       { "Tame Harsh Highs", { { "smooth", 0.85f }, { "deess", 0.6f }, { "silk", 0.1f }, { "warmth", 0.1f } } },
                       { "Airy Pop", { { "smooth", 0.45f }, { "deess", 0.45f }, { "silk", 0.65f }, { "warmth", 0.15f } } },
                       { "Warm R&B", { { "smooth", 0.5f }, { "deess", 0.4f }, { "silk", 0.25f }, { "warmth", 0.55f } } },
                       { "De-Box Bedroom Mic", { { "smooth", 0.9f }, { "deess", 0.35f }, { "silk", 0.35f }, { "warmth", 0.2f } } },
                       { "Ad-lib Bright", { { "smooth", 0.4f }, { "deess", 0.3f }, { "silk", 0.75f }, { "warmth", 0.3f } } },
                       { "Background Stack", { { "smooth", 0.7f }, { "deess", 0.7f }, { "silk", 0.2f }, { "warmth", 0.2f }, { "out", -3.0f } } } } },
        { "satur", { { "Tape Warmth", { { "drive", 6.0f }, { "char", 0.0f } } }, { "Tube Push", { { "drive", 10.0f }, { "char", 1.0f } } }, { "Transformer Grit", { { "drive", 14.0f }, { "char", 2.0f }, { "mix", 0.7f } } } } },
        { "strip", { { "Rap Vocal", { { "gateTh", -55.0f }, { "compTh", -20.0f }, { "compRatio", 4.0f }, { "makeup", 5.0f }, { "hpf", 100.0f }, { "mid", -2.0f }, { "midF", 400.0f }, { "high", 3.0f } } }, { "Singer", { { "compTh", -18.0f }, { "compRatio", 3.0f }, { "hpf", 80.0f }, { "high", 2.0f } } }, { "Podcast", { { "gateTh", -50.0f }, { "compTh", -22.0f }, { "compRatio", 3.0f }, { "hpf", 90.0f }, { "low", -2.0f }, { "high", 2.0f } } } } },
        { "autopitch", { { "Natural Tune", { { "style", 0.0f }, { "speed", 0.5f }, { "humanize", 0.4f } } }, { "Modern Pop", { { "style", 2.0f }, { "speed", 0.8f }, { "humanize", 0.15f } } }, { "Hard Tune", { { "style", 3.0f }, { "speed", 1.0f } } }, { "Robot", { { "style", 3.0f }, { "speed", 1.0f }, { "humanize", 0.0f } } }, { "Chipmunk", { { "pitch", 7.0f } } }, { "Deep Voice", { { "pitch", -5.0f } } }, { "A = 432 Hz", { { "tuning", 432.0f } } } } },
        { "deess", { { "Gentle", { { "threshold", -28.0f }, { "range", 6.0f } } }, { "Strong", { { "threshold", -35.0f }, { "range", 12.0f } } }, { "Bright Vocal", { { "freq", 7500.0f } } } } },
        { "eq", { { "Vocal", { { "hpf", 90.0f }, { "midGain", -2.0f }, { "midFreq", 400.0f }, { "highGain", 3.0f } } }, { "Kick", { { "lowGain", 4.0f }, { "lowFreq", 60.0f }, { "midGain", -4.0f }, { "midFreq", 350.0f } } }, { "Telephone", { { "hpf", 400.0f }, { "lpf", 3500.0f } } } } },
        { "filter", { { "Low Pass Sweep", { { "mode", 0.0f }, { "cutoff", 800.0f }, { "res", 0.4f }, { "lfo", 0.6f }, { "rate", 1.0f } } }, { "High Pass Build", { { "mode", 1.0f }, { "cutoff", 600.0f } } }, { "Wah", { { "mode", 2.0f }, { "cutoff", 1200.0f }, { "res", 0.7f }, { "lfo", 0.8f }, { "rate", 3.0f } } } } },
        { "gate", { { "Tight", { { "threshold", -40.0f }, { "release", 50.0f } } }, { "Soft", { { "threshold", -55.0f }, { "release", 200.0f }, { "range", -20.0f } } } } },
        { "pump", { { "Classic 1/4", { { "depth", 0.8f }, { "rate", 1.0f } } }, { "Subtle", { { "depth", 0.4f } } }, { "1/8 Pulse", { { "rate", 2.0f }, { "depth", 0.7f } } } } },
        { "clip", { { "Warm", { { "gain", 3.0f }, { "ceiling", -1.0f } } }, { "Loud", { { "gain", 9.0f }, { "ceiling", -0.5f } } } } },
        { "verb", { { "Vocal Plate", { { "size", 0.5f }, { "damp", 0.5f }, { "mix", 0.2f }, { "lowcut", 300.0f } } }, { "Big Hall", { { "size", 0.9f }, { "damp", 0.3f }, { "mix", 0.3f } } }, { "Small Room", { { "size", 0.25f }, { "damp", 0.6f }, { "mix", 0.15f } } }, { "Lo-fi Wash", { { "size", 0.8f }, { "damp", 0.9f }, { "mix", 0.4f } } } } },
        { "delay", { { "1/4 Echo", { { "time", 4.0f }, { "feedback", 0.3f }, { "mix", 0.2f } } }, { "1/8 Ping Pong", { { "time", 2.0f }, { "pingpong", 1.0f }, { "feedback", 0.4f } } }, { "Dotted 1/8", { { "time", 3.0f }, { "feedback", 0.45f } } }, { "Dub Throw", { { "time", 5.0f }, { "feedback", 0.7f }, { "tone", 3000.0f }, { "mix", 0.35f } } } } },
        { "flanger", { { "Jet Plane", { { "rate", 0.1f }, { "depth", 0.9f }, { "delay", 3.0f }, { "feedback", 0.8f } } }, { "Classic Flange", { { "rate", 0.3f }, { "depth", 0.7f }, { "delay", 1.5f }, { "feedback", 0.5f } } }, { "Zero Flange", { { "rate", 0.2f }, { "depth", 1.0f }, { "delay", 0.6f }, { "feedback", -0.7f }, { "invert", 1.0f } } }, { "Subtle Vocal", { { "rate", 0.2f }, { "depth", 0.4f }, { "delay", 2.0f }, { "feedback", 0.2f }, { "mix", 0.25f } } }, { "Metal Comb", { { "rate", 0.05f }, { "depth", 0.2f }, { "delay", 5.0f }, { "feedback", 0.9f } } }, { "Wobble 808", { { "rate", 2.0f }, { "depth", 0.5f }, { "delay", 1.0f }, { "feedback", 0.3f }, { "mix", 0.35f } } } } },
        { "flangus", { { "Dreamy Choir", { { "voices", 4.0f }, { "spread", 1.0f }, { "depth", 0.6f }, { "speed", 0.2f }, { "delay", 12.0f }, { "feedback", 0.1f } } }, { "Psychedelic Swirl", { { "voices", 3.0f }, { "speed", 0.8f }, { "feedback", 0.6f }, { "cross", 0.6f }, { "delay", 3.0f } } }, { "Wide Guitar", { { "voices", 2.0f }, { "delay", 8.0f }, { "spread", 1.0f }, { "mix", 0.4f } } }, { "Liquid Hats", { { "voices", 2.0f }, { "speed", 1.5f }, { "delay", 1.5f }, { "feedback", 0.7f } } }, { "Subtle Thick", { { "voices", 2.0f }, { "depth", 0.25f }, { "mix", 0.25f } } }, { "Space Flange", { { "voices", 6.0f }, { "depth", 0.9f }, { "delay", 5.0f }, { "feedback", 0.5f }, { "cross", 0.8f } } } } },
        { "chorus", { { "Wide Vocal", { { "rate", 0.6f }, { "depth", 0.3f }, { "mix", 0.3f } } }, { "Lush", { { "rate", 1.2f }, { "depth", 0.6f }, { "mix", 0.5f } } }, { "Flanger", { { "delay", 2.0f }, { "feedback", 0.6f }, { "rate", 0.3f } } } } },
        { "phaser", { { "Slow Swirl", { { "rate", 0.3f }, { "depth", 0.7f } } }, { "Fast Phase", { { "rate", 4.0f }, { "depth", 0.5f } } } } },
        { "wide", { { "Wide", { { "width", 1.6f } } }, { "Subtle", { { "width", 1.2f } } }, { "Mono", { { "width", 0.0f } } } } },
        { "gain", { { "+6 dB", { { "gain", 6.0f } } }, { "-6 dB", { { "gain", -6.0f } } }, { "Mono", { { "mono", 1.0f } } }, { "Flip Phase", { { "phase", 1.0f } } } } },
        { "grit", { { "Tape", { { "mode", 0.0f }, { "drive", 0.3f } } }, { "Tube", { { "mode", 1.0f }, { "drive", 0.5f } } }, { "Fuzz", { { "mode", 2.0f }, { "drive", 0.8f }, { "mix", 0.6f } } }, { "Fold", { { "mode", 3.0f }, { "drive", 0.6f } } } } },
        { "crush", { { "8-bit", { { "bits", 8.0f }, { "down", 4.0f } } }, { "Lo-fi", { { "bits", 12.0f }, { "down", 2.0f }, { "mix", 0.6f } } }, { "Destroy", { { "bits", 4.0f }, { "down", 16.0f } } } } },
        { "glitch", { { "Stutter All", { { "s1", 1.0f }, { "s2", 1.0f }, { "s3", 1.0f }, { "s4", 1.0f }, { "s5", 1.0f }, { "s6", 1.0f }, { "s7", 1.0f }, { "s8", 1.0f }, { "s9", 1.0f }, { "s10", 1.0f }, { "s11", 1.0f }, { "s12", 1.0f }, { "s13", 1.0f }, { "s14", 1.0f }, { "s15", 1.0f }, { "s16", 1.0f } } }, { "Off-beats", { { "s1", 0.0f }, { "s2", 1.0f }, { "s3", 0.0f }, { "s4", 1.0f }, { "s5", 0.0f }, { "s6", 1.0f }, { "s7", 0.0f }, { "s8", 1.0f }, { "s9", 0.0f }, { "s10", 1.0f }, { "s11", 0.0f }, { "s12", 1.0f }, { "s13", 0.0f }, { "s14", 1.0f }, { "s15", 0.0f }, { "s16", 1.0f } } }, { "Off", { { "s1", 0.0f }, { "s2", 0.0f }, { "s3", 0.0f }, { "s4", 0.0f }, { "s5", 0.0f }, { "s6", 0.0f }, { "s7", 0.0f }, { "s8", 0.0f }, { "s9", 0.0f }, { "s10", 0.0f }, { "s11", 0.0f }, { "s12", 0.0f }, { "s13", 0.0f }, { "s14", 0.0f }, { "s15", 0.0f }, { "s16", 0.0f } } } } } };
    static const std::vector<FactoryPreset> none;
    auto it = all.find (type);
    return it != all.end() ? it->second : none;
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
            if (p.choices.size() == 2 && juce::String (p.choices[0]) == "Off")
            {
                auto* b = new juce::TextButton();   // FL-style on/off button (Normalize, Reverse, Declick, ...)
                setupButton (*b, juce::String (p.name).toUpperCase(), p.name + " on / off", true);
                b->setToggleState (p.value > p.mn + 0.5f, juce::dontSendNotification);
                const float mn = p.mn;
                b->onClick = [this, b, id, mn] { set (id, mn + (b->getToggleState() ? 1.0f : 0.0f)); onCommit(); };
                controls.add (b);
            }
            else if (! p.choices.empty())
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
        visibleCount = (int) params.size();
        layoutSize();
    }
    void layoutSize()
    {
        const int cols = juce::jlimit (1, 6, visibleCount);
        const int rows = juce::jmax (1, (visibleCount + cols - 1) / cols);
        setSize (juce::jmax (300, cols * 86 + 20), rows * 92 + 50);
    }
    std::function<void()> onAnyChange;
    void setExternal (const juce::String& id, float v)
    {
        for (size_t i = 0; i < params.size(); ++i)
        {
            if (params[i].id != id) continue;
            v = juce::jlimit (params[i].mn, params[i].mx, v);
            if (auto* tb = dynamic_cast<juce::TextButton*> (controls[(int) i])) tb->setToggleState (v > params[i].mn + 0.5f, juce::dontSendNotification);
            else if (auto* cb = dynamic_cast<juce::ComboBox*> (controls[(int) i])) cb->setSelectedItemIndex ((int) std::round (v - params[i].mn), juce::dontSendNotification);
            else if (auto* k = dynamic_cast<juce::Slider*> (controls[(int) i])) k->setValue (v, juce::dontSendNotification);
            set (id, v);
        }
    }
    float valueOf (const juce::String& id) const { for (auto& p : params) if (p.id == id) return p.value; return 0.0f; }
    void commitNow() { onCommit(); }
    void hideWhere (const std::function<bool (const juce::String&)>& hide)
    {
        visibleCount = 0;
        for (size_t i = 0; i < params.size(); ++i)
        {
            const bool h = hide (params[i].id);
            controls[(int) i]->setVisible (! h); labels[(int) i]->setVisible (! h);
            visibleCount += h ? 0 : 1;
        }
        layoutSize();
    }
    void paint (juce::Graphics& g) override { g.fillAll (col::panel); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto top = r.removeFromTop (28);
        if (kind.isNotEmpty()) presetBtn.setBounds (top.removeFromRight (80).reduced (0, 2));
        heading.setBounds (top);
        const int cols = juce::jlimit (1, 6, visibleCount);
        for (int i = 0, v = 0; i < controls.size(); ++i)
        {
            if (! controls[i]->isVisible()) continue;
            juce::Rectangle<int> cell (r.getX() + (v % cols) * 86, r.getY() + (v / cols) * 92, 82, 88);
            ++v;
            labels[i]->setBounds (cell.removeFromTop (16));
            if (dynamic_cast<juce::ComboBox*> (controls[i]) != nullptr) controls[i]->setBounds (cell.withSizeKeepingCentre (80, 24));
            else controls[i]->setBounds (cell.reduced (12, 2));
        }
    }

private:
    juce::String kind;
    int visibleCount = 0;
    juce::TextButton presetBtn;
    std::unique_ptr<juce::FileChooser> chooser;
    void set (const juce::String& id, float v)
    {
        for (auto& p : params) if (p.id == id) p.value = v;
        onSet (id, v);
        if (onAnyChange) onAnyChange();
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
        const auto& fac = factoryPresets (kind);
        if (! fac.empty()) menu.addSectionHeader ("Factory presets");
        for (size_t i = 0; i < fac.size(); ++i) menu.addItem (5000 + (int) i, fac[i].name);
        auto files = presetDir().findChildFiles (juce::File::findFiles, false, "*.hhpreset");
        files.sort();
        if (! files.isEmpty()) menu.addSectionHeader ("Your presets");
        for (int i = 0; i < files.size() && i < 200; ++i) menu.addItem (100 + i, files[i].getFileNameWithoutExtension());
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetBtn), [this, files] (int r) {
            if (r == 1) savePreset();
            else if (r >= 5000 && r - 5000 < (int) factoryPresets (kind).size()) loadFactory (factoryPresets (kind)[(size_t) (r - 5000)]);
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
    void loadFactory (const FactoryPreset& fp)
    {
        std::vector<std::pair<juce::String, float>> defs;
        for (auto& p : params) defs.push_back ({ p.id, p.def });
        for (auto& [id, v] : defs) setExternal (id, v);
        for (auto& [id, v] : fp.values) setExternal (id, v);
        onCommit();
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

// ---- visual editors: EQ curve with draggable bands, compressor transfer curve + GR, effect pictures
static void drawPanelBg (juce::Graphics& g, juce::Rectangle<float> a)
{
    g.setColour (col::bg.darker (0.35f)); g.fillRoundedRectangle (a, 6.0f);
    g.setColour (col::grid.withAlpha (0.6f)); g.drawRoundedRectangle (a, 6.0f, 1.0f);
}

class EqGraph : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    EqGraph (ParamGrid& g, std::function<BuiltinEffect*()> liveFx, std::function<ScopeTap*()> src)
        : grid (g), live (std::move (liveFx)), tap (std::move (src))
    {
        model = createBuiltinEffect ("steq7");
        if (model) model->prepare (48000, 512);
        snap.assign (fftN, 0.0f); fftBuf.assign (fftN * 2, 0.0f); spec.assign (fftN / 2, -100.0f);
        static const char* tn[] { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass" };
        for (int t = 0; t < 7; ++t) typeBox.addItem (tn[t], t + 1);
        typeBox.onChange = [this] { if (sel >= 0) { grid.setExternal (pid (sel, "T"), (float) typeBox.getSelectedItemIndex()); grid.commitNow(); repaint(); } };
        for (auto* l : { &fL, &gL, &qL })
        {
            l->setEditable (false, true);
            l->setJustificationType (juce::Justification::centred);
            l->setColour (juce::Label::backgroundColourId, col::panel2);
            l->setColour (juce::Label::outlineColourId, col::grid);
            l->setTooltip ("Double-click to type a value");
            addAndMakeVisible (l);
        }
        fL.onTextChange = [this] { if (sel < 0) return; auto t = fL.getText().toLowerCase(); float v = t.getFloatValue(); if (t.containsChar ('k')) v *= 1000.0f;
                                   grid.setExternal (pid (sel, "F"), juce::jlimit (20.0f, 20000.0f, v)); grid.commitNow(); repaint(); };
        gL.onTextChange = [this] { if (sel < 0) return; grid.setExternal (pid (sel, "G"), juce::jlimit (-18.0f, 18.0f, gL.getText().getFloatValue())); grid.commitNow(); repaint(); };
        qL.onTextChange = [this] { if (sel < 0) return; grid.setExternal (pid (sel, "Q"), juce::jlimit (0.1f, 18.0f, qL.getText().getFloatValue())); grid.commitNow(); repaint(); };
        dynB.setClickingTogglesState (true); listenB.setClickingTogglesState (true);
        dynB.setTooltip ("Dynamic EQ for this band"); listenB.setTooltip ("Listen: hear only this band (or Alt+drag a dot)"); delB.setTooltip ("Delete band");
        dynB.onClick = [this] { if (sel >= 0) { grid.setExternal (pid (sel, "Dyn"), dynB.getToggleState() ? 1.0f : 0.0f); grid.commitNow(); repaint(); } };
        listenB.onClick = [this] { if (sel >= 0) { grid.setExternal ("solo", listenB.getToggleState() ? (float) (sel + 1) : 0.0f); grid.commitNow(); } };
        delB.onClick = [this] { if (sel >= 0) { grid.setExternal (pid (sel, "On"), 0.0f); grid.setExternal ("solo", 0.0f); grid.commitNow(); sel = -1; syncBar(); repaint(); } };
        for (juce::Component* c : { (juce::Component*) &typeBox, (juce::Component*) &dynB, (juce::Component*) &listenB, (juce::Component*) &delB }) addAndMakeVisible (c);
        syncBar();
        setTooltip ("Double-click empty space = add a band (up to 12). Drag a dot = frequency + gain. Wheel = width (Q). Double-click a dot = reset gain. Right-click a dot = type, Dynamic EQ, delete. Click NATURAL / LINEAR to switch phase.");
        startTimerHz (30);
    }
    void paint (juce::Graphics& g) override
    {
        auto a = area();
        drawPanelBg (g, getLocalBounds().toFloat());
        g.setFont (juce::FontOptions (10.0f));
        for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const float x = xOf (f);
            g.setColour (col::grid.withAlpha (0.5f)); g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
            g.setColour (col::dim); g.drawText (f >= 1000 ? juce::String (f / 1000, 0) + "k" : juce::String (f, 0), (int) x + 2, (int) a.getBottom() - 12, 30, 12, juce::Justification::left);
        }
        std::vector<int> ticks;
        const int stepDb = range <= 6 ? 3 : range <= 12 ? 6 : range <= 20 ? 6 : 10;
        for (int d = -(int) range / stepDb * stepDb; d <= (int) range; d += stepDb) ticks.push_back (d);
        for (int d : ticks)
        {
            g.setColour (col::grid.withAlpha (d == 0 ? 0.9f : 0.35f)); g.drawHorizontalLine ((int) yOf ((float) d), a.getX(), a.getRight());
            if (d != 0) { g.setColour (col::dim); g.drawText (juce::String (d), (int) a.getX() + 2, (int) yOf ((float) d) - 6, 24, 12, juce::Justification::left); }
        }
        if (rate > 0)
        {
            juce::Path sp; bool started = false;
            for (int i = 1; i < fftN / 2; ++i)
            {
                const double f = i * rate / fftN;
                if (f < 20 || f > 20000) continue;
                const float y = juce::jmap (juce::jlimit (-90.0f, 0.0f, spec[(size_t) i]), -90.0f, 0.0f, a.getBottom(), a.getY() + a.getHeight() * 0.15f);
                if (! started) { sp.startNewSubPath (xOf (f), a.getBottom()); started = true; }
                sp.lineTo (xOf (f), y);
            }
            if (started) { sp.lineTo (a.getRight(), a.getBottom()); sp.closeSubPath(); g.setColour (col::text.withAlpha (0.08f)); g.fillPath (sp); }
        }
        if (model)
        {
            for (auto& d : model->params()) model->setParam (d.id, grid.valueOf (d.id));
            juce::Path c;
            for (float x = a.getX(); x <= a.getRight(); x += 2.0f)
            {
                const float y = yOf (model->responseDb (freqAt (x)));
                if (x == a.getX()) c.startNewSubPath (x, y); else c.lineTo (x, y);
            }
            juce::Path fill (c); fill.lineTo (a.getRight(), yOf (0)); fill.lineTo (a.getX(), yOf (0)); fill.closeSubPath();
            g.setColour (col::accent.withAlpha (0.16f)); g.fillPath (fill);
            if (sel >= 0 && isOn (sel))   // selected band's own shape, Pro-Q style
            {
                for (int k = 0; k < bands; ++k) if (k != sel) model->setParam (pid (k, "On"), 0.0f);
                model->setParam ("out", 0.0f);
                juce::Path bs; bs.startNewSubPath (a.getX(), yOf (0));
                for (float x = a.getX(); x <= a.getRight(); x += 2.0f) bs.lineTo (x, yOf (model->responseDb (freqAt (x))));
                bs.lineTo (a.getRight(), yOf (0)); bs.closeSubPath();
                g.setColour (colourOf (sel).withAlpha (0.28f)); g.fillPath (bs);
                for (auto& d : model->params()) model->setParam (d.id, grid.valueOf (d.id));
            }
            g.setColour (col::accent.withAlpha (0.35f)); g.strokePath (c, juce::PathStrokeType (5.0f));
            g.setColour (col::accent); g.strokePath (c, juce::PathStrokeType (2.0f));
        }
        auto* fx = live ? live() : nullptr;
        for (int k = 0; k < bands; ++k)
        {
            if (! isOn (k)) continue;
            auto pt = dotOf (k);
            const auto c = colourOf (k);
            if (hasGain (k) && grid.valueOf (pid (k, "Dyn")) > 0.5f)
            {
                const float yr = yOf (grid.valueOf (pid (k, "G")) + (fx ? fx->meter (k) : 0.0f));
                g.setColour (c.withAlpha (0.5f)); g.drawLine (pt.x, pt.y, pt.x, yr, 1.5f);
                g.setColour (c); g.drawEllipse (pt.x - 10, yr - 10, 20, 20, 2.0f);
            }
            g.setColour (c.withAlpha (k == hot ? 0.45f : 0.2f)); g.fillEllipse (pt.x - 11, pt.y - 11, 22, 22);
            g.setColour (c); g.fillEllipse (pt.x - 7, pt.y - 7, 14, 14);
            g.setColour (juce::Colours::black.withAlpha (0.8f)); g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
            g.drawText (juce::String (k + 1), juce::Rectangle<float> (pt.x - 7, pt.y - 7, 14, 14), juce::Justification::centred);
            if (k == hot)
            {
                static const char* tn[] { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass" };
                juce::String t = juce::String (tn[typeOf (k)]) + "  " + hzText (grid.valueOf (pid (k, "F")));
                if (hasGain (k)) t << "  " << juce::String (grid.valueOf (pid (k, "G")), 1) << " dB";
                t << "  Q " << juce::String (grid.valueOf (pid (k, "Q")), 2);
                g.setColour (col::text); g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
                g.drawText (t, (int) pt.x - 110, (int) pt.y - 30, 220, 14, juce::Justification::centred);
            }
        }
        if (hover.x >= a.getX() && hot < 0 && drag < 0 && a.contains (hover))
        {
            const double hz = freqAt (hover.x);
            static const char* nn[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            const int midi = juce::roundToInt (69.0 + 12.0 * std::log2 (hz / 440.0));
            g.setColour (col::dim); g.setFont (juce::FontOptions (11.0f));
            g.drawText (hzText ((float) hz) + "  " + nn[((midi % 12) + 12) % 12] + juce::String (midi / 12) + "  " + juce::String (dbAt (hover.y), 1) + " dB",
                        (int) a.getX() + 30, (int) a.getY() + 2, 220, 14, juce::Justification::left);
        }
        g.setColour (col::panel2.withAlpha (0.9f)); g.fillRect (getLocalBounds().removeFromBottom (barH + 4).reduced (6, 2).toFloat());
        g.setColour (sel >= 0 ? colourOf (sel) : col::dim); g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (sel >= 0 ? "BAND " + juce::String (sel + 1) : "Click a dot to edit it here", getLocalBounds().removeFromBottom (barH + 4).reduced (12, 2), juce::Justification::centredLeft);
        auto rc = rangeChip();
        g.setColour (col::panel2); g.fillRoundedRectangle (rc, 4.0f);
        g.setColour (col::text); g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (juce::String::charToString (0xb1) + juce::String ((int) range) + " dB", rc, juce::Justification::centred);
        const bool lin = grid.valueOf ("phase") > 0.5f;
        auto chip = modeChip();
        g.setColour (lin ? col::accent2.withAlpha (0.3f) : col::good.withAlpha (0.25f)); g.fillRoundedRectangle (chip, 4.0f);
        g.setColour (col::text); g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (lin ? "LINEAR  ~23 ms" : "NATURAL  0 ms", chip, juce::Justification::centred);
    }
    void resized() override
    {
        auto b = getLocalBounds().removeFromBottom (barH + 4).reduced (6, 4).withTrimmedLeft (110);
        typeBox.setBounds (b.removeFromLeft (110)); b.removeFromLeft (6);
        for (auto* l : { &fL, &gL, &qL }) { l->setBounds (b.removeFromLeft (84)); b.removeFromLeft (6); }
        dynB.setBounds (b.removeFromLeft (50)); b.removeFromLeft (4);
        listenB.setBounds (b.removeFromLeft (64)); b.removeFromLeft (4);
        delB.setBounds (b.removeFromLeft (28));
    }
    void mouseMove (const juce::MouseEvent& e) override { hover = e.position; const int h = hit (e.position); hot = h; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hot = -1; hover = { -1, -1 }; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (modeChip().contains (e.position)) { grid.setExternal ("phase", grid.valueOf ("phase") > 0.5f ? 0.0f : 1.0f); grid.commitNow(); repaint(); return; }
        if (rangeChip().contains (e.position)) { range = range <= 6 ? 12.0f : range <= 12 ? 20.0f : range <= 20 ? 30.0f : 6.0f; repaint(); return; }
        drag = hit (e.position); hot = drag;
        sel = drag; syncBar();
        if (drag >= 0 && e.mods.isPopupMenu()) { bandMenu (drag); drag = -1; return; }
        if (drag >= 0)
        {
            drag0 = e.position; f0 = grid.valueOf (pid (drag, "F")); g0 = grid.valueOf (pid (drag, "G"));
            if (e.mods.isAltDown()) { altSolo = true; grid.setExternal ("solo", (float) (drag + 1)); grid.commitNow(); }
        }
        repaint();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag < 0) return;
        const float fine = e.mods.isShiftDown() ? 0.1f : 1.0f;   // Shift = fine moves
        const float x = drag0.x + (e.position.x - drag0.x) * fine, y = drag0.y + (e.position.y - drag0.y) * fine;
        grid.setExternal (pid (drag, "F"), (float) juce::jlimit (20.0, 20000.0, f0 * std::pow (1000.0, (double) ((x - drag0.x) / area().getWidth()))));
        if (hasGain (drag)) grid.setExternal (pid (drag, "G"), juce::jlimit (-18.0f, 18.0f, g0 + (dbAt (y) - dbAt (drag0.y))));
        syncBar(); repaint();
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (altSolo) { grid.setExternal ("solo", 0.0f); altSolo = false; }
        if (drag >= 0) grid.commitNow();
        drag = -1;
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int k = hit (e.position);
        if (k >= 0) { if (hasGain (k)) grid.setExternal (pid (k, "G"), 0.0f); grid.commitNow(); repaint(); return; }
        if (modeChip().contains (e.position)) return;
        for (int b = 0; b < bands; ++b)
            if (! isOn (b))
            {
                grid.setExternal (pid (b, "T"), 0.0f); grid.setExternal (pid (b, "F"), (float) freqAt (e.position.x));
                grid.setExternal (pid (b, "G"), juce::jlimit (-18.0f, 18.0f, dbAt (e.position.y))); grid.setExternal (pid (b, "Q"), 1.0f);
                grid.setExternal (pid (b, "Dyn"), 0.0f); grid.setExternal (pid (b, "On"), 1.0f);
                grid.commitNow(); hot = sel = b; syncBar(); repaint();
                return;
            }
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const int k = hit (e.position);
        if (k < 0) return;
        grid.setExternal (pid (k, "Q"), juce::jlimit (0.1f, 18.0f, grid.valueOf (pid (k, "Q")) * std::pow (1.12f, juce::jlimit (-3.0f, 3.0f, w.deltaY * 6.0f))));
        grid.commitNow(); syncBar(); repaint();
    }
private:
    static constexpr int bands = 12;
    static juce::String pid (int k, const char* f) { return "e" + juce::String (k + 1) + f; }
    static juce::String hzText (float f) { return f >= 1000 ? juce::String (f / 1000.0f, 2) + " kHz" : juce::String ((int) f) + " Hz"; }
    juce::Colour colourOf (int k) const
    {
        static const juce::uint32 c[] { 0xff9aa0aa, 0xffff5d5d, 0xffffa53d, 0xfff5e04a, 0xff4ade80, 0xff38bdf8, 0xffa78bfa,
                                        0xfff472b6, 0xff2dd4bf, 0xfffb923c, 0xffa3e635, 0xff818cf8 };
        return juce::Colour (c[k % 12]);
    }
    bool isOn (int k) const { return grid.valueOf (pid (k, "On")) > 0.5f; }
    int  typeOf (int k) const { return juce::jlimit (0, 6, (int) std::round (grid.valueOf (pid (k, "T")))); }
    bool hasGain (int k) const { return typeOf (k) <= 2; }
    juce::Rectangle<float> area() const { return getLocalBounds().toFloat().reduced (6.0f, 8.0f).withTrimmedBottom ((float) barH); }
    juce::Rectangle<float> modeChip() const { return { (float) getWidth() - 128.0f, 8.0f, 118.0f, 20.0f }; }
    juce::Rectangle<float> rangeChip() const { return { (float) getWidth() - 196.0f, 8.0f, 62.0f, 20.0f }; }
    void syncBar()
    {
        const bool on = sel >= 0 && isOn (sel);
        for (juce::Component* c : { (juce::Component*) &typeBox, (juce::Component*) &fL, (juce::Component*) &gL, (juce::Component*) &qL,
                                    (juce::Component*) &dynB, (juce::Component*) &listenB, (juce::Component*) &delB }) c->setVisible (on);
        if (! on) return;
        typeBox.setSelectedItemIndex (typeOf (sel), juce::dontSendNotification);
        if (! fL.isBeingEdited()) fL.setText (hzText (grid.valueOf (pid (sel, "F"))), juce::dontSendNotification);
        if (! gL.isBeingEdited()) gL.setText (hasGain (sel) ? juce::String (grid.valueOf (pid (sel, "G")), 1) + " dB" : "--", juce::dontSendNotification);
        if (! qL.isBeingEdited()) qL.setText ("Q " + juce::String (grid.valueOf (pid (sel, "Q")), 2), juce::dontSendNotification);
        gL.setEnabled (hasGain (sel)); dynB.setEnabled (hasGain (sel));
        dynB.setToggleState (grid.valueOf (pid (sel, "Dyn")) > 0.5f, juce::dontSendNotification);
        listenB.setToggleState ((int) std::round (grid.valueOf ("solo")) == sel + 1, juce::dontSendNotification);
    }
    float xOf (double f) const { auto a = area(); return a.getX() + a.getWidth() * (float) (std::log (juce::jlimit (20.0, 20000.0, f) / 20.0) / std::log (1000.0)); }
    double freqAt (float x) const { auto a = area(); return 20.0 * std::pow (1000.0, juce::jlimit (0.0, 1.0, (double) ((x - a.getX()) / a.getWidth()))); }
    float yOf (float db) const { auto a = area(); return a.getCentreY() - juce::jlimit (-range, range, db) / range * a.getHeight() * 0.5f; }
    float dbAt (float y) const { auto a = area(); return (a.getCentreY() - y) / (a.getHeight() * 0.5f) * range; }
    juce::Point<float> dotOf (int k) const { return { xOf (grid.valueOf (pid (k, "F"))), yOf (hasGain (k) ? grid.valueOf (pid (k, "G")) : 0.0f) }; }
    int hit (juce::Point<float> p) const
    {
        int best = -1; float bd = 14.0f;
        for (int k = 0; k < bands; ++k) if (isOn (k)) { const float d = dotOf (k).getDistanceFrom (p); if (d < bd) { bd = d; best = k; } }
        return best;
    }
    void bandMenu (int k)
    {
        juce::PopupMenu m, th, rg, at, rl;
        m.addSectionHeader ("Band " + juce::String (k + 1));
        static const char* tn[] { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass" };
        for (int t = 0; t < 7; ++t) m.addItem (20 + t, tn[t], true, typeOf (k) == t);
        m.addSeparator();
        if (hasGain (k))
        {
            const bool on = grid.valueOf (pid (k, "Dyn")) > 0.5f;
            m.addItem (1, "Dynamic", true, on);
            m.addItem (2, "Only this band dynamic");
            m.addItem (3, "Make all bands dynamic");
            m.addItem (4, "Make all bands static");
            for (int v : { 12, 24, 36, 48 }) th.addItem (100 + v, "-" + juce::String (v) + " dB", true, (int) std::round (-grid.valueOf (pid (k, "DTh"))) == v);
            m.addSubMenu ("Dynamic threshold", th);
            for (int v : { -12, -6, -3, 3, 6, 12 }) rg.addItem (250 + v, (v > 0 ? "+" : "") + juce::String (v) + " dB" + (v < 0 ? " (cut when loud)" : " (boost when loud)"), true, (int) std::round (grid.valueOf (pid (k, "DRg"))) == v);
            m.addSubMenu ("Dynamic range", rg);
            for (int v : { 1, 5, 20, 50 }) at.addItem (400 + v, juce::String (v) + " ms", true, (int) std::round (grid.valueOf (pid (k, "DAt"))) == v);
            m.addSubMenu ("Dynamic attack", at);
            for (int v : { 50, 120, 300, 800 }) rl.addItem (1000 + v, juce::String (v) + " ms", true, (int) std::round (grid.valueOf (pid (k, "DRl"))) == v);
            m.addSubMenu ("Dynamic release", rl);
            m.addSeparator();
        }
        m.addItem (8, "Listen (solo this band)", true, (int) std::round (grid.valueOf ("solo")) == k + 1);
        m.addItem (9, "Delete band");
        juce::Component::SafePointer<EqGraph> safe (this);
        m.showMenuAsync ({}, [safe, k] (int r) {
            if (safe == nullptr || r <= 0) return;
            auto& gr = safe->grid;
            const bool on = gr.valueOf (pid (k, "Dyn")) > 0.5f;
            if (r == 8) gr.setExternal ("solo", (int) std::round (gr.valueOf ("solo")) == k + 1 ? 0.0f : (float) (k + 1));
            else if (r == 9) { gr.setExternal (pid (k, "On"), 0.0f); gr.setExternal ("solo", 0.0f); }
            else if (r >= 20 && r < 27) gr.setExternal (pid (k, "T"), (float) (r - 20));
            else if (r == 1) gr.setExternal (pid (k, "Dyn"), on ? 0.0f : 1.0f);
            else if (r == 2) for (int i = 0; i < bands; ++i) gr.setExternal (pid (i, "Dyn"), i == k ? 1.0f : 0.0f);
            else if (r == 3 || r == 4) for (int i = 0; i < bands; ++i) gr.setExternal (pid (i, "Dyn"), r == 3 ? 1.0f : 0.0f);
            else if (r < 200) { gr.setExternal (pid (k, "DTh"), (float) -(r - 100)); gr.setExternal (pid (k, "Dyn"), 1.0f); }
            else if (r < 400) { gr.setExternal (pid (k, "DRg"), (float) (r - 250)); gr.setExternal (pid (k, "Dyn"), 1.0f); }
            else if (r < 1000) gr.setExternal (pid (k, "DAt"), (float) (r - 400));
            else gr.setExternal (pid (k, "DRl"), (float) (r - 1000));
            gr.commitNow(); safe->syncBar(); safe->repaint();
        });
    }
    void timerCallback() override
    {
        if (! isShowing()) return;
        if (auto* t = tap ? tap() : nullptr)
        {
            rate = t->rate.load();
            t->read (snap.data(), fftN, t->pos.load());
            std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);
            for (int i = 0; i < fftN; ++i) fftBuf[(size_t) i] = snap[(size_t) i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftN));
            fft.performFrequencyOnlyForwardTransform (fftBuf.data());
            for (int i = 0; i < fftN / 2; ++i)
            {
                const float db = juce::Decibels::gainToDecibels (fftBuf[(size_t) i] / (float) (fftN / 4), -100.0f);
                spec[(size_t) i] = db > spec[(size_t) i] ? db : spec[(size_t) i] * 0.85f + db * 0.15f;
            }
        }
        if (drag < 0) syncBar();
        repaint();
    }
    static constexpr int fftOrder = 11, fftN = 1 << fftOrder;
    ParamGrid& grid;
    std::function<BuiltinEffect*()> live;
    std::function<ScopeTap*()> tap;
    std::unique_ptr<BuiltinEffect> model;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> snap, fftBuf, spec;
    double rate = 0;
    int hot = -1, drag = -1, sel = -1;
    static constexpr int barH = 30;
    float range = 20.0f, f0 = 1000.0f, g0 = 0.0f;
    bool altSolo = false;
    juce::Point<float> hover { -1, -1 }, drag0;
    juce::ComboBox typeBox;
    juce::Label fL, gL, qL;
    juce::TextButton dynB { "DYN" }, listenB { "LISTEN" }, delB { "X" };
};

// compressor: transfer curve (in -> out), threshold line and live gain-reduction meter
class DynGraph : public juce::Component, private juce::Timer
{
public:
    DynGraph (ParamGrid& g, std::function<BuiltinEffect*()> liveFx, juce::String th, juce::String ratio, juce::String knee)
        : grid (g), live (std::move (liveFx)), thId (std::move (th)), ratioId (std::move (ratio)), kneeId (std::move (knee)) { startTimerHz (30); }
    void paint (juce::Graphics& g) override
    {
        drawPanelBg (g, getLocalBounds().toFloat());
        auto r = getLocalBounds().toFloat().reduced (10.0f);
        auto meter = r.removeFromRight (40.0f); r.removeFromRight (12.0f);
        auto sq = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), r.getHeight()), juce::jmin (r.getWidth(), r.getHeight()));
        const float th = grid.valueOf (thId), ratio = juce::jmax (1.0f, grid.valueOf (ratioId)), knee = kneeId.isNotEmpty() ? grid.valueOf (kneeId) : 6.0f;
        auto px = [&] (float db) { return sq.getX() + (db + 60.0f) / 60.0f * sq.getWidth(); };
        auto py = [&] (float db) { return sq.getBottom() - (juce::jlimit (-60.0f, 0.0f, db) + 60.0f) / 60.0f * sq.getHeight(); };
        g.setColour (col::grid.withAlpha (0.5f));
        for (int d = -48; d < 0; d += 12) { g.drawVerticalLine ((int) px ((float) d), sq.getY(), sq.getBottom()); g.drawHorizontalLine ((int) py ((float) d), sq.getX(), sq.getRight()); }
        g.setColour (col::dim.withAlpha (0.6f)); g.drawLine (sq.getX(), sq.getBottom(), sq.getRight(), sq.getY(), 1.0f);
        juce::Path c;
        for (float in = -60.0f; in <= 0.0f; in += 0.5f)
        {
            const float over = in - th;
            float out = in;
            if (knee > 0 && 2 * std::abs (over) <= knee) out = in + (1 / ratio - 1) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
            else if (2 * over > knee) out = th + over / ratio;
            if (in == -60.0f) c.startNewSubPath (px (in), py (out)); else c.lineTo (px (in), py (out));
        }
        g.setColour (col::accent.withAlpha (0.35f)); g.strokePath (c, juce::PathStrokeType (5.0f));
        g.setColour (col::accent); g.strokePath (c, juce::PathStrokeType (2.0f));
        g.setColour (col::accent2.withAlpha (0.7f)); g.drawVerticalLine ((int) px (th), sq.getY(), sq.getBottom());
        g.setColour (col::text); g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText ("THRESH " + juce::String (th, 1) + " dB   RATIO " + juce::String (ratio, 1) + ":1", sq.toNearestInt().removeFromTop (16), juce::Justification::centredLeft);
        const float gr = std::abs (live && live() ? live()->meter (0) : 0.0f);
        shown = std::max (gr, shown * 0.9f);
        g.setColour (col::bg); g.fillRoundedRectangle (meter, 4.0f);
        g.setColour (col::rec.withAlpha (0.85f));
        g.fillRoundedRectangle (meter.withHeight (meter.getHeight() * juce::jlimit (0.0f, 1.0f, shown / 24.0f)), 4.0f);
        g.setColour (col::text); g.setFont (juce::FontOptions (10.0f));
        g.drawText ("GR", meter.toNearestInt().removeFromBottom (28), juce::Justification::centred);
        g.drawText ("-" + juce::String (shown, 1), meter.toNearestInt().removeFromBottom (14), juce::Justification::centred);
    }
private:
    void timerCallback() override { if (isShowing()) repaint(); }
    ParamGrid& grid;
    std::function<BuiltinEffect*()> live;
    juce::String thId, ratioId, kneeId;
    float shown = 0;
};

// simple pictures of what an effect does (reverb tail, echo taps, filter curve)
class FxPicture : public juce::Component, private juce::Timer
{
public:
    FxPicture (ParamGrid& g, juce::String t) : grid (g), type (std::move (t)) { startTimerHz (15); }
    void paint (juce::Graphics& g) override
    {
        drawPanelBg (g, getLocalBounds().toFloat());
        auto a = getLocalBounds().toFloat().reduced (12.0f, 14.0f);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        if (type == "verb")
        {
            const float size = grid.valueOf ("size"), damp = grid.valueOf ("damp"), mix = grid.valueOf ("mix");
            const float decay = 0.08f + 0.9f * size;
            juce::Random rnd (7);
            const auto c = col::accent.interpolatedWith (col::accent2, damp);
            for (float x = 0; x < a.getWidth(); x += 3.0f)
            {
                const float t = x / a.getWidth();
                const float h = (t < 0.02f ? t / 0.02f : std::exp (-t / decay)) * (0.55f + 0.45f * rnd.nextFloat()) * (0.3f + 0.7f * mix);
                g.setColour (c.withAlpha (0.35f + 0.6f * (1.0f - t)));
                g.fillRect (a.getX() + x, a.getBottom() - h * a.getHeight(), 2.0f, h * a.getHeight());
            }
            g.setColour (col::text); g.drawText ("REVERB TAIL  size " + juce::String ((int) (size * 100)) + "%  damp " + juce::String ((int) (damp * 100)) + "%", a.toNearestInt().removeFromTop (14), juce::Justification::centredLeft);
        }
        else if (type == "delay")
        {
            static const float spacing[] { 0.125f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
            const int ti = juce::jlimit (0, 6, (int) std::round (grid.valueOf ("time")));
            const float fb = grid.valueOf ("feedback"), mix = grid.valueOf ("mix");
            const bool pp = grid.valueOf ("pingpong") > 0.5f;
            const float step = a.getWidth() * spacing[ti] / 4.0f;
            g.setColour (col::text); g.fillRect (a.getX(), a.getY() + 16, 4.0f, a.getHeight() - 16);
            float h = mix;
            for (int k = 1; a.getX() + k * step < a.getRight() && h > 0.01f; ++k, h *= fb)
            {
                const float x = a.getX() + k * step, hh = (a.getHeight() - 16) * juce::jmin (1.0f, h * 1.6f);
                g.setColour ((pp && (k % 2 == 0) ? col::accent2 : col::accent).withAlpha (0.9f));
                g.fillRoundedRectangle (x, a.getBottom() - hh, 4.0f, hh, 2.0f);
            }
            g.setColour (col::text); g.drawText ("ECHO TAPS  feedback " + juce::String ((int) (fb * 100)) + "%" + (pp ? "  ping-pong" : ""), a.toNearestInt().removeFromTop (14), juce::Justification::centredLeft);
        }
        else if (type == "filter")
        {
            const int mode = (int) std::round (grid.valueOf ("mode"));
            const float cut = grid.valueOf ("cutoff"), res = grid.valueOf ("res");
            Biquad q; q.set (mode == 0 ? Biquad::LowPass : mode == 1 ? Biquad::HighPass : Biquad::BandPass, 48000, cut, 0.707 + res * 8.0, 0);
            juce::Path c;
            for (float x = 0; x <= a.getWidth(); x += 2.0f)
            {
                const double f = 20.0 * std::pow (1000.0, x / a.getWidth()), w = juce::MathConstants<double>::twoPi * f / 48000.0;
                const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
                const double m = std::abs (((double) q.b0 + (double) q.b1 * z1 + (double) q.b2 * z2) / (1.0 + (double) q.a1 * z1 + (double) q.a2 * z2));
                const float db = juce::jlimit (-36.0f, 18.0f, (float) (20.0 * std::log10 (std::max (1.0e-6, m))));
                const float y = juce::jmap (db, -36.0f, 18.0f, a.getBottom(), a.getY() + 16);
                if (x == 0) c.startNewSubPath (a.getX() + x, y); else c.lineTo (a.getX() + x, y);
            }
            g.setColour (col::accent.withAlpha (0.35f)); g.strokePath (c, juce::PathStrokeType (5.0f));
            g.setColour (col::accent); g.strokePath (c, juce::PathStrokeType (2.0f));
            g.setColour (col::text); g.drawText ("FILTER  " + juce::String (cut >= 1000 ? juce::String (cut / 1000, 1) + " kHz" : juce::String ((int) cut) + " Hz"), a.toNearestInt().removeFromTop (14), juce::Justification::centredLeft);
        }
    }
private:
    void timerCallback() override { if (isShowing()) repaint(); }
    ParamGrid& grid;
    juce::String type;
};

class FxEditor : public juce::Component
{
public:
    FxEditor (ParamGrid* g, juce::Component* v, int visualH) : grid (g), visual (v), vh (visualH)
    {
        addAndMakeVisible (*grid); addAndMakeVisible (*visual);
        setSize (juce::jmax (640, grid->getWidth()), vh + grid->getHeight());
    }
    void paint (juce::Graphics& g) override { g.fillAll (col::panel); }
    void resized() override { auto r = getLocalBounds(); visual->setBounds (r.removeFromTop (vh).reduced (10, 8)); grid->setBounds (r); }
private:
    std::unique_ptr<ParamGrid> grid;
    std::unique_ptr<juce::Component> visual;
    int vh;
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

    if (slot->type == "scope")
    {
        auto* view = createScopeView ([&s, track, slotUid]() -> ScopeTap* {
            auto* sc = dynamic_cast<ScopeFx*> (s.engine.builtinFx (track, slotUid));
            return sc != nullptr ? sc->tap.get() : nullptr; }, trackName);
        auto* w = new EditorWindow ("Scope - " + trackName, view, key, [&s, track, slotUid] { return findSlot (s, track, slotUid) != nullptr; });
        w->setResizable (true, false); w->setResizeLimits (420, 240, 3000, 2000);
        EditorWindow::windows().add (w);
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
    auto liveFx = [&s, track, slotUid] { return s.engine.builtinFx (track, slotUid); };
    auto tapFn = [&s, track]() -> ScopeTap* { return s.engine.trackScope (track); };
    const auto& ty = slot->type;
    juce::Component* visual = nullptr;
    int vh = 170;
    if (ty == "steq7")
    {
        grid->hideWhere ([] (const juce::String& id) { return id.startsWithChar ('e') || id == "solo"; });   // graph-first: only Output + Phase knobs
        visual = new EqGraph (*grid, liveFx, tapFn); vh = 360;
    }
    else if (ty == "stdyn" || ty == "comp") visual = new DynGraph (*grid, liveFx, "threshold", "ratio", "knee");
    else if (ty == "strip") visual = new DynGraph (*grid, liveFx, "compTh", "compRatio", {});
    else if (ty == "verb" || ty == "delay" || ty == "filter") visual = new FxPicture (*grid, ty);
    else visual = createScopeView (tapFn, trackName);
    auto* editor = new FxEditor (grid, visual, vh);
    EditorWindow::windows().add (new EditorWindow (title + " - " + trackName, editor, key,
                                                   [&s, track, slotUid] { return findSlot (s, track, slotUid) != nullptr; }));
}


// FL-style channel settings: SMP / INS / FUNC pages (the "wrench" panel)
// sampler WAVE page: the sample after SMP edits (normalize, reverse, trim, fades, stretch), Ableton-style
struct SampleWaveView : juce::Component, private juce::Timer
{
    SampleWaveView (Studio& st, int id) : s (st), channelId (id) { setSize (560, 300); startTimer (250); timerCallback(); }
    void timerCallback() override
    {
        auto* c = s.channel (channelId);
        if (c == nullptr) return;
        const juce::String k = c->sampleFile.getFullPathName() + juce::String ((int) c->smpNormalize) + juce::String ((int) c->smpReverse)
            + juce::String ((int) c->smpInvert) + juce::String ((int) c->smpDeclick) + juce::String ((int) c->smpTrim) + juce::String (c->smpBoostDb)
            + "|" + juce::String (c->smpFadeIn) + "|" + juce::String (c->smpFadeOut) + "|" + juce::String (c->stretchMode) + "|" + juce::String (c->stretchTime);
        if (k == key) return;
        key = k;
        const Channel copy = *c;
        auto raw = loadSampleFile (copy.sampleFile, 44100.0);
        auto done = raw != nullptr ? processSample (raw, copy, 44100.0) : nullptr;
        peaks.clear();
        if (done != nullptr && done->getNumSamples() > 0)
        {
            const int cols = 1024, n = done->getNumSamples();
            for (int x = 0; x < cols; ++x)
            {
                const int a = (int) ((int64_t) n * x / cols), b = juce::jmax (a + 1, (int) ((int64_t) n * (x + 1) / cols));
                float lo = 0, hi = 0;
                for (int ch = 0; ch < done->getNumChannels(); ++ch)
                {
                    auto r = done->findMinMax (ch, a, juce::jmin (b, n) - a);
                    lo = juce::jmin (lo, r.getStart()); hi = juce::jmax (hi, r.getEnd());
                }
                peaks.push_back ({ lo, hi });
            }
            seconds = (double) n / 44100.0;
        }
        fadeIn = copy.smpFadeIn; fadeOut = copy.smpFadeOut;
        repaint();
    }
    void paint (juce::Graphics& g) override
    {
        auto a = getLocalBounds().toFloat().reduced (8);
        g.fillAll (col::panel);
        g.setColour (col::bg); g.fillRoundedRectangle (a, 6);
        g.setColour (col::grid);
        for (int i = 1; i < 8; ++i) g.drawVerticalLine ((int) (a.getX() + a.getWidth() * (float) i / 8.0f), a.getY(), a.getBottom());
        g.drawHorizontalLine ((int) a.getCentreY(), a.getX(), a.getRight());
        if (peaks.empty()) { g.setColour (col::dim); g.drawText ("No sample loaded", a, juce::Justification::centred); return; }
        juce::Path p; const float mid = a.getCentreY(), hh = a.getHeight() * 0.46f;
        for (size_t i = 0; i < peaks.size(); ++i)
            p.lineTo (a.getX() + a.getWidth() * (float) i / (float) (peaks.size() - 1), mid - peaks[i].second * hh);
        for (size_t i = peaks.size(); i-- > 0;)
            p.lineTo (a.getX() + a.getWidth() * (float) i / (float) (peaks.size() - 1), mid - peaks[i].first * hh);
        p.closeSubPath();
        g.setColour (col::accent.withAlpha (0.25f)); g.fillPath (p);
        g.setColour (col::accent); g.strokePath (p, juce::PathStrokeType (1.0f));
        g.setColour (col::accent2);
        if (fadeIn > 0 && seconds > 0) g.drawLine (a.getX(), a.getBottom(), a.getX() + a.getWidth() * (float) juce::jmin (1.0, fadeIn / seconds), a.getY(), 1.5f);
        if (fadeOut > 0 && seconds > 0) g.drawLine (a.getRight() - a.getWidth() * (float) juce::jmin (1.0, fadeOut / seconds), a.getY(), a.getRight(), a.getBottom(), 1.5f);
        g.setColour (col::dim); g.setFont (juce::FontOptions (11.0f));
        g.drawText (juce::String (seconds, 2) + " s", a.reduced (6), juce::Justification::topRight);
    }
    Studio& s; int channelId; juce::String key;
    std::vector<std::pair<float, float>> peaks; double seconds = 0; float fadeIn = 0, fadeOut = 0;
};

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
        ins.push_back ({ "wave", "Wave", 0, 8, 0, (float) sp.wave, { "Saw", "Square", "Triangle", "Sine", "808", "Supersaw", "FM keys", "Morph", "Air" } });
        ins.push_back ({ "attack", "Attack", 0.001f, 2, 0.002f, sp.attack, {} });
        ins.push_back ({ "decay", "Decay", 0.01f, 4, 0.3f, sp.decay, {} });
        ins.push_back ({ "sustain", "Sustain", 0, 1, 0.7f, sp.sustain, {} });
        ins.push_back ({ "release", "Release", 0.01f, 4, 0.25f, sp.release, {} });
        ins.push_back ({ "cutoff", "Cutoff", 40, 20000, 12000, sp.cutoff, {} });
        ins.push_back ({ "res", "Resonance", 0, 1, 0.2f, sp.resonance, {} });
        ins.push_back ({ "detune", "Detune", 0, 1, 0, sp.detune, {} });
        ins.push_back ({ "unison", "Unison", 1, 7, 1, (float) sp.unison, {} });
        ins.push_back ({ "drive", "Drive", 0, 1, 0, sp.drive, {} });
        ins.push_back ({ "fmRatio", "FM ratio", 0.5f, 8, 1, sp.fmRatio, {} });
        ins.push_back ({ "fmIndex", "FM amount", 0, 8, 0, sp.fmIndex, {} });
        ins.push_back ({ "morph", "Morph", 0, 1, 0, sp.morph, {} });
        ins.push_back ({ "noise", "Air / noise", 0, 1, 0, sp.noise, {} });
        func.push_back ({ "lfoRate", "LFO rate (Hz)", 0, 12, 0, sp.lfoRate, {} });
        func.push_back ({ "vibrato", "Vibrato (st)", 0, 1, 0, sp.vibrato, {} });
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
                else if (id == "fmRatio") sp.fmRatio = v; else if (id == "fmIndex") sp.fmIndex = v; else if (id == "morph") sp.morph = v;
                else if (id == "noise") sp.noise = v; else if (id == "lfoRate") sp.lfoRate = v; else if (id == "vibrato") sp.vibrato = v;
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
    if (sampler) tabs->add ("WAVE", new SampleWaveView (s, channelId));
    tabs->add ("SCOPE", createScopeView ([&s, channelId]() -> ScopeTap* {
        auto* c = s.channel (channelId);
        const int t = c != nullptr ? c->mixerTrack : 0;
        return t > 0 ? s.engine.trackScope (t) : &masterScope(); }, ch->name));
    tabs->finish();
    EditorWindow::windows().add (new EditorWindow (ch->name, tabs, key, [&s, channelId] { return s.channel (channelId) != nullptr; }));
}

void closeAllEditorWindows() { EditorWindow::windows().clear(); }

void openScopeWindow (Studio&)
{
    if (focusExisting ("scope:master")) return;
    auto* w = new EditorWindow ("Scope - Master", createScopeView ([]() -> ScopeTap* { return &masterScope(); }, "Master"),
                                "scope:master", [] { return true; });
    w->setResizable (true, false); w->setResizeLimits (420, 240, 3000, 2000);
    EditorWindow::windows().add (w);
}

void openTrackScopeWindow (Studio& s, int track)
{
    const juce::String key = "scope:track:" + juce::String (track);
    if (focusExisting (key)) return;
    const auto title = track == 0 ? juce::String ("Master") : s.p().mixer.size() > (size_t) track ? s.p().mixer[(size_t) track].name : "Track " + juce::String (track);
    auto* w = new EditorWindow ("Scope - " + title, createScopeView ([&s, track] { return s.engine.trackScope (track); }, title), key, [] { return true; });
    w->setResizable (true, false);
    w->setResizeLimits (420, 240, 3000, 2000);
    EditorWindow::windows().add (w);
}

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
