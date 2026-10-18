#include "UI.h"

namespace hhs
{
namespace
{
struct TP { const char* name; float mn, mx, def, step; std::vector<const char*> choices; const char* help; };
using ApplyFn = std::function<void (NoteList&, const std::vector<float>&, const Project&)>;
struct ToolDef { const char* title; std::vector<TP> params; ApplyFn apply; };

const double rates[] { 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125, 1.0 / 12.0, 0.0625 };
const std::vector<const char*> rateNames { "1/8", "1/8 T", "1/16", "1/16 T", "1/32", "1/32 T", "1/64" };
const std::vector<const char*> offOn { "Off", "On" };
const double grids[] { 0.0625, 0.125, 0.25, 0.5, 1.0, 4.0 };

ToolDef getDef (Tool t)
{
    switch (t)
    {
        case Tool::Arp: return { "Arpeggiator / Roll (Alt+A)", {
            { "Rate", 0, 6, 2, 1, rateNames, "repeat speed - 1/32 and 1/64 for hi-hat and snare rolls" },
            { "Range", 1, 4, 1, 1, {}, "octaves the arp climbs (1 = same note, for rolls)" },
            { "Direction", 0, 3, 0, 1, { "Up", "Down", "Up-Down", "As played" }, "order of chord notes" },
            { "Gate", 0.1f, 1, 0.9f, 0, {}, "note length relative to the rate" },
            { "Vel start", 0, 1, 0.6f, 0, {}, "velocity of the first hit (ramp for rolls)" },
            { "Vel end", 0, 1, 1, 0, {}, "velocity of the last hit" } },
            [] (NoteList& n, const std::vector<float>& v, const Project&) {
                tools::ArpSettings a; a.rate = rates[(int) v[0]]; a.range = (int) v[1]; a.direction = (int) v[2];
                a.gate = v[3]; a.velStart = v[4]; a.velEnd = v[5]; tools::arpeggiate (n, a); } };
        case Tool::Strum: return { "Strum (Alt+S)", {
            { "Time", -0.5f, 0.5f, 0.06f, 0, {}, "offset between chord notes in beats (+ low to high, - high to low)" },
            { "Tension", -1, 1, 0, 0, {}, "accelerate / decelerate the offsets" },
            { "Velocity", -1, 1, 0, 0, {}, "velocity change per strummed note" },
            { "Preserve end", 0, 1, 1, 1, offOn, "keep note ends where they were" },
            { "Alternate", 0, 1, 0, 1, offOn, "flip direction on every other chord" } },
            [] (NoteList& n, const std::vector<float>& v, const Project&) {
                tools::StrumSettings s; s.time = v[0]; s.tension = v[1]; s.velocity = v[2]; s.preserveEnd = v[3] > 0.5f; s.alternate = v[4] > 0.5f;
                tools::strum (n, s); } };
        case Tool::Chop: return { "Chop (Alt+U)", {
            { "Rate", 0, 6, 2, 1, rateNames, "slice length" }, { "Gate", 0.1f, 1, 1, 0, {}, "slice length relative to rate" } },
            [] (NoteList& n, const std::vector<float>& v, const Project&) { tools::ChopSettings c; c.rate = rates[(int) v[0]]; c.gate = v[1]; tools::chop (n, c); } };
        case Tool::Flam: return { "Flam (Alt+F)", {
            { "Time", 0.005f, 0.25f, 0.03f, 0, {}, "grace note distance in beats" }, { "Velocity", 0, 1, 0.6f, 0, {}, "grace note velocity" } },
            [] (NoteList& n, const std::vector<float>& v, const Project&) { tools::FlamSettings f; f.time = v[0]; f.velocity = v[1]; tools::flam (n, f); } };
        case Tool::Quantize: return { "Quantize (Alt+Q)", {
            { "Grid", 0, 5, 2, 1, { "1/4 step", "1/2 step", "Step", "1/2 beat", "Beat", "Bar" }, "grid to snap to" },
            { "Strength", 0, 1, 1, 0, {}, "how far notes move toward the grid" },
            { "Starts", 0, 1, 1, 1, offOn, "quantize note starts" }, { "Ends", 0, 1, 0, 1, offOn, "quantize note ends" } },
            [] (NoteList& n, const std::vector<float>& v, const Project&) {
                tools::QuantizeSettings q; q.grid = grids[(int) v[0]]; q.strength = v[1]; q.starts = v[2] > 0.5f; q.ends = v[3] > 0.5f; tools::quantize (n, q); } };
        case Tool::Randomize: return { "Randomize - in key (Alt+R)", {
            { "Octave", 1, 8, 5, 1, {}, "lowest octave" }, { "Octaves", 1, 4, 1, 1, {}, "octave range" },
            { "Region", 1, 16, 4, 1, {}, "beats to fill" }, { "Length", 0.0625f, 2, 0.25f, 0, {}, "note length in beats" },
            { "Population", 0, 1, 0.5f, 0, {}, "how many notes" }, { "Vel min", 0, 1, 0.5f, 0, {}, "lowest velocity" },
            { "Vel max", 0, 1, 1, 0, {}, "highest velocity" }, { "Seed", 1, 999, 1, 1, {}, "change for a new idea" } },
            [] (NoteList& n, const std::vector<float>& v, const Project& p) {
                tools::RandomSettings r; r.octave = (int) v[0]; r.octaves = (int) v[1]; r.region = v[2]; r.length = v[3]; r.population = v[4];
                r.velMin = v[5]; r.velMax = v[6]; r.seed = (int) v[7]; tools::randomize (n, r, p.keyRoot, p.scaleIndex, 0.0); } };
        case Tool::Limit: return { "Limit - fold into key (Alt+K)", {
            { "Low key", 0, 127, 36, 1, {}, "lowest allowed note" }, { "High key", 0, 127, 96, 1, {}, "highest allowed note" },
            { "To scale", 0, 1, 1, 1, offOn, "snap notes into the project key / scale" } },
            [] (NoteList& n, const std::vector<float>& v, const Project& p) {
                tools::LimitSettings l; l.lowKey = (int) v[0]; l.highKey = (int) v[1]; l.toScale = v[2] > 0.5f; tools::limit (n, l, p.keyRoot, p.scaleIndex); } };
        case Tool::Articulate: return { "Articulate (Alt+L)", { { "Length", 0.1f, 2, 1, 0, {}, "multiply note lengths (staccato / legato)" } },
            [] (NoteList& n, const std::vector<float>& v, const Project&) { tools::articulate (n, v[0]); } };
        case Tool::ScaleLevels: return { "Scale levels - velocity (Alt+X)", {
            { "Multiply", 0, 2, 1, 0, {}, "scale velocities" }, { "Offset", -1, 1, 0, 0, {}, "add to velocities" } },
            [] (NoteList& n, const std::vector<float>& v, const Project&) { tools::ScaleLevelSettings s; s.multiply = v[0]; s.offset = v[1]; tools::scaleLevels (n, s); } };
        case Tool::Flip: return { "Flip (Alt+Y)", { { "Direction", 0, 2, 0, 1, { "Horizontal (reverse)", "Vertical (invert)", "Both" }, "how to flip" } },
            [] (NoteList& n, const std::vector<float>& v, const Project&) { const int d = (int) v[0]; tools::flip (n, d != 1, d != 0); } };
        case Tool::Claw: return { "Claw machine (Alt+W)", {
            { "Period", 1, 8, 2, 1, {}, "group size" }, { "Trash every", 1, 8, 2, 1, {}, "remove every Nth note" },
            { "Time distort", -1, 1, 0, 0, {}, "shift kept notes in time" } },
            [] (NoteList& n, const std::vector<float>& v, const Project&) { tools::ClawSettings c; c.period = (int) v[0]; c.trashEvery = (int) v[1]; c.timeDistort = v[2]; tools::claw (n, c); } };
        case Tool::Lfo: return { "LFO - velocity (Alt+O)", {
            { "Shape", 0, 3, 0, 1, { "Sine", "Triangle", "Square", "Saw" }, "wave shape" }, { "Period", 0.25f, 8, 1, 0, {}, "cycle length in beats" },
            { "Min", 0, 1, 0.3f, 0, {}, "lowest velocity" }, { "Max", 0, 1, 1, 0, {}, "highest velocity" }, { "Phase", 0, 1, 0, 0, {}, "start position" } },
            [] (NoteList& n, const std::vector<float>& v, const Project&) {
                tools::LfoSettings l; l.shape = (int) v[0]; l.period = v[1]; l.minV = v[2]; l.maxV = v[3]; l.phase = v[4]; tools::lfo (n, l); } };
    }
    return {};
}

std::map<int, std::vector<float>>& lastValues() { static std::map<int, std::vector<float>> m; return m; }

class ToolPanel : public juce::Component
{
public:
    ToolPanel (Studio& st, Tool t, int ch) : s (st), tool (t), def (getDef (t)), channel (ch)
    {
        original = s.notes (ch);
        auto& last = lastValues()[(int) t];
        if (last.size() != def.params.size()) { last.clear(); for (auto& p : def.params) last.push_back (p.def); }
        for (size_t i = 0; i < def.params.size(); ++i)
        {
            auto& tp = def.params[i];
            auto* l = labels.add (new juce::Label ({}, tp.name));
            l->setJustificationType (juce::Justification::centred);
            l->setFont (juce::FontOptions (12.0f));
            addAndMakeVisible (l);
            if (! tp.choices.empty())
            {
                auto* cb = new juce::ComboBox();
                for (size_t c = 0; c < tp.choices.size(); ++c) cb->addItem (tp.choices[c], (int) c + 1);
                cb->setSelectedItemIndex ((int) last[i], juce::dontSendNotification);
                cb->setTooltip (juce::String (tp.name) + " - " + tp.help);
                cb->onChange = [this] { preview(); };
                controls.add (cb);
            }
            else
            {
                auto* k = new Knob (tp.name, tp.help, tp.mn, tp.mx, tp.def);
                if (tp.step > 0) k->setRange (tp.mn, tp.mx, tp.step);
                k->setValue (last[i], juce::dontSendNotification);
                k->onValueChange = [this] { preview(); };
                controls.add (k);
            }
            addAndMakeVisible (controls.getLast());
        }
        setupButton (ok, "Accept", "Keep the result (Enter)");
        setupButton (cancel, "Cancel", "Undo this tool (Esc)");
        setupButton (reset, "Reset", "Restore default settings");
        ok.onClick = [this] { finish (true); };
        cancel.onClick = [this] { finish (false); };
        reset.onClick = [this] {
            for (size_t i = 0; i < def.params.size(); ++i)
            {
                if (auto* k = dynamic_cast<juce::Slider*> (controls[(int) i])) k->setValue (def.params[i].def, juce::dontSendNotification);
                if (auto* c = dynamic_cast<juce::ComboBox*> (controls[(int) i])) c->setSelectedItemIndex ((int) def.params[i].def, juce::dontSendNotification);
            }
            preview();
        };
        for (auto* b : { &ok, &cancel, &reset }) addAndMakeVisible (b);
        const int cols = juce::jmin (4, (int) def.params.size());
        const int rowsN = ((int) def.params.size() + cols - 1) / cols;
        setSize (juce::jmax (330, cols * 92 + 20), rowsN * 96 + 60);
        setWantsKeyboardFocus (true);
        preview();
    }
    ~ToolPanel() override { if (! done) finish (true); }
    juce::String title() const { return def.title; }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto bottom = r.removeFromBottom (30);
        ok.setBounds (bottom.removeFromRight (90)); bottom.removeFromRight (6);
        cancel.setBounds (bottom.removeFromRight (90));
        reset.setBounds (bottom.removeFromLeft (80));
        const int cols = juce::jmin (4, (int) def.params.size());
        for (int i = 0; i < controls.size(); ++i)
        {
            juce::Rectangle<int> cell (r.getX() + (i % cols) * 92, r.getY() + (i / cols) * 96, 88, 90);
            labels[i]->setBounds (cell.removeFromTop (18));
            if (dynamic_cast<juce::ComboBox*> (controls[i]) != nullptr) controls[i]->setBounds (cell.withSizeKeepingCentre (86, 26));
            else controls[i]->setBounds (cell.reduced (10, 2));
        }
    }
    void paint (juce::Graphics& g) override { g.fillAll (col::panel); }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::returnKey) { finish (true); return true; }
        if (k == juce::KeyPress::escapeKey) { finish (false); return true; }
        return false;
    }

private:
    Studio& s; Tool tool; ToolDef def; int channel; NoteList original; bool done = false;
    juce::OwnedArray<juce::Label> labels;
    juce::OwnedArray<juce::Component> controls;
    juce::TextButton ok, cancel, reset;

    std::vector<float> values() const
    {
        std::vector<float> v;
        for (auto* c : controls)
        {
            if (auto* k = dynamic_cast<juce::Slider*> (c)) v.push_back ((float) k->getValue());
            else if (auto* cb = dynamic_cast<juce::ComboBox*> (c)) v.push_back ((float) juce::jmax (0, cb->getSelectedItemIndex()));
        }
        return v;
    }
    void preview()
    {
        const auto v = values();
        s.live ([&] (Project& p) { auto& n = s.notes (channel); n = original; def.apply (n, v, p); tools::sortNotes (n); });
        s.sendChangeMessage();
    }
    void finish (bool accept)
    {
        if (done) return;
        done = true;
        if (accept) { lastValues()[(int) tool] = values(); s.commit (def.title); }
        else { s.live ([&] (Project&) { s.notes (channel) = original; }); s.sendChangeMessage(); }
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0);
    }
};
}

void showToolDialog (Studio& s, Tool t, int channelId)
{
    if (s.channel (channelId) == nullptr) return;
    auto* panel = new ToolPanel (s, t, channelId);
    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = panel->title();
    o.content.setOwned (panel);
    o.dialogBackgroundColour = col::panel;
    o.escapeKeyTriggersCloseButton = false;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}
}
