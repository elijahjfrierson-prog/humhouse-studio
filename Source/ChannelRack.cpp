#include "UI.h"

namespace hhs
{
static bool isAudioFile (const juce::File& f) { return f.hasFileExtension ("wav;aif;aiff;flac;mp3;ogg"); }

void addSampleChannels (Studio& s, const juce::Array<juce::File>& files, const juce::String& insertName)
{
    s.edit ("Add samples", [&] (Project& p) {
        int insert = -1;
        if (insertName.isNotEmpty())
        {
            for (int i = 1; i < (int) p.mixer.size(); ++i) if (p.mixer[(size_t) i].name == insertName) insert = i;
            if (insert < 0) insert = p.addMixerTrack (insertName);
        }
        for (auto& f : files)
        {
            Channel c;
            c.name = f.getFileNameWithoutExtension();
            c.type = ChannelType::Sampler;
            c.sampleFile = f;
            c.mixerTrack = insert;
            c.colour = juce::Colour::fromHSV (juce::Random::getSystemRandom().nextFloat(), 0.55f, 0.95f, 1.0f);
            s.selectedChannel = p.addChannel (c);
        }
    });
}

void importKit (Studio& s, const juce::File& folder)
{
    juce::Array<juce::File> files;
    for (const auto& e : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
        if (isAudioFile (e.getFile())) { files.add (e.getFile()); if (files.size() >= 128) break; }
    files.sort();
    if (! files.isEmpty()) addSampleChannels (s, files, folder.getFileName());
}

void addPluginChannel (Studio& s, const juce::PluginDescription& d)
{
    if (s.dirty) writeBackup (s.p());
    s.edit ("Add " + d.name, [&] (Project& p) {
        Channel c; c.name = d.name; c.type = ChannelType::Plugin; c.pluginId = d.createIdentifierString(); c.mixerTrack = -1;
        c.colour = col::accent2;
        s.selectedChannel = p.addChannel (c);
    });
    openChannelEditor (s, s.selectedChannel);
}

namespace
{
constexpr int rowH = 34, headerH = 34, ctlW = 352;

class ChannelRack;

struct NameTag : public juce::Component, public juce::SettableTooltipClient
{
    std::function<void (const juce::MouseEvent&)> onMouse;
    juce::String text; juce::Colour colour; bool selected = false;
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1);
        g.setColour (selected ? colour.withAlpha (0.9f) : col::panel2);
        g.fillRoundedRectangle (r, 4);
        g.setColour (colour); g.fillRect (r.removeFromLeft (4));
        g.setColour (selected ? juce::Colours::black : col::text);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawFittedText (text, getLocalBounds().reduced (8, 0), juce::Justification::centredLeft, 1);
    }
    void mouseUp (const juce::MouseEvent& e) override { if (onMouse && contains (e.getPosition())) onMouse (e); }
};

struct Row : public juce::Component
{
    Row (Studio& st, int id) : s (st), channelId (id)
    {
        setupButton (mute, {}, "Channel on/off (green = on)", true);
        mute.setColour (juce::TextButton::buttonOnColourId, col::good);
        mute.onClick = [this] { const bool on = mute.getToggleState(); s.edit ("Mute channel", [&] (Project&) { if (auto* c = s.channel (channelId)) c->muted = ! on; }); };
        bind (vol, [] (Channel& c, float v) { c.volume = v; });
        bind (pan, [] (Channel& c, float v) { c.pan = v; });
        bind (pitch, [] (Channel& c, float v) { c.pitch = v; });
        bind (glide, [] (Channel& c, float v) { c.glide = v; });
        insert.setSliderStyle (juce::Slider::LinearBar);
        insert.setTooltip ("Mixer insert this channel plays through (drag to change, 0 = master)");
        insert.onValueChange = [this] { s.edit ("Route channel", [&] (Project&) { if (auto* c = s.channel (channelId)) c->mixerTrack = (int) insert.getValue(); }); };
        name.onMouse = [this] (const juce::MouseEvent& e) { e.mods.isPopupMenu() ? menu() : select (true); };
        for (juce::Component* c : { (juce::Component*) &mute, (juce::Component*) &vol, (juce::Component*) &pan, (juce::Component*) &pitch,
                                    (juce::Component*) &glide, (juce::Component*) &name, (juce::Component*) &insert })
            addAndMakeVisible (c);
        name.setTooltip ("Click: open in Piano Roll. Right-click: rename, delete, editor, routing");
        refresh();
    }
    void bind (Knob& k, std::function<void (Channel&, float)> set)
    {
        k.onValueChange = [this, &k, set] {
            s.live ([&] (Project&) { if (auto* c = s.channel (channelId)) set (*c, (float) k.getValue()); });
            if (! k.isMouseButtonDown()) s.commit (k.getName());
        };
        k.onDragEnd = [this, &k] { s.commit (k.getName()); };
    }
    void refresh()
    {
        auto* c = s.channel (channelId);
        if (c == nullptr) return;
        mute.setToggleState (! c->muted, juce::dontSendNotification);
        vol.setValue (c->volume, juce::dontSendNotification); pan.setValue (c->pan, juce::dontSendNotification);
        pitch.setValue (c->pitch, juce::dontSendNotification); glide.setValue (c->glide, juce::dontSendNotification);
        insert.setRange (0, juce::jmax (1, (int) s.p().mixer.size() - 1), 1);
        insert.setValue (c->mixerTrack, juce::dontSendNotification);
        name.text = c->name; name.colour = c->colour; name.selected = s.selectedChannel == channelId;
        name.repaint();
    }
    void select (bool openRoll)
    {
        s.selectedChannel = channelId;
        if (auto* c = s.channel (channelId)) if (c->mixerTrack > 0) s.selectedTrack = c->mixerTrack;
        s.sendChangeMessage();
        if (openRoll && s.showView) s.showView (0);
    }
    void menu()
    {
        juce::PopupMenu m;
        auto* c = s.channel (channelId);
        if (c == nullptr) return;
        m.addItem (1, "Open in Piano Roll");
        m.addItem (2, c->type == ChannelType::Plugin ? "Open plugin editor" : "Channel settings (synth / sampler)...");
        m.addItem (3, "Rename...");
        m.addItem (4, "Route to selected mixer insert (" + juce::String (s.selectedTrack) + ")");
        m.addItem (5, "New mixer insert for this channel");
        m.addItem (6, "Clear notes in this pattern");
        m.addItem (8, "Cut itself (808s never overlap)", c->type == ChannelType::Sampler, c->cutItself);
        m.addItem (9, "One-shot (play whole sample)", c->type == ChannelType::Sampler, c->oneShot);
        m.addSeparator();
        m.addItem (7, "Delete channel");
        m.showMenuAsync ({}, [this] (int r) {
            const int id = channelId;
            if (r == 1) select (true);
            else if (r == 2) openChannelEditor (s, id);
            else if (r == 3) { auto* c = s.channel (id); if (c) askText ("Rename channel", c->name, [this, id] (juce::String t) { s.edit ("Rename", [&] (Project&) { if (auto* ch = s.channel (id)) ch->name = t; }); }); }
            else if (r == 4) s.edit ("Route channel", [&] (Project&) { if (auto* ch = s.channel (id)) ch->mixerTrack = s.selectedTrack; });
            else if (r == 5) s.edit ("New insert", [&] (Project& p) { if (auto* ch = s.channel (id)) ch->mixerTrack = p.addMixerTrack (ch->name); });
            else if (r == 8) s.edit ("Cut itself", [&] (Project&) { if (auto* ch = s.channel (id)) ch->cutItself = ! ch->cutItself; });
            else if (r == 9) s.edit ("One-shot", [&] (Project&) { if (auto* ch = s.channel (id)) ch->oneShot = ! ch->oneShot; });
            else if (r == 6) s.edit ("Clear notes", [&] (Project&) { s.notes (id).clear(); });
            else if (r == 7) s.edit ("Delete channel", [&] (Project& p) {
                p.channels.erase (std::remove_if (p.channels.begin(), p.channels.end(), [id] (const Channel& ch) { return ch.id == id; }), p.channels.end());
                for (auto& pt : p.patterns) pt.notes.erase (id);
            });
        });
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (2);
        mute.setBounds (r.removeFromLeft (16).reduced (0, 6)); r.removeFromLeft (4);
        for (auto* k : { &vol, &pan, &pitch, &glide }) k->setBounds (r.removeFromLeft (30));
        r.removeFromLeft (4);
        insert.setBounds (r.removeFromRight (34).reduced (0, 4));
        name.setBounds (r.reduced (2, 1));
    }
    Studio& s; int channelId;
    juce::TextButton mute;
    Knob vol { "Volume", "channel level", 0.0, 1.25, 0.8 }, pan { "Pan", "left / right", -1.0, 1.0, 0.0 },
         pitch { "Pitch", "transpose in semitones", -24.0, 24.0, 0.0, " st" },
         glide { "Glide", "slide time between notes (808s, melodies); 0 = off", 0.0, 1.0, 0.0, " s" };
    NameTag name;
    juce::Slider insert;
};

class ChannelRack : public juce::Component, public juce::FileDragAndDropTarget, public juce::TooltipClient,
                    private juce::ChangeListener, private juce::Timer
{
public:
    explicit ChannelRack (Studio& st) : s (st)
    {
        s.addChangeListener (this);
        setupButton (addBtn, "+ Channel", "Add a sampler, synth, 808 or plugin instrument");
        addBtn.onClick = [this] { addMenu(); };
        setupButton (kitBtn, "Import Kit", "Pick a drum kit folder: every sample becomes a channel (or drag folders / samples onto the rack)");
        kitBtn.onClick = [this] {
            chooser = std::make_unique<juce::FileChooser> ("Choose a drum kit folder", juce::File::getSpecialLocation (juce::File::userMusicDirectory));
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [this] (const juce::FileChooser& fc) { if (fc.getResult().isDirectory()) importKit (s, fc.getResult()); });
        };
        patternBox.setTooltip ("Pattern being edited (pick 'New pattern' to add one)");
        patternBox.onChange = [this] {
            const int i = patternBox.getSelectedItemIndex();
            if (i == (int) s.p().patterns.size())
                s.edit ("New pattern", [] (Project& p) { Pattern pt; pt.name = "Pattern " + juce::String (p.patterns.size() + 1); p.patterns.push_back (pt); p.currentPattern = (int) p.patterns.size() - 1; });
            else if (i >= 0 && i != s.p().currentPattern)
                s.edit ("Select pattern", [i] (Project& p) { p.currentPattern = i; });
        };
        swing.onValueChange = [this] { s.live ([this] (Project& p) { p.swing = swing.getValue(); }); if (! swing.isMouseButtonDown()) s.commit ("Swing"); };
        swing.onDragEnd = [this] { s.commit ("Swing"); };
        for (juce::Component* c : { (juce::Component*) &addBtn, (juce::Component*) &kitBtn, (juce::Component*) &patternBox, (juce::Component*) &swing })
            addAndMakeVisible (c);
        refresh();
        startTimerHz (30);
    }
    ~ChannelRack() override { s.removeChangeListener (this); }

    juce::String getTooltip() override
    {
        if (lastMouse.x >= ctlW) return "Steps: click on/off, drag to paint, right-click = snare / hi-hat roll (x2 - x8). Drop kits or samples here";
        return {};
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::panel);
        g.setColour (col::bg); g.fillRect (0, 0, getWidth(), headerH);
        g.setColour (col::dim); g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText ("CHANNEL RACK", getWidth() - 140, 0, 130, headerH, juce::Justification::centredRight);
        auto& p = s.p();
        if (p.patterns.empty()) return;
        auto& pat = p.patterns[(size_t) juce::jlimit (0, (int) p.patterns.size() - 1, p.currentPattern)];
        const int steps = numSteps();
        const float w = stepW();
        int playStep = -1;
        if (s.engine.isPlaying()) playStep = (int) (std::fmod (s.engine.positionBeats(), pat.lengthBeats()) / 0.25);
        for (size_t ri = 0; ri < rows.size(); ++ri)
        {
            const int y = headerH + (int) ri * rowH - scrollY;
            if (y + rowH < headerH || y > getHeight()) continue;
            auto* ch = s.channel (rows[ri]->channelId);
            if (ch == nullptr) continue;
            auto it = pat.notes.find (ch->id);
            for (int st = 0; st < steps; ++st)
            {
                juce::Rectangle<float> r ((float) ctlW + st * w + 1, (float) y + 4, w - 2, (float) rowH - 8);
                const bool group = (st / 4) % 2 == 0;
                int count = 0;
                if (it != pat.notes.end())
                    for (auto& n : it->second) if (n.start >= st * 0.25 - 1e-6 && n.start < (st + 1) * 0.25 - 1e-6) ++count;
                g.setColour (count > 0 ? ch->colour : (group ? col::panel2.brighter (0.06f) : col::grid));
                g.fillRoundedRectangle (r, 3);
                if (count > 1)
                {
                    g.setColour (juce::Colours::black.withAlpha (0.6f));
                    for (int k = 1; k < count; ++k) g.fillRect (r.getX() + r.getWidth() * k / count, r.getY() + 3, 1.5f, r.getHeight() - 6);
                }
                if (st == playStep) { g.setColour (col::text); g.drawRoundedRectangle (r, 3, 1.5f); }
            }
        }
    }

    void resized() override
    {
        auto top = getLocalBounds().removeFromTop (headerH).reduced (6, 5);
        addBtn.setBounds (top.removeFromLeft (90)); top.removeFromLeft (4);
        kitBtn.setBounds (top.removeFromLeft (90)); top.removeFromLeft (8);
        patternBox.setBounds (top.removeFromLeft (150)); top.removeFromLeft (8);
        swing.setBounds (top.removeFromLeft (28));
        layoutRows();
    }

    void mouseMove (const juce::MouseEvent& e) override { lastMouse = e.getPosition(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        int row, step;
        if (! hitStep (e.getPosition(), row, step)) return;
        const int id = rows[(size_t) row]->channelId;
        if (e.mods.isPopupMenu()) { rollMenu (id, step); return; }
        paintOn = ! stepOn (id, step);
        setStep (id, step, paintOn);
        lastStep = { row, step };
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        int row, step;
        if (e.mods.isPopupMenu() || ! hitStep (e.getPosition(), row, step) || juce::Point<int> (row, step) == lastStep) return;
        lastStep = { row, step };
        setStep (rows[(size_t) row]->channelId, step, paintOn);
    }
    void mouseUp (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu() && e.getMouseDownX() >= ctlW) s.commit ("Steps"); lastStep = { -1, -1 }; }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        scrollY = juce::jlimit (0, juce::jmax (0, (int) rows.size() * rowH - (getHeight() - headerH)), scrollY - (int) (w.deltaY * 120));
        layoutRows(); repaint();
    }

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override
    {
        juce::Array<juce::File> samples;
        for (auto& f : files)
        {
            juce::File file (f);
            if (file.isDirectory()) importKit (s, file);
            else if (isAudioFile (file)) samples.add (file);
        }
        if (! samples.isEmpty()) addSampleChannels (s, samples, samples.size() > 1 ? "Samples" : juce::String());
    }

private:
    Studio& s;
    std::vector<std::unique_ptr<Row>> rows;
    juce::TextButton addBtn, kitBtn;
    juce::ComboBox patternBox;
    Knob swing { "Swing", "delays every other step for groove", 0.0, 1.0, 0.0 };
    std::unique_ptr<juce::FileChooser> chooser;
    int scrollY = 0;
    bool paintOn = true;
    juce::Point<int> lastStep { -1, -1 }, lastMouse;

    int numSteps() const
    {
        auto& p = const_cast<Studio&> (s).p();
        if (p.patterns.empty()) return 16;
        return juce::jmax (16, (int) std::round (p.patterns[(size_t) juce::jlimit (0, (int) p.patterns.size() - 1, p.currentPattern)].lengthBeats() * 4));
    }
    float stepW() const { return juce::jlimit (8.0f, 30.0f, (float) (getWidth() - ctlW - 8) / (float) numSteps()); }
    bool hitStep (juce::Point<int> pt, int& row, int& step) const
    {
        if (pt.x < ctlW || pt.y < headerH) return false;
        row = (pt.y - headerH + scrollY) / rowH;
        step = (int) ((pt.x - ctlW) / stepW());
        return row >= 0 && row < (int) rows.size() && step >= 0 && step < numSteps();
    }
    bool stepOn (int id, int step) { auto& p = s.p(); return p.patterns[(size_t) p.currentPattern].stepOn (id, step); }
    void setStep (int id, int step, bool on)
    {
        if (stepOn (id, step) == on) return;
        auto* ch = s.channel (id);
        if (ch == nullptr) return;
        s.live ([&] (Project& p) { p.patterns[(size_t) p.currentPattern].toggleStep (id, step, ch->rootKey); });
        if (on) s.engine.previewNote (id, ch->rootKey, 0.8f, true);
        repaint();
    }
    void rollMenu (int id, int step)
    {
        juce::PopupMenu m;
        for (int d : { 2, 3, 4, 6, 8 }) m.addItem (d, "Roll x" + juce::String (d) + (d == 3 || d == 6 ? " (triplet)" : ""));
        m.addItem (100, "Clear step");
        m.showMenuAsync ({}, [this, id, step] (int r) {
            if (r == 0) return;
            auto* ch = s.channel (id);
            if (ch == nullptr) return;
            const int key = ch->rootKey;
            s.edit (r == 100 ? "Clear step" : "Roll", [&] (Project&) {
                auto& n = s.notes (id);
                const double st = step * 0.25;
                n.erase (std::remove_if (n.begin(), n.end(), [st] (const Note& x) { return x.start >= st - 1e-6 && x.start < st + 0.25 - 1e-6; }), n.end());
                if (r != 100) tools::rollStep (n, st, 0.25, r, key, 0.8f);
            });
        });
    }
    void addMenu()
    {
        juce::PopupMenu m, inst;
        m.addItem (1, "Sampler (pick a sample)...");
        m.addItem (2, "Synth (keys / lead)");
        m.addItem (3, "808 (glide bass)");
        auto types = s.engine.knownPlugins.getTypes();
        for (int i = 0; i < types.size(); ++i) if (types.getReference (i).isInstrument) inst.addItem (1000 + i, types.getReference (i).name + " (" + types.getReference (i).manufacturerName + ")");
        m.addSubMenu ("Plugin instrument", inst, inst.getNumItems() > 0);
        m.showMenuAsync ({}, [this, types] (int r) {
            if (r == 1)
            {
                chooser = std::make_unique<juce::FileChooser> ("Choose samples", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                                      [this] (const juce::FileChooser& fc) { if (! fc.getResults().isEmpty()) addSampleChannels (s, fc.getResults(), {}); });
            }
            else if (r == 2 || r == 3)
                s.edit ("Add synth", [&] (Project& p) {
                    Channel c; c.type = ChannelType::Synth; c.mixerTrack = -1;
                    if (r == 3)
                    {
                        for (auto& e : p.channels) if (e.name.containsIgnoreCase ("808")) { c = e; c.id = 0; c.mixerTrack = -1; }
                        c.name = "808"; c.glide = juce::jmax (0.08f, c.glide); c.mono = true; c.rootKey = 36; c.type = ChannelType::Synth;
                    }
                    else c.name = "Synth";
                    c.colour = r == 3 ? juce::Colour (0xffff5d8a) : juce::Colour (0xff8a7dff);
                    s.selectedChannel = p.addChannel (c);
                });
            else if (r >= 1000) addPluginChannel (s, types.getReference (r - 1000));
        });
    }
    void refresh()
    {
        auto& p = s.p();
        bool same = rows.size() == p.channels.size();
        for (size_t i = 0; same && i < rows.size(); ++i) same = rows[i]->channelId == p.channels[i].id;
        if (! same)
        {
            rows.clear();
            for (auto& c : p.channels) { rows.push_back (std::make_unique<Row> (s, c.id)); addAndMakeVisible (*rows.back()); }
            layoutRows();
        }
        for (auto& r : rows) r->refresh();
        patternBox.clear (juce::dontSendNotification);
        for (size_t i = 0; i < p.patterns.size(); ++i) patternBox.addItem (p.patterns[i].name, (int) i + 1);
        patternBox.addItem ("+ New pattern", (int) p.patterns.size() + 1);
        patternBox.setSelectedItemIndex (p.currentPattern, juce::dontSendNotification);
        swing.setValue (p.swing, juce::dontSendNotification);
        if (s.selectedChannel < 0 && ! p.channels.empty()) s.selectedChannel = p.channels.front().id;
        repaint();
    }
    void layoutRows()
    {
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const int y = headerH + (int) i * rowH - scrollY;
            rows[i]->setBounds (0, y, ctlW - 6, rowH);
            rows[i]->setVisible (y + rowH > headerH);
        }
    }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override { if (s.engine.isPlaying()) repaint (ctlW, headerH, getWidth() - ctlW, getHeight() - headerH); }
};
}

std::unique_ptr<juce::Component> createChannelRack (Studio& s) { return std::make_unique<ChannelRack> (s); }
}
