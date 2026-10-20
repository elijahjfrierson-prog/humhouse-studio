#include "UI.h"
#include "Drummer/DrummerCore.h"

namespace hhs
{

struct PoolInstrument
{
    const char* name; juce::uint32 colour; int wave; float a, d, s, r, cut, res, det; int uni; float drv, fmr, fmi, mor, noi, lfo, vib, glide; bool mono; int root;
};
static const std::vector<PoolInstrument>& instrumentPool()
{
    static const std::vector<PoolInstrument> pool {
        //  name                 colour      wv  a      d     s     r     cut    res   det  uni drv   fmr  fmi  mor  noi   lfo   vib   glide mono   root
        { "Prism (morph pad)",   0xffb06bff, 7, 0.8f,  1.0f, 0.8f, 1.6f, 6500,  0.3f, 0,   1, 0,    1,   0,   0.3f,0,    0.15f,0.04f,0,    false, 60 },
        { "Drift (supersaw)",    0xff5ad1ff, 5, 0.01f, 0.4f, 0.8f, 0.35f,9000,  0.25f,0.5f,1, 0.1f, 1,   0,   0,   0,    5.5f, 0.12f,0.05f,false, 60 },
        { "Lava (squelch bass)", 0xffff6a3d, 0, 0.002f,0.35f,0.5f, 0.12f,900,   0.75f,0.2f,1, 0.45f,1,   0,   0,   0,    0,    0,    0.07f,true,  36 },
        { "Glass (FM bells)",    0xff8affe0, 6, 0.001f,1.6f, 0,    1.2f, 14000, 0.1f, 0,   1, 0,    3.5f,4,   0,   0,    0,    0,    0,    false, 72 },
        { "Haze (lo-fi keys)",   0xffe8c27a, 6, 0.005f,1.2f, 0.35f,0.6f, 4200,  0.15f,0,   1, 0.15f,1,   1.2f,0,   0.04f,0.7f, 0.09f,0,    false, 60 },
        { "Nebula (drone)",      0xff6a7dff, 8, 2.0f,  2.0f, 1.0f, 3.0f, 2800,  0.5f, 0,   1, 0,    1,   0,   0.2f,0.5f, 0.08f,0.03f,0,    false, 48 },
        { "Acoustic Piano",      0xfff2f2f2, 6, 0.001f,2.6f, 0,    0.45f,9000,  0.05f,0,   1, 0,    1,   2.8f,0,   0,    0,    0,    0,    false, 60 },
        { "Electric Piano",      0xffffb35a, 6, 0.001f,2.2f, 0.25f,0.5f, 7000,  0.1f, 0,   1, 0.05f,1,   1.8f,0,   0,    0,    0,    0,    false, 60 },
        { "Rhodes",              0xffff8a5a, 6, 0.002f,2.5f, 0.3f, 0.6f, 5500,  0.1f, 0,   1, 0.1f, 1,   1.3f,0,   0,    5,    0.03f,0,    false, 60 },
        { "Guitar Pluck",        0xffd9a066, 0, 0.001f,0.7f, 0,    0.25f,3200,  0.35f,0.15f,1,0.1f, 1,   0,   0,   0,    0,    0,    0,    false, 60 },
        { "Bass Guitar",         0xff9a6a3d, 2, 0.002f,0.9f, 0.35f,0.15f,1600,  0.3f, 0,   1, 0.25f,1,   0,   0,   0,    0,    0,    0.03f,true,  36 },
        { "Strings",             0xffc9a0ff, 5, 0.45f, 1.0f, 0.9f, 0.9f, 5000,  0.15f,0.35f,1,0,    1,   0,   0,   0,    5,    0.08f,0,    false, 60 },
        { "Brass Stab",          0xffffd04a, 0, 0.03f, 0.35f,0.55f,0.25f,3800,  0.35f,0.3f,2, 0.2f, 1,   0,   0,   0,    0,    0,    0,    false, 60 },
        { "Flute",               0xffa0e8ff, 3, 0.08f, 0.3f, 0.85f,0.25f,6000,  0.05f,0,   1, 0,    1,   0,   0,   0.08f,5,    0.12f,0,    false, 72 },
        { "Choir Pad",           0xffe0d0ff, 7, 0.6f,  1.2f, 0.85f,1.4f, 4200,  0.25f,0,   1, 0,    1,   0,   0.12f,0.03f,0.3f, 0.04f,0,    false, 60 },
    };
    return pool;
}
static Channel poolChannel (const PoolInstrument& e)
{
    Channel c; c.type = ChannelType::Synth; c.mixerTrack = -1;
    c.name = juce::String (e.name).upToFirstOccurrenceOf (" (", false, false);
    c.colour = juce::Colour (e.colour);
    auto& sp = c.synth;
    sp.wave = e.wave; sp.attack = e.a; sp.decay = e.d; sp.sustain = e.s; sp.release = e.r; sp.cutoff = e.cut; sp.resonance = e.res;
    sp.detune = e.det; sp.unison = e.uni; sp.drive = e.drv; sp.fmRatio = e.fmr; sp.fmIndex = e.fmi; sp.morph = e.mor; sp.noise = e.noi;
    sp.lfoRate = e.lfo; sp.vibrato = e.vib;
    c.glide = e.glide; c.mono = e.mono; c.rootKey = e.root;
    return c;
}
static bool isMelodicSample (const juce::File& f)   // notes change the pitch (root key plays the WAV as-is); drum hits stay put
{
    const auto n = f.getFileNameWithoutExtension().toLowerCase(), dir = f.getParentDirectory().getFileName().toLowerCase();
    if (n.contains ("808") || dir.contains ("808") || f.getFullPathName().containsIgnoreCase ("Instruments")) return true;
    return ! isDrumSampleName (n) && ! isDrumSampleName (dir);   // a kit's Hi-Hats / Snares folder keeps every hit at its own pitch
}
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
            c.keyTrack = isMelodicSample (f);
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
    std::function<void()> onDouble;
    void mouseDoubleClick (const juce::MouseEvent&) override { if (onDouble) onDouble(); }
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
    std::function<void (int)> onShuffle;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (onShuffle && w.deltaY != 0) onShuffle (w.deltaY > 0 ? -1 : 1); else Component::mouseWheelMove (e, w);
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (isTypingNoteKey (k)) return false;
        if (onShuffle && k == juce::KeyPress::upKey) { onShuffle (-1); return true; }
        if (onShuffle && k == juce::KeyPress::downKey) { onShuffle (1); return true; }
        return false;
    }
};

// FL-style sample shuffling: step to the previous / next audio file in the same folder
static juce::File siblingSample (const juce::File& f, int dir)
{
    juce::Array<juce::File> files;
    for (const auto& e : juce::RangedDirectoryIterator (f.getParentDirectory(), false, "*", juce::File::findFiles))
        if (isAudioFile (e.getFile())) files.add (e.getFile());
    files.sort();
    const int i = files.indexOf (f);
    if (files.isEmpty()) return f;
    return files[(i < 0 ? 0 : i + dir + files.size()) % files.size()];
}

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
        glide.setSkewFactorFromMidPoint (0.15);
        bind (glide, [] (Channel& c, float v) { c.glide = v; });
        bind (shift, [] (Channel& c, float v) { c.shift = v; });
        insert.setSliderStyle (juce::Slider::LinearBar);
        insert.setTooltip ("Mixer insert this channel plays through (drag to change, 0 = master)");
        insert.onValueChange = [this] { s.edit ("Route channel", [&] (Project&) { if (auto* c = s.channel (channelId)) c->mixerTrack = (int) insert.getValue(); }); };
        name.onMouse = [this] (const juce::MouseEvent& e) { e.mods.isPopupMenu() ? menu() : select (true); };
        setupButton (wrench, "SET", "Channel settings (FL wrench): normalize, reverse, trim, fades, declick, ADSR, cut itself, stretch", false);
        wrench.onClick = [this] { openChannelEditor (s, channelId); };
        addAndMakeVisible (wrench);
        for (juce::Component* c : { (juce::Component*) &mute, (juce::Component*) &vol, (juce::Component*) &pan, (juce::Component*) &pitch,
                                    (juce::Component*) &glide, (juce::Component*) &shift, (juce::Component*) &name, (juce::Component*) &insert })
            addAndMakeVisible (c);
        name.onDouble = [this] { openChannelEditor (s, channelId); };   // FL: wave, amp / INS, sample settings in one window
        name.setTooltip ("Click: open in Piano Roll. Double-click: channel settings (wave, amp, sample). Drop a sample here: replace this channel's sound. Mouse wheel / Up-Down: shuffle samples in this folder. Right-click: rename, delete, editor, routing");
        name.setWantsKeyboardFocus (true);
        name.onShuffle = [this] (int dir) {
            auto* c = s.channel (channelId);
            if (c == nullptr || c->type != ChannelType::Sampler || ! c->sampleFile.existsAsFile()) return;
            const auto next = siblingSample (c->sampleFile, dir);
            if (next == c->sampleFile) return;
            s.edit ("Shuffle sample", [&] (Project&) { if (auto* ch = s.channel (channelId)) { ch->sampleFile = next; ch->name = next.getFileNameWithoutExtension(); } });
            if (! s.engine.isPlaying()) s.engine.previewFile (next);
        };
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
        shift.setValue (c->shift, juce::dontSendNotification);
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
        m.addItem (10, "Change sound...");
        m.addItem (6, "Clear notes in this pattern");
        m.addItem (8, "Cut itself (808s never overlap)", c->type == ChannelType::Sampler, c->cutItself);
        m.addItem (9, "One-shot (play whole sample)", c->type == ChannelType::Sampler, c->oneShot);
        m.addSubMenu ("Color", colourMenu (30));
        m.addSeparator();
        m.addItem (7, "Delete channel");
        m.showMenuAsync ({}, [this] (int r) {
            const int id = channelId;
            if (r == 1) select (true);
            else if (r == 10) changeSoundMenu();
            else if (r == 2) openChannelEditor (s, id);
            else if (r == 3) { auto* c = s.channel (id); if (c) askText ("Rename channel", c->name, [this, id] (juce::String t) { s.edit ("Rename", [&] (Project&) { if (auto* ch = s.channel (id)) ch->name = t; }); }); }
            else if (r == 4) s.edit ("Route channel", [&] (Project&) { if (auto* ch = s.channel (id)) ch->mixerTrack = s.selectedTrack; });
            else if (r == 5) s.edit ("New insert", [&] (Project& p) { if (auto* ch = s.channel (id)) ch->mixerTrack = p.addMixerTrack (ch->name); });
            else if (r == 8) s.edit ("Cut itself", [&] (Project&) { if (auto* ch = s.channel (id)) ch->cutItself = ! ch->cutItself; });
            else if (r == 9) s.edit ("One-shot", [&] (Project&) { if (auto* ch = s.channel (id)) ch->oneShot = ! ch->oneShot; });
            else if (r == 6) s.edit ("Clear notes", [&] (Project&) { s.notes (id).clear(); });
            else if (r >= 30 && r <= 42) { const auto c = pickedColour (r - 30); s.edit ("Channel color", [&] (Project&) { if (auto* ch = s.channel (id)) ch->colour = c; }); }
            else if (r == 7) s.edit ("Delete channel", [&] (Project& p) {
                p.channels.erase (std::remove_if (p.channels.begin(), p.channels.end(), [id] (const Channel& ch) { return ch.id == id; }), p.channels.end());
                for (auto& pt : p.patterns) pt.notes.erase (id);
            });
        });
    }
    void replaceSound (const std::function<void (Channel&)>& make)
    {
        const int id = channelId;
        s.edit ("Change sound", [&] (Project&) {
            auto* ch = s.channel (id);
            if (ch == nullptr) return;
            Channel nc; make (nc);
            nc.id = ch->id; nc.mixerTrack = ch->mixerTrack; nc.volume = ch->volume; nc.pan = ch->pan; nc.muted = ch->muted; nc.solo = ch->solo;
            *ch = nc;
        });
        s.selectedChannel = id;
        s.sendChangeMessage();
    }
    void changeSoundMenu()
    {
        auto* c = s.channel (channelId);
        if (c == nullptr) return;
        juce::PopupMenu m, pool, inst;
        m.addSectionHeader ("Change sound - " + c->name);
        m.addItem (1, "Sample...");
        m.addItem (2, "Synth (keys / lead)");
        m.addItem (3, "808 (glide bass)");
        pool.addSectionHeader ("Psychedelic synths");
        for (int i = 0; i < (int) instrumentPool().size(); ++i)
        {
            if (i == 6) pool.addSectionHeader ("Live instruments");
            pool.addItem (2000 + i, instrumentPool()[(size_t) i].name);
        }
        m.addSubMenu ("Instrument Pool", pool);
        auto types = s.engine.knownPlugins.getTypes();
        for (int i = 0; i < types.size(); ++i) if (types.getReference (i).isInstrument) inst.addItem (1000 + i, types.getReference (i).name + " (" + types.getReference (i).manufacturerName + ")");
        m.addSubMenu ("Plugin instrument", inst, inst.getNumItems() > 0);
        const auto startDir = c->sampleFile.existsAsFile() ? c->sampleFile.getParentDirectory()
                                                           : juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads");
        m.showMenuAsync ({}, [this, types, startDir] (int r) {
            if (r == 1)
            {
                chooser = std::make_unique<juce::FileChooser> ("Change sample", startDir, "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
                    const auto f = fc.getResult();
                    if (! f.existsAsFile()) return;
                    replaceSound ([&] (Channel& n) { n.type = ChannelType::Sampler; n.sampleFile = f; n.keyTrack = isMelodicSample (f); n.name = f.getFileNameWithoutExtension(); });
                    if (! s.engine.isPlaying()) s.engine.previewFile (f);
                });
            }
            else if (r == 2) replaceSound ([] (Channel& n) { n.type = ChannelType::Synth; n.name = "Synth"; n.colour = juce::Colour (0xff8a7dff); });
            else if (r == 3) replaceSound ([] (Channel& n) { n.type = ChannelType::Synth; n.name = "808"; n.synth.wave = 4; n.glide = 0.08f; n.mono = true; n.rootKey = 36; n.colour = juce::Colour (0xffff5d8a); });
            else if (r >= 2000 && r < 2000 + (int) instrumentPool().size())
                replaceSound ([r] (Channel& n) { n = poolChannel (instrumentPool()[(size_t) (r - 2000)]); });
            else if (r >= 1000 && r - 1000 < types.size())
            {
                const auto d = types[r - 1000];
                if (s.dirty) writeBackup (s.p());
                replaceSound ([&d] (Channel& n) { n.type = ChannelType::Plugin; n.name = d.name; n.pluginId = d.createIdentifierString(); n.colour = col::accent2; });
                openChannelEditor (s, channelId);
            }
        });
    }
    std::unique_ptr<juce::FileChooser> chooser;
    void resized() override
    {
        auto r = getLocalBounds().reduced (2);
        mute.setBounds (r.removeFromLeft (16).reduced (0, 6)); r.removeFromLeft (4);
        for (auto* k : { &vol, &pan, &pitch, &glide, &shift }) k->setBounds (r.removeFromLeft (30));
        r.removeFromLeft (4);
        insert.setBounds (r.removeFromRight (34).reduced (0, 4));
        wrench.setBounds (r.removeFromRight (30).reduced (1, 4));
        name.setBounds (r.reduced (2, 1));
    }
    Studio& s; int channelId;
    juce::TextButton mute, wrench;
    Knob vol { "Volume", "channel level", 0.0, 1.25, 0.8 }, pan { "Pan", "left / right", -1.0, 1.0, 0.0 },
         pitch { "Pitch", "transpose in semitones", -24.0, 24.0, 0.0, " st" },
         glide { "Glide", "slide time between notes (808s, melodies); 0 = off", 0.0, 1.0, 0.0, " s" },
         shift { "Shift", "play this channel's hits off the grid: + late (laid back), - early (pushed); 0 = on the grid", -60.0, 60.0, 0.0, " ms" };
    NameTag name;
    juce::Slider insert;
};

class ChannelRack : public juce::Component, public juce::FileDragAndDropTarget, public juce::DragAndDropTarget, public juce::TooltipClient,
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
                s.edit ("New pattern", [] (Project& p) {
                    Pattern pt; pt.name = "Pattern " + juce::String (p.patterns.size() + 1);
                   #if HHS_LICENSED
                    p.patterns.push_back (pt); p.currentPattern = (int) p.patterns.size() - 1;
                   #else
                    p.currentPattern = p.insertPatternAfter (p.currentPattern, pt);   // stacks right after the pattern you were editing
                   #endif
                });
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
        double localPos = -1.0;
        {
            const double plen = pat.lengthBeats(), pos = s.engine.positionBeats();
            if (! p.songMode) localPos = std::fmod (pos, plen);
            else for (auto& c : p.clips)
                if (! c.isAudio() && c.patternIndex == p.currentPattern && pos >= c.start && pos < c.start + c.length) localPos = std::fmod (pos - c.start, plen);
        }
        const int playStep = s.engine.isPlaying() && localPos >= 0 ? (int) (localPos / 0.25) : -1;
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
                if (st == playStep)
                {
                    if (count > 0)   // FL-style: the step lights up as it plays
                    {
                        g.setColour (ch->colour.brighter (0.9f).withAlpha (0.35f)); g.fillRoundedRectangle (r.expanded (2.0f), 4);
                        g.setColour (ch->colour.brighter (0.7f)); g.fillRoundedRectangle (r, 3);
                    }
                    g.setColour (col::text); g.drawRoundedRectangle (r, 3, 1.5f);
                }
            }
        }
        if (localPos >= 0)   // playhead line across the whole rack
        {
            const float x = (float) ctlW + (float) (localPos / 0.25) * w;
            const float y0 = (float) headerH, y1 = (float) juce::jmin (getHeight(), headerH + (int) rows.size() * rowH - scrollY);
            g.setColour (col::accent2.withAlpha (0.22f)); g.fillRect (x - 2.5f, y0, 5.0f, y1 - y0);
            g.setColour (col::accent2); g.fillRect (x - 0.75f, y0, 1.5f, y1 - y0);
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

    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description.toString() == "sample"; }
    void itemDropped (const SourceDetails& d) override   // drag from the Samples browser
    {
        if (auto* t = dynamic_cast<juce::FileTreeComponent*> (d.sourceComponent.get()))
        {
            juce::StringArray files;
            for (int i = 0; i < t->getNumSelectedFiles(); ++i) files.add (t->getSelectedFile (i).getFullPathName());
            if (! files.isEmpty()) filesDropped (files, d.localPosition.x, d.localPosition.y);
        }
    }
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        juce::Array<juce::File> samples;
        for (auto& f : files)
        {
            juce::File file (f);
            if (file.isDirectory()) importKit (s, file);
            else if (isAudioFile (file)) samples.add (file);
        }
        if (samples.size() == 1 && replaceSampleAt ({ x, y }, samples[0])) return;
        if (! samples.isEmpty()) addSampleChannels (s, samples, samples.size() > 1 ? "Samples" : juce::String());
    }
    // FL: a sample dropped onto a channel swaps that channel's sound; its notes, steps, routing and settings stay
    bool replaceSampleAt (juce::Point<int> pt, const juce::File& f)
    {
        if (pt.y < headerH) return false;
        const int row = (pt.y - headerH + scrollY) / rowH;
        if (row < 0 || row >= (int) rows.size()) return false;
        const int id = rows[(size_t) row]->channelId;
        auto* c = s.channel (id);
        if (c == nullptr) return false;
        s.edit ("Replace sample", [&] (Project&) {
            auto* ch = s.channel (id);
            if (ch == nullptr) return;
            if (ch->type != ChannelType::Sampler) { ch->type = ChannelType::Sampler; ch->pluginId = {}; ch->pluginState.reset(); ch->builtinDrum = -1; }
            ch->sampleFile = f;
            ch->keyTrack = isMelodicSample (f);
            ch->name = f.getFileNameWithoutExtension();
        });
        s.selectedChannel = id;
        s.sendChangeMessage();
        if (! s.engine.isPlaying()) s.engine.previewFile (f);
        return true;
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
       #if ! HHS_LICENSED
        m.addItem (4, "Drummer (rock drums, Logic-style)");
       #endif
        juce::PopupMenu pool;
        pool.addSectionHeader ("Psychedelic synths");
        for (int i = 0; i < (int) instrumentPool().size(); ++i)
        {
            if (i == 6) pool.addSectionHeader ("Live instruments");
            pool.addItem (2000 + i, instrumentPool()[(size_t) i].name);
        }
        m.addSubMenu ("Instrument Pool", pool);
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
            else if (r == 4)
                s.edit ("Add Drummer", [&] (Project& p) { s.selectedChannel = addDrummerTrack (p, 0.0, 8); });
            else if (r >= 2000 && r < 2000 + (int) instrumentPool().size())
                s.edit ("Add instrument", [&] (Project& p) { s.selectedChannel = p.addChannel (poolChannel (instrumentPool()[(size_t) (r - 2000)])); });
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
