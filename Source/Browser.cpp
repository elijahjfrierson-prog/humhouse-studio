#include "UI.h"
#include <set>
#include <map>

namespace hhs
{
static bool isHumHouse (const juce::PluginDescription& d)
{
    return d.manufacturerName.containsIgnoreCase ("HumHouse") || d.manufacturerName.containsIgnoreCase ("Hum House")
        || d.name.containsIgnoreCase ("HumHouse") || d.name.containsIgnoreCase ("Hum House");
}

void addEffect (Studio& s, int track, const juce::String& type, const juce::String& pluginId, int replaceSlotUid)
{
    if (track < 0 || track >= (int) s.p().mixer.size()) return;
    if (type == "plugin" && s.dirty) writeBackup (s.p());   // safety net before loading third-party code
    int uid = -1;
    s.edit ("Add effect", [&] (Project& p) {
        auto& fx = p.mixer[(size_t) track].fx;
        for (auto& f : fx)
            if (f.uid == replaceSlotUid) { f.type = type; f.pluginId = pluginId; f.params.clear(); f.pluginState.reset(); uid = f.uid; }
        if (uid < 0) { auto& slot = p.addFx (track, type); slot.pluginId = pluginId; uid = slot.uid; }
    });
    s.selectedTrack = track;
    openEffectEditor (s, track, uid);
}

void showPluginScanner (Studio& s)
{
    auto pedal = s.settings->getFile().getSiblingFile ("PluginScanCrash.txt");
    auto* c = new juce::PluginListComponent (s.engine.formats, s.engine.knownPlugins, pedal, s.settings, true);
    c->setNumberOfThreadsForScanning (2);   // each plugin is probed in its own child process, so this is crash-safe
    c->setSize (780, 540);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (c);
    o.dialogTitle = "Plugin House - scan for VST3 / AU plugins (Options > Scan)";
    o.dialogBackgroundColour = col::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void showPluginHouseMenu (Studio& s, int track, int replaceSlotUid)
{
    juce::PopupMenu m, builtins, hum;
    std::map<juce::String, juce::PopupMenu> cats, makers;
    const auto& fx = builtinEffects();
    for (size_t i = 0; i < fx.size(); ++i) cats[fx[i].category].addItem ((int) i + 1, fx[i].name);
    for (auto& [c, sub] : cats) builtins.addSubMenu (c, sub);
    auto types = s.engine.knownPlugins.getTypes();
    for (int i = 0; i < types.size(); ++i)
    {
        auto& d = types.getReference (i);
        if (d.isInstrument) continue;
        const auto label = d.name + "  (" + d.pluginFormatName + ")";
        if (isHumHouse (d)) hum.addItem (1000 + i, label);
        else makers[d.manufacturerName.isEmpty() ? juce::String ("Other") : d.manufacturerName].addItem (1000 + i, label);
    }
    m.addSectionHeader ("PLUGIN HOUSE");
    m.addSubMenu ("HumHouse built-ins", builtins);
    if (hum.getNumItems() > 0) m.addSubMenu ("HumHouse plugins", hum);
    juce::PopupMenu third;
    for (auto& [maker, sub] : makers) third.addSubMenu (maker, sub);
    m.addSubMenu ("Third-party plugins", third, third.getNumItems() > 0);
    m.addSeparator();
    m.addItem (9999, "Scan / manage plugins...");
    m.showMenuAsync ({}, [&s, track, replaceSlotUid, types] (int r) {
        const auto& all = builtinEffects();
        if (r >= 1 && r <= (int) all.size()) addEffect (s, track, all[(size_t) r - 1].id, {}, replaceSlotUid);
        else if (r >= 1000 && r < 1000 + types.size()) addEffect (s, track, "plugin", types.getReference (r - 1000).createIdentifierString(), replaceSlotUid);
        else if (r == 9999) showPluginScanner (s);
    });
}

namespace
{
struct Entry { juce::String label, sub; int kind; juce::String id; int pluginIndex; }; // kind 0 header, 1 builtin fx, 2 plugin fx, 3 instrument

class PluginList : public juce::Component, private juce::ListBoxModel, private juce::ChangeListener
{
public:
    explicit PluginList (Studio& st) : s (st)
    {
        search.setTextToShowWhenEmpty ("Search Plugin House...", col::dim);
        search.setTooltip ("Filter built-ins and installed plugins");
        search.onTextChange = [this] { rebuild(); };
        addAndMakeVisible (search);
        list.setModel (this);
        list.setRowHeight (22);
        list.setTooltip ("Double-click an effect: adds it to the selected mixer insert. Double-click an instrument: new channel");
        addAndMakeVisible (list);
        setupButton (scan, "Scan plugins", "Find every VST3 / AU plugin on this computer");
        scan.onClick = [this] { showPluginScanner (s); };
        addAndMakeVisible (scan);
        s.engine.knownPlugins.addChangeListener (this);
        rebuild();
    }
    ~PluginList() override { s.engine.knownPlugins.removeChangeListener (this); }
    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        search.setBounds (r.removeFromTop (26)); r.removeFromTop (4);
        scan.setBounds (r.removeFromBottom (28)); r.removeFromBottom (4);
        list.setBounds (r);
    }

private:
    Studio& s;
    juce::TextEditor search;
    juce::ListBox list;
    juce::TextButton scan;
    std::vector<Entry> entries;
    juce::Array<juce::PluginDescription> types;

    void rebuild()
    {
        entries.clear();
        const auto q = search.getText().trim();
        auto match = [&q] (const juce::String& a, const juce::String& b) { return q.isEmpty() || a.containsIgnoreCase (q) || b.containsIgnoreCase (q); };
        juce::String lastCat;
        entries.push_back ({ "HUMHOUSE BUILT-INS", {}, 0, {}, -1 });
        for (auto& f : builtinEffects())
            if (match (f.name, f.category)) entries.push_back ({ f.name, f.category, 1, f.id, -1 });
        types = s.engine.knownPlugins.getTypes();
        for (int pass = 0; pass < 3; ++pass)
        {
            entries.push_back ({ pass == 0 ? "HUMHOUSE PLUGINS" : pass == 1 ? "THIRD-PARTY EFFECTS" : "INSTRUMENTS", {}, 0, {}, -1 });
            for (int i = 0; i < types.size(); ++i)
            {
                auto& d = types.getReference (i);
                const bool want = pass == 2 ? d.isInstrument : (! d.isInstrument && (pass == 0) == isHumHouse (d));
                if (want && match (d.name, d.manufacturerName))
                    entries.push_back ({ d.name, d.manufacturerName + " " + d.pluginFormatName, d.isInstrument ? 3 : 2, d.createIdentifierString(), i });
            }
        }
        if (types.isEmpty()) entries.push_back ({ "No plugins scanned yet - click Scan plugins", {}, 0, {}, -1 });
        list.updateContent();
        list.repaint();
    }
    int getNumRows() override { return (int) entries.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= (int) entries.size()) return;
        auto& e = entries[(size_t) row];
        if (e.kind == 0)
        {
            g.setColour (col::accent); g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (e.label, 6, 0, w - 12, h, juce::Justification::bottomLeft);
            return;
        }
        if (selected) { g.setColour (col::accent.withAlpha (0.3f)); g.fillRect (0, 0, w, h); }
        g.setColour (col::text); g.setFont (juce::FontOptions (13.0f));
        g.drawText (e.label, 14, 0, w - 100, h, juce::Justification::centredLeft, true);
        g.setColour (col::dim); g.setFont (juce::FontOptions (10.0f));
        g.drawText (e.sub, w - 110, 0, 104, h, juce::Justification::centredRight, true);
    }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (row < 0 || row >= (int) entries.size()) return;
        auto e = entries[(size_t) row];
        if (e.kind == 1) addEffect (s, s.selectedTrack, e.id, {});
        else if (e.kind == 2) addEffect (s, s.selectedTrack, "plugin", e.id);
        else if (e.kind == 3 && e.pluginIndex >= 0 && e.pluginIndex < types.size()) addPluginChannel (s, types.getReference (e.pluginIndex));
    }
    juce::String getTooltipForRow (int row) override
    {
        if (row < 0 || row >= (int) entries.size() || entries[(size_t) row].kind == 0) return {};
        return entries[(size_t) row].kind == 3 ? "Double-click: add as a new instrument channel"
                                               : "Double-click: add to mixer insert " + juce::String (s.selectedTrack);
    }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { rebuild(); }
};

static bool isAudioFile (const juce::File& f) { return f.hasFileExtension ("wav;aif;aiff;flac;mp3;ogg"); }

struct SampleSearchFilter : public juce::FileFilter
{
    SampleSearchFilter() : juce::FileFilter ("Audio") {}
    bool isFileSuitable (const juce::File& f) const override { return isAudioFile (f); }
    bool isDirectorySuitable (const juce::File&) const override { return true; }
};

static juce::File bundledSounds()
{
   #if JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::currentApplicationFile).getChildFile ("Contents/Resources/Sounds");
   #else
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("Sounds");
   #endif
}
static juce::File downloadsFolder() { return juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads"); }

class SampleBrowser : public juce::Component, private juce::FileBrowserListener, private juce::ListBoxModel
{
public:
    explicit SampleBrowser (Studio& st) : s (st)
    {
        thread.startThread (juce::Thread::Priority::low);
        folders.setTooltip ("Your sample / kit folders (Downloads is always here; they are remembered)");
        folders.onChange = [this] { const int i = folders.getSelectedItemIndex(); if (i >= 0 && i < roots.size()) setRoot (juce::File (roots[i])); };
        addAndMakeVisible (folders);
        setupButton (addFolder, "+", "Find a folder: add a sample or drum-kit folder to the browser");
        addFolder.onClick = [this] {
            chooser = std::make_unique<juce::FileChooser> ("Add a sample folder", downloadsFolder());
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc) {
                auto f = fc.getResult();
                if (! f.isDirectory()) return;
                roots.addIfNotAlreadyThere (f.getFullPathName());
                save(); refreshFolders(); setRoot (f);
            });
        };
        setupButton (removeFolder, "-", "Remove this folder from the browser (files are not deleted)");
        removeFolder.onClick = [this] {
            const int i = folders.getSelectedItemIndex();
            if (i <= 0 || i >= roots.size()) return;   // Downloads stays
            roots.remove (i); save(); refreshFolders(); setRoot (juce::File (roots[0]));
        };
        addAndMakeVisible (addFolder); addAndMakeVisible (removeFolder);
        search.setTextToShowWhenEmpty ("Search samples...", col::dim);
        search.setTooltip ("Type to search every sample in this folder and its subfolders");
        search.onTextChange = [this] { runSearch(); };
        search.onEscapeKey = [this] { search.clear(); runSearch(); };
        addAndMakeVisible (search);
        setupButton (kitBtn, "Import as kit", "Every sample in the selected folder becomes a Channel Rack row");
        kitBtn.onClick = [this] {
            auto f = tree.getSelectedFile();
            importKit (s, f.isDirectory() ? f : list.getDirectory());
        };
        setupButton (rackBtn, "Add to rack", "Add the selected sample as a channel (or double-click it)");
        rackBtn.onClick = [this] { auto f = selected(); if (f.existsAsFile()) addSampleChannels (s, { f }, {}); };
        addAndMakeVisible (kitBtn); addAndMakeVisible (rackBtn);
        tree.addListener (this);
        tree.setTooltip ("Click a sample to hear it. Double-click or drag it into the Channel Rack, or drag it onto the Playlist. Desktop files can be dragged in too");
        tree.setDragAndDropDescription ("sample");
        tree.setColour (juce::TreeView::backgroundColourId, col::panel);
        tree.setColour (juce::TreeView::selectedItemBackgroundColourId, col::accent.withAlpha (0.35f));
        tree.setColour (juce::TreeView::linesColourId, col::gridBar);
        tree.setColour (juce::DirectoryContentsDisplayComponent::textColourId, col::text);
        tree.setColour (juce::DirectoryContentsDisplayComponent::highlightColourId, col::accent.withAlpha (0.35f));
        tree.setIndentSize (14);
        tree.setItemHeight (22);
        addAndMakeVisible (tree);
        results.setModel (this);
        results.setRowHeight (22);
        results.setColour (juce::ListBox::backgroundColourId, col::panel);
        results.setTooltip ("Search results: click to hear, double-click to add to the Channel Rack");
        addChildComponent (results);
        loadFolders();
        folders.setSelectedItemIndex (firstKit, juce::dontSendNotification);
        setRoot (juce::File (roots[firstKit]));   // opens on the built-in kits
    }
    ~SampleBrowser() override { tree.removeListener (this); }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::panel);
        auto h = header.toFloat();
        g.setGradientFill (juce::ColourGradient (col::panel2.brighter (0.06f), 0, h.getY(), col::panel, 0, h.getBottom(), false));
        g.fillRoundedRectangle (h, 6.0f);
        drawLogo (g, h.reduced (6, 5).removeFromLeft (150), true);
        g.setColour (col::dim);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        drawHeadline (g, "SAMPLES", h.reduced (8, 0).toNearestInt(), juce::Justification::centredRight);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        header = r.removeFromTop (38);
        r.removeFromTop (4);
        auto top = r.removeFromTop (26);
        removeFolder.setBounds (top.removeFromRight (26)); top.removeFromRight (2);
        addFolder.setBounds (top.removeFromRight (26)); top.removeFromRight (4);
        folders.setBounds (top);
        r.removeFromTop (4);
        search.setBounds (r.removeFromTop (26));
        r.removeFromTop (4);
        auto bottom = r.removeFromBottom (28);
        kitBtn.setBounds (bottom.removeFromLeft (bottom.getWidth() / 2).reduced (2, 0)); rackBtn.setBounds (bottom.reduced (2, 0));
        r.removeFromBottom (4);
        tree.setBounds (r); results.setBounds (r);
    }

private:
    Studio& s;
    juce::TimeSliceThread thread { "HumHouse browser" };
    SampleSearchFilter filter;
    juce::DirectoryContentsList list { &filter, thread };
    juce::FileTreeComponent tree { list };
    juce::ListBox results;
    juce::Array<juce::File> hits;
    juce::ComboBox folders;
    juce::TextEditor search;
    juce::TextButton addFolder, removeFolder, kitBtn, rackBtn;
    juce::StringArray roots;
    int firstKit = 0;
    juce::Rectangle<int> header;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::File selected() const
    {
        if (results.isVisible()) { const int r = results.getSelectedRow(); return r >= 0 && r < hits.size() ? hits[r] : juce::File(); }
        return tree.getSelectedFile();
    }
    void loadFolders()
    {
        roots = juce::StringArray::fromLines (s.settings ? s.settings->getValue ("sampleFolders") : juce::String());
        roots.removeEmptyStrings();
        roots.removeString (downloadsFolder().getFullPathName());
        roots.insert (0, downloadsFolder().getFullPathName());   // beats come from Downloads: always first
        const auto lib = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("HumHouse Studio").getChildFile ("Sounds");
        roots.removeString (lib.getFullPathName());
        if (lib.isDirectory()) roots.insert (1, lib.getFullPathName());
        // kits that ship with the app (Classic Trap, Etherea Instruments, Triple S Hitters): each one in the folder list, right after Downloads
        auto kits = bundledSounds().findChildFiles (juce::File::findDirectories, false);
        kits.sort();
        for (int k = kits.size(); --k >= 0;) { roots.removeString (kits[k].getFullPathName()); roots.insert (1, kits[k].getFullPathName()); }
        firstKit = kits.isEmpty() ? 0 : 1;
        refreshFolders();
    }
    void refreshFolders()
    {
        folders.clear (juce::dontSendNotification);
        for (int i = 0; i < roots.size(); ++i) folders.addItem (juce::File (roots[i]).getFileName(), i + 1);
    }
    void save() { if (s.settings) { s.settings->setValue ("sampleFolders", roots.joinIntoString ("\n")); s.settings->saveIfNeeded(); } }
    void setRoot (const juce::File& f)
    {
        list.setDirectory (f, true, true);
        folders.setSelectedItemIndex (roots.indexOf (f.getFullPathName()), juce::dontSendNotification);
        runSearch();
    }
    void runSearch()
    {
        const auto q = search.getText().trim();
        hits.clearQuick();
        if (q.isNotEmpty())
        {
            int scanned = 0;
            for (const auto& e : juce::RangedDirectoryIterator (list.getDirectory(), true, "*", juce::File::findFiles))
            {
                if (++scanned > 50000 || hits.size() >= 1000) break;
                const auto f = e.getFile();
                if (isAudioFile (f) && f.getFileName().containsIgnoreCase (q)) hits.add (f);
            }
        }
        results.updateContent(); results.deselectAllRows(); results.repaint();
        results.setVisible (q.isNotEmpty()); tree.setVisible (q.isEmpty());
    }
    int getNumRows() override { return hits.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override
    {
        if (row < 0 || row >= hits.size()) return;
        if (sel) { g.setColour (col::accent.withAlpha (0.35f)); g.fillRoundedRectangle (2, 1, (float) w - 4, (float) h - 2, 4.0f); }
        g.setColour (col::accent); g.fillEllipse (8, h * 0.5f - 3, 6, 6);
        g.setColour (col::text); g.setFont (juce::FontOptions (13.0f));
        g.drawText (hits[row].getFileNameWithoutExtension(), 20, 0, w - 24, h, juce::Justification::centredLeft, true);
    }
    void selectedRowsChanged (int row) override { if (row >= 0 && row < hits.size()) s.engine.previewFile (hits[row]); }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { if (row >= 0 && row < hits.size()) addSampleChannels (s, { hits[row] }, {}); }
    juce::var getDragSourceDescription (const juce::SparseSet<int>&) override { return "sample"; }
    void selectionChanged() override { auto f = tree.getSelectedFile(); if (f.existsAsFile()) s.engine.previewFile (f); }
    void fileClicked (const juce::File&, const juce::MouseEvent&) override {}
    void fileDoubleClicked (const juce::File& f) override { if (f.existsAsFile()) addSampleChannels (s, { f }, {}); }
    void browserRootChanged (const juce::File&) override {}
};

#if ! HHS_LICENSED
// PATTERNS tab (FL-style pool): beat patterns + audio clips, split by channel / track, drag onto the Playlist
class PoolBrowser : public juce::Component, private juce::ListBoxModel, private juce::Timer
{
public:
    explicit PoolBrowser (Studio& st) : s (st)
    {
        patTab.setButtonText ("BEAT PATTERNS"); audTab.setButtonText ("AUDIO CLIPS");
        for (auto* b : { &patTab, &audTab }) { b->setRadioGroupId (8); b->setClickingTogglesState (true); addAndMakeVisible (b); }
        patTab.setToggleState (true, juce::dontSendNotification);
        patTab.onClick = [this] { audio = false; refresh(); };
        audTab.onClick = [this] { audio = true; refresh(); };
        newBtn.setButtonText ("+ New pattern"); splitBtn.setButtonText ("Split by channel");
        unusedBtn.setButtonText ("Clear unused"); allBtn.setButtonText ("Clear all");
        for (auto* b : { &newBtn, &splitBtn, &unusedBtn, &allBtn }) addAndMakeVisible (b);
        newBtn.onClick = [this] { s.edit ("New pattern", [] (Project& p) { Pattern pt; pt.name = "Pattern " + juce::String (p.patterns.size() + 1); p.currentPattern = p.insertPatternAfter (p.currentPattern, pt); }); refresh(); };
        splitBtn.onClick = [this] { split(); };
        unusedBtn.onClick = [this] { askClear (false); };
        allBtn.onClick = [this] { askClear (true); };
        list.setModel (this); list.setRowHeight (24);
        list.setColour (juce::ListBox::backgroundColourId, col::panel);
        addAndMakeVisible (list);
        hint.setText ("Drag onto the Playlist or Record page. Everything stays here until you clear it (undo with Cmd/Ctrl+Z).", juce::dontSendNotification);
        hint.setFont (juce::Font (juce::FontOptions (11.0f))); hint.setColour (juce::Label::textColourId, col::dim);
        addAndMakeVisible (hint);
        startTimer (400);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        auto t = r.removeFromTop (26); patTab.setBounds (t.removeFromLeft (t.getWidth() / 2).reduced (1, 0)); audTab.setBounds (t.reduced (1, 0));
        r.removeFromTop (4);
        auto b1 = r.removeFromTop (24); newBtn.setBounds (b1.removeFromLeft (b1.getWidth() / 2).reduced (1, 0)); splitBtn.setBounds (b1.reduced (1, 0));
        r.removeFromTop (3);
        auto b2 = r.removeFromTop (24); unusedBtn.setBounds (b2.removeFromLeft (b2.getWidth() / 2).reduced (1, 0)); allBtn.setBounds (b2.reduced (1, 0));
        hint.setBounds (r.removeFromBottom (34));
        r.removeFromTop (4); list.setBounds (r);
    }

private:
    struct Row { juce::String name, sub; juce::Colour colour; int index = -1; juce::File file; bool header = false; };
    Studio& s;
    bool audio = false;
    juce::TextButton patTab, audTab, newBtn, splitBtn, unusedBtn, allBtn;
    juce::ListBox list;
    juce::Label hint;
    std::vector<Row> rows;
    juce::String sig;

    void timerCallback() override { if (isShowing()) refresh(); }
    void refresh()
    {
        auto& p = s.p();
        newBtn.setVisible (! audio); splitBtn.setVisible (! audio);
        std::vector<Row> next;
        if (! audio)
        {
            std::map<int, int> uses;
            for (auto& c : p.clips) if (c.patternIndex >= 0) ++uses[c.patternIndex];
            std::map<juce::String, std::vector<Row>> groups;   // split by channel: one-channel patterns sit under that channel
            for (int i = 0; i < (int) p.patterns.size(); ++i)
            {
                auto& pt = p.patterns[(size_t) i];
                juce::StringArray chans;
                for (auto& [id, nl] : pt.notes) if (! nl.empty()) if (auto* ch = s.channel (id)) chans.add (ch->name);
                Row r; r.name = pt.name; r.colour = pt.colour; r.index = i;
                r.sub = uses[i] > 0 ? juce::String (uses[i]) + "x" : "unused";
                groups[chans.size() == 1 ? chans[0] : chans.isEmpty() ? juce::String ("(empty)") : juce::String ("Full patterns")].push_back (r);
            }
            for (auto& [g, rs] : groups) { Row h; h.header = true; h.name = g; next.push_back (h); next.insert (next.end(), rs.begin(), rs.end()); }
        }
        else
        {
            for (auto& c : p.clips)   // every clip that ever played lands in the pool
                if (c.patternIndex < 0 && c.audioFile != juce::File() && std::find (p.audioPool.begin(), p.audioPool.end(), c.audioFile) == p.audioPool.end())
                    s.live ([f = c.audioFile] (Project& pr) { pr.audioPool.push_back (f); });
            std::map<juce::String, std::vector<Row>> groups;   // split by track
            for (auto& f : p.audioPool)
            {
                int n = 0; juce::String lane;
                for (auto& c : p.clips) if (c.patternIndex < 0 && c.audioFile == f) { ++n; if (lane.isEmpty() && c.lane >= 0 && c.lane < (int) p.lanes.size()) lane = p.lanes[(size_t) c.lane].name; }
                Row r; r.name = f.getFileNameWithoutExtension(); r.file = f; r.colour = col::accent2;
                r.sub = n > 0 ? juce::String (n) + "x" : "unused";
                groups[n > 0 ? (lane.isEmpty() ? juce::String ("Playlist") : lane) : juce::String ("Unused (kept until you clear)")].push_back (r);
            }
            for (auto& [g, rs] : groups) { Row h; h.header = true; h.name = g; next.push_back (h); next.insert (next.end(), rs.begin(), rs.end()); }
        }
        juce::String sg (audio ? "A" : "P"); sg << p.currentPattern;
        for (auto& r : next) sg << "|" << r.name << r.sub << (int) r.header;
        if (sg == sig) return;
        sig = sg; rows = std::move (next);
        list.updateContent(); list.repaint();
    }

    int selectedPattern() const
    {
        const int r = list.getSelectedRow();
        return r >= 0 && r < (int) rows.size() && ! rows[(size_t) r].header && ! audio ? rows[(size_t) r].index : s.p().currentPattern;
    }
    void split()
    {
        const int idx = selectedPattern();
        s.edit ("Split pattern by channel", [this, idx] (Project& p) {
            if (idx < 0 || idx >= (int) p.patterns.size()) return;
            const Pattern src = p.patterns[(size_t) idx];
            int filled = 0;
            for (auto& [id, nl] : src.notes) if (! nl.empty()) ++filled;
            if (filled < 2) return;
            int after = idx;
            for (auto& ch : p.channels)
            {
                auto it = src.notes.find (ch.id);
                if (it == src.notes.end() || it->second.empty()) continue;
                Pattern one; one.name = src.name + " - " + ch.name; one.colour = ch.colour; one.notes[ch.id] = it->second;
                after = p.insertPatternAfter (after, one);
            }
            juce::ignoreUnused (this);
        });
        refresh();
    }
    void askClear (bool all)
    {
        const juce::String what = audio ? "audio clips" : "patterns";
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, all ? "Clear all " + what : "Clear unused " + what,
            all ? "Remove every " + what.dropLastCharacters (1) + " and its clips from the project? (Files on disk are kept. Undo with Cmd/Ctrl+Z.)"
                : "Remove " + what + " that aren't used in the Playlist? (Files on disk are kept.)",
            "Clear", "Cancel", this, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<PoolBrowser> (this), all] (int r) { if (r != 0 && safe != nullptr) safe->clear (all); }));
    }
    void clear (bool all)
    {
        if (! audio)
            s.edit (all ? "Clear all patterns" : "Clear unused patterns", [all] (Project& p) {
                std::set<int> used;
                for (auto& c : p.clips) if (c.patternIndex >= 0) used.insert (c.patternIndex);
                p.removePatterns ([&] (int i) { return all || used.count (i) == 0; });
            });
        else
            s.edit (all ? "Clear all audio" : "Clear unused audio", [all] (Project& p) {
                if (all) { p.clips.erase (std::remove_if (p.clips.begin(), p.clips.end(), [] (const Clip& c) { return c.patternIndex < 0; }), p.clips.end()); p.audioPool.clear(); return; }
                p.audioPool.erase (std::remove_if (p.audioPool.begin(), p.audioPool.end(), [&] (const juce::File& f) {
                    return std::none_of (p.clips.begin(), p.clips.end(), [&] (const Clip& c) { return c.patternIndex < 0 && c.audioFile == f; }); }), p.audioPool.end());
            });
        sig = {}; refresh();
    }

    int getNumRows() override { return (int) rows.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= (int) rows.size()) return;
        auto& r = rows[(size_t) row];
        if (r.header) { g.setColour (col::dim); g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold))); g.drawText (r.name.toUpperCase(), 6, 0, w - 12, h, juce::Justification::bottomLeft); return; }
        if (selected) g.fillAll (col::accent.withAlpha (0.25f));
        if (! audio && r.index == s.p().currentPattern) { g.setColour (col::accent2); g.drawRect (0, 0, w, h, 1); }
        g.setColour (r.colour); g.fillRoundedRectangle (8.0f, 5.0f, 6.0f, (float) h - 10.0f, 2.0f);
        g.setColour (col::text); g.setFont (juce::Font (juce::FontOptions (13.0f)));
        g.drawText (r.name, 20, 0, w - 80, h, juce::Justification::centredLeft, true);
        g.setColour (r.sub == "unused" ? col::dim.withAlpha (0.6f) : col::dim);
        g.drawText (r.sub, w - 62, 0, 56, h, juce::Justification::centredRight);
    }
    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        if (row < 0 || row >= (int) rows.size() || rows[(size_t) row].header) return;
        if (audio) s.engine.previewFile (rows[(size_t) row].file);
        else { const int i = rows[(size_t) row].index; if (i != s.p().currentPattern) s.edit ("Open pattern", [i] (Project& p) { p.currentPattern = i; }); }
    }
    juce::var getDragSourceDescription (const juce::SparseSet<int>& sel) override
    {
        const int row = sel.isEmpty() ? -1 : sel[0];
        if (row < 0 || row >= (int) rows.size() || rows[(size_t) row].header) return {};
        return audio ? "audio:" + rows[(size_t) row].file.getFullPathName() : "pattern:" + juce::String (rows[(size_t) row].index);
    }
};
#endif

class Browser : public juce::Component
{
public:
    explicit Browser (Studio& st) : plugins (st), samples (st)
       #if ! HHS_LICENSED
        , pool (st)
       #endif
    {
        setupButton (pTab, "PLUGIN HOUSE", "Built-in effects, HumHouse plugins and every third-party plugin you own");
        setupButton (sTab, "SAMPLES / KITS", "Browse your samples and drum kits");
        for (auto* t : { &pTab, &sTab }) { t->setRadioGroupId (7); t->setClickingTogglesState (true); addAndMakeVisible (t); }
       #if ! HHS_LICENSED
        setupButton (aTab, "PATTERNS", "Every beat pattern and audio clip in the project");
        aTab.setRadioGroupId (7); aTab.setClickingTogglesState (true); addAndMakeVisible (aTab);
        aTab.onClick = [this] { show (2); };
        addChildComponent (pool);
       #endif
        pTab.onClick = [this] { show (0); };
        sTab.onClick = [this] { show (1); };
        addChildComponent (plugins); addChildComponent (samples);
        pTab.setToggleState (true, juce::dontSendNotification);
        show (0);
    }
    void paint (juce::Graphics& g) override { g.fillAll (col::panel); g.setColour (col::gridBar); g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight()); }
    void resized() override
    {
        auto r = getLocalBounds().withTrimmedRight (1);
        auto tabs = r.removeFromTop (30).reduced (4, 3);
       #if ! HHS_LICENSED
        const int w3 = tabs.getWidth() / 3;
        pTab.setBounds (tabs.removeFromLeft (w3).reduced (1, 0)); sTab.setBounds (tabs.removeFromLeft (w3).reduced (1, 0)); aTab.setBounds (tabs.reduced (1, 0));
        pool.setBounds (r);
       #else
        pTab.setBounds (tabs.removeFromLeft (tabs.getWidth() / 2).reduced (1, 0)); sTab.setBounds (tabs.reduced (1, 0));
       #endif
        plugins.setBounds (r); samples.setBounds (r);
    }

private:
    juce::TextButton pTab, sTab;
    PluginList plugins;
    SampleBrowser samples;
   #if ! HHS_LICENSED
    juce::TextButton aTab;
    PoolBrowser pool;
   #endif
    void show (int i)
    {
        plugins.setVisible (i == 0); samples.setVisible (i == 1);
       #if ! HHS_LICENSED
        pool.setVisible (i == 2);
       #endif
    }
};
}

std::unique_ptr<juce::Component> createBrowser (Studio& s) { return std::make_unique<Browser> (s); }
}
