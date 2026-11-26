#include <set>
#include "MainComponent.h"

namespace isfx
{
// ============================================================ workers (no project / UI access)
SyncInfo syncTake (const juce::File& f, const std::vector<float>& songMono, juce::String& err)
{
    SyncInfo si;
    std::vector<float> mono;
    if (! decodeAudio (f, kAnalysisRate, 1, mono, err)) { si.status = SyncInfo::Failed; si.note = "no readable audio"; return si; }
    auto r = syncToSong (songMono, mono, kAnalysisRate);
    si.offset = r.offset; si.confidence = r.confidence; si.note = r.note;
    si.status = ! r.ok ? SyncInfo::Failed : r.confidence >= 0.35f ? SyncInfo::Synced : SyncInfo::Uncertain;
    if (si.note.isEmpty()) si.note = si.status == SyncInfo::Synced ? "matched to the song" : si.status == SyncInfo::Uncertain ? "weak match" : "no match";
    return si;
}

ImportResult decodeFiles (const juce::Array<juce::File>& files, const juce::String& songIdIn, std::shared_ptr<std::vector<float>> songMonoIn,
                          bool makeSong, ProgressCb progress, std::function<bool()> cancelled)
{
    ImportResult r;
    r.songMono = songMonoIn;
    juce::String songId = songIdIn;
    std::vector<std::pair<size_t, juce::File>> videosToSync;
    for (int i = 0; i < files.size(); ++i)
    {
        if (cancelled && cancelled()) break;
        auto f = files[i];
        if (progress) progress ((double) i / files.size() * 0.7, "Reading " + f.getFileName());
        auto ext = f.getFileExtension().toLowerCase();
        if (ext == ".cube") { r.luts.add (f); continue; }
        if (ext == ".isfxpreset") { r.presets.add (f); continue; }
        if (ext == ".isfxtemplate") { r.templates.add (f); continue; }
        auto pr = probeMedia (f);
        if (! pr.ok) { r.errors.add (f.getFileName() + ": " + pr.error); continue; }
        MediaItem m; m.id = newId(); m.path = f.getFullPathName(); m.name = f.getFileName();
        m.kind = pr.kind; m.duration = pr.duration; m.fps = pr.fps; m.width = pr.width; m.height = pr.height; m.rotation = pr.rotation; m.hasAudio = pr.hasAudio;
        juce::String err;
        if (m.kind == MediaKind::Audio)
        {
            std::vector<float> mono, st;
            if (! decodeAudio (f, kAnalysisRate, 1, mono, err) || ! decodeAudio (f, kMixRate, 2, st, err)) { r.errors.add (m.name + ": " + err); continue; }
            r.waves[m.id] = buildWaveform (mono, kAnalysisRate);
            r.stereo[m.id] = std::make_shared<const std::vector<float>> (std::move (st));
            if (makeSong && songId.isEmpty())
            {
                if (ticksToSec (m.duration) > 600.5) { r.errors.add (m.name + ": songs up to 10 minutes are supported"); }
                else
                {
                    if (progress) progress ((double) i / files.size() * 0.7, "Analyzing " + m.name + " (tempo, beats, drums, sections)");
                    r.song = analyzeSong (mono, kAnalysisRate);
                    r.songMono = std::make_shared<std::vector<float>> (std::move (mono));
                    songId = r.newSongId = m.id;
                }
            }
        }
        else if (m.kind == MediaKind::Video)
        {
            r.thumbs[m.id] = makeThumbnail (f, secToTicks (std::min (1.0, ticksToSec (m.duration) / 2)), 72);
            if (m.hasAudio)
            {
                std::vector<float> mono;
                if (decodeAudio (f, kAnalysisRate, 1, mono, err)) r.waves[m.id] = buildWaveform (mono, kAnalysisRate);
                videosToSync.push_back ({ r.items.size(), f });
            }
        }
        else
        {
            r.thumbs[m.id] = makeThumbnail (f, 0, 72);
            if (m.duration <= 0) m.duration = secToTicks (5);
        }
        r.items.push_back (m);
    }
    if (r.songMono != nullptr && ! r.songMono->empty())
        for (size_t k = 0; k < videosToSync.size(); ++k)
        {
            if (cancelled && cancelled()) break;
            auto& [idx, f] = videosToSync[k];
            if (progress) progress (0.7 + 0.3 * (double) k / videosToSync.size(), "Auto-syncing " + f.getFileName() + " to the song");
            juce::String err;
            r.items[idx].sync = syncTake (f, *r.songMono, err);
        }
    return r;
}

// ============================================================ task runner
namespace
{
class Task : public juce::ThreadWithProgressWindow
{
public:
    Task (const juce::String& t, std::function<void (ProgressCb, std::function<bool()>)> w, std::function<void()> d)
        : juce::ThreadWithProgressWindow (t, true, true), work (std::move (w)), done (std::move (d)) {}
    void run() override
    {
        work ([this] (double p, const juce::String& m) { setProgress (p); setStatusMessage (m); }, [this] { return threadShouldExit(); });
    }
    void threadComplete (bool) override { if (done) done(); delete this; }
private:
    std::function<void (ProgressCb, std::function<bool()>)> work;
    std::function<void()> done;
};

class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioPluginInstance& p)
        : juce::DocumentWindow (p.getName(), theme::panel, juce::DocumentWindow::closeButton)
    {
        auto* ed = p.createEditorIfNeeded();
        if (ed == nullptr) ed = new juce::GenericAudioProcessorEditor (p);
        setContentOwned (ed, true);
        setUsingNativeTitleBar (true);
        setResizable (ed->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};
}

void MainComponent::runTask (const juce::String& title, std::function<void (ProgressCb, std::function<bool()>)> work, std::function<void()> done)
{
    if (selfTest) { work ([] (double, const juce::String&) {}, [] { return false; }); if (done) done(); return; }
    (new Task (title, std::move (work), std::move (done)))->launchThread();
}

// ============================================================ construction / layout
MainComponent::MainComponent (juce::AudioDeviceManager* external)
    : devices (external != nullptr ? *external : ownDevices), embedded (external != nullptr)
{
    setLookAndFeel (&lnf);
    if (! embedded) juce::LookAndFeel::setDefaultLookAndFeel (&lnf);
    for (auto* c : std::initializer_list<juce::Component*> { &preview, &timeline, &media, &fxlib, &inspector, &mediaTgl, &inspTgl, &maxPrevBtn, &maxTlBtn, &movieBtn, &hscroll, &fileBtn, &saveBtn, &undoBtn, &redoBtn, &importBtn,
            &presetsBtn, &pluginsBtn, &playBtn, &exportBtn, &snapBtn, &rippleBtn, &magnetBtn, &gridBtn, &chopBtn, &genBtn, &randBtn, &seqBtn, &keepBtn, &delRangeBtn, &zoomIn, &zoomOut,
            &seqBox, &aspectBox, &snapBox, &chopBox, &styleBox, &status })
        addAndMakeVisible (c);
    const char* toolHelp[4] = { "E: select, move (drag) and trim (drag clip edges). Drag in the ruler to select a range.",
                                "P: paint copies of the selected clip onto the grid. Click or drag on empty track space.",
                                "D: click or drag over clips to delete them (ripple closes gaps when R is on).",
                                "C: click a clip to cut it at the snapped position." };
    for (int i = 0; i < 4; ++i)
    {
        addAndMakeVisible (toolBtn[i]); toolBtn[i].setClickingTogglesState (false); toolBtn[i].setTooltip (toolHelp[i]);
        toolBtn[i].onClick = [this, i] { setTool ((Tool) i); };
    }
    setTool (Tool::Select);
    exportBtn.setColour (juce::TextButton::buttonColourId, theme::accent);
    genBtn.setColour (juce::TextButton::buttonColourId, theme::accent.darker (0.3f));
    fileBtn.onClick = [this] { fileMenu(); };
    saveBtn.onClick = [this] { savePage(); };
    saveBtn.setTooltip ("Save page: save, save as, save as template, recent projects and your templates");
    undoBtn.onClick = [this] { if (st.history.undo (st.project)) { st.dirty = true; refresh (true); } };
    redoBtn.onClick = [this] { if (st.history.redo (st.project)) { st.dirty = true; refresh (true); } };
    importBtn.onClick = [this] { media.onImport(); };
    presetsBtn.onClick = [this] { presetsMenu(); };
    pluginsBtn.onClick = [this] { pluginsMenu(); };
    playBtn.onClick = [this] { togglePlay(); };
    {
        auto arrow = [] (bool redo)
        {
            juce::Path arc, p;
            arc.addCentredArc (12, 14, 7, 6, 0, -1.45f, 1.9f, true);
            juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (p, arc);
            p.addTriangle (1.0f, 10.0f, 10.0f, 10.0f, 5.0f, 16.5f);
            if (redo) p.applyTransform (juce::AffineTransform::scale (-1.0f, 1.0f, 12.0f, 0.0f));
            return p;
        };
        undoBtn.setShape (arrow (false), false, true, false);
        redoBtn.setShape (arrow (true), false, true, false);
        juce::Path st; st.addRectangle (0, 0, 2.5f, 14); st.addTriangle (14, 0, 14, 14, 3.5f, 7);
        startBtn.setShape (st, false, true, false);
        startBtn.onClick = [this] { seek (0); };
        startBtn.setTooltip ("Go to start (Home)");
        qualityBox.addItem ("Full", 1); qualityBox.addItem ("1/2", 2); qualityBox.addItem ("1/4", 4); qualityBox.addItem ("1/8", 8);
        qualityBox.setSelectedId (2, juce::dontSendNotification);
        qualityBox.onChange = [this] { preview.setQuality (qualityBox.getSelectedId()); };
        qualityBox.setTooltip ("Playback resolution (like Premiere): lower = less CPU and smoother playback. Paused frames and export are always full quality.");
        addAndMakeVisible (startBtn); addAndMakeVisible (qualityBox);
        renderBtn.setTooltip ("Render the whole timeline ahead at the playback ratio (Full, 1/2, 1/4, 1/8) so playback never skips. Any edit drops the render; click again to cancel.");
        renderBtn.onClick = [this]
        {
            if (preview.renderAllProgress() >= 0) { preview.cancelRenderAll(); renderBtn.setButtonText ("RENDER"); return; }
            preview.setProject (this->st.project);
            preview.startRenderAll (qualityBox.getSelectedId());
        };
        addAndMakeVisible (renderBtn);
    }
    exportBtn.onClick = [this] { exportDialog(); };
    seqBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff7b4fd6));
    seqBtn.setTooltip ("SEQ (Triller-style): cuts all your random videos, each with its own unrelated sound, to the song's beat in a new SEQ sequence. Their audio is muted under the song. Every cut stays a normal editable clip; press again for a new shuffle.");
    seqBtn.onClick = [this]
    {
        if (! st.project.song.valid) { showMsg ("SEQ", "Import a song first, then your videos (any audio)."); return; }
        st.edit ("SEQ");
        const int i = buildSeqEdit (st.project, genSeed++);
        if (i < 0) { st.history.undo (st.project); showMsg ("SEQ", "Import some videos first."); return; }
        st.project.active = i;
        st.selected.clear();
        status.setText (st.project.sequences[(size_t) i].name + ": your videos are cut to the beat (their sound muted). Pick other SEQ versions in the sequence list.", juce::dontSendNotification);
        refresh (true);
    };
    randBtn.setTooltip ("Randomize: rebuilds this generated edit with a brand-new random variation (pace, chops, flashes, transitions). Undo brings the last one back.");
    randBtn.onClick = [this]
    {
        auto style = styleBox.getSelectedItemIndex() > 0 ? styleModes()[styleBox.getSelectedItemIndex() - 1] : juce::String();
        const auto seed = (juce::uint32) juce::Random::getSystemRandom().nextInt();
        st.edit ("Randomize");
        if (regenerateActive (st.project, seed, style)) { st.selected.clear(); status.setText ("Randomized this edit. Press again for another, Ctrl+Z to go back.", juce::dontSendNotification); refresh (true); return; }
        st.history.undo (st.project);
        genSeed = seed; generate();
    };
    genBtn.onClick = [this]
    {
        juce::PopupMenu m;
        m.addItem (1, "Generate 3 new edits (new sequences)");
        m.addItem (2, "Generate on top: stack a new pass onto this sequence", st.project.song.valid && ! st.project.seq().clips.empty());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&genBtn), [this] (int r)
        {
            if (r == 1) generate();
            if (r == 2)
            {
                st.edit ("Generate on top");
                auto style = styleBox.getSelectedItemIndex() > 0 ? styleModes()[styleBox.getSelectedItemIndex() - 1] : juce::String();
                const int n = stackGenerated (st.project, genSeed++, style);
                status.setText ("Stacked " + juce::String (n) + " generated clips on new tracks. Move, cut, swap or delete them like any clip; press again to keep stacking.", juce::dontSendNotification);
                refresh (true);
            }
        });
    };
    movieBtn.setTooltip ("Movie Border: one-click cinema bars on the whole video (preview and export).");
    movieBtn.onClick = [this]
    {
        auto& fx = st.project.seq().fx;
        const bool on = fx.get ("letterbox", 0) > 0;
        const float ratio = fx.get ("letterboxRatio", 2.39f);
        const float ratios[] { 2.39f, 2.0f, 1.85f, 2.76f };
        juce::PopupMenu m;
        m.addItem (1, "Off", true, ! on);
        for (int i = 0; i < 4; ++i)
            m.addItem (2 + i, juce::String (ratios[i], 2) + ":1", true, on && std::abs (ratio - ratios[i]) < 0.01f);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&movieBtn), [this, ratios] (int r)
        {
            if (r <= 0) return;
            st.edit ("Movie border");
            auto& f = st.project.seq().fx;
            if (r == 1) f.set ("letterbox", 0);
            else { f.set ("letterbox", 1); f.set ("letterboxRatio", ratios[r - 2]); }
            movieBtn.setToggleState (r != 1, juce::dontSendNotification);
            refresh (true);
        });
    };
    chopBtn.onClick = [this] { chop(); };
    keepBtn.onClick = [this] { if (st.rangeB <= st.rangeA) return; st.edit ("Keep range"); keepRange (st.project.seq(), st.rangeA, st.rangeB); refresh (true); };
    delRangeBtn.onClick = [this] { if (st.rangeB <= st.rangeA) return; st.edit ("Delete range"); deleteRange (st.project.seq(), st.rangeA, st.rangeB, st.ripple, -1); refresh (true); };
    zoomIn.onClick = [this] { timeline.zoomBy (1.5f, timeline.getWidth() / 2); };
    zoomOut.onClick = [this] { timeline.zoomBy (1 / 1.5f, timeline.getWidth() / 2); };
    snapBtn.setClickingTogglesState (true); snapBtn.setToggleState (st.snapOn, juce::dontSendNotification);
    rippleBtn.setClickingTogglesState (true); gridBtn.setClickingTogglesState (true); gridBtn.setToggleState (true, juce::dontSendNotification);
    snapBtn.onClick = [this] { st.snapOn = snapBtn.getToggleState(); };
    rippleBtn.onClick = [this] { st.ripple = rippleBtn.getToggleState(); };
    magnetBtn.setClickingTogglesState (true); magnetBtn.setToggleState (st.magnetic, juce::dontSendNotification);
    magnetBtn.onClick = [this] { st.magnetic = magnetBtn.getToggleState(); };
    magnetBtn.setTooltip ("Magnetic main track (CapCut style): V1 clips close up with no gaps; clips snap to edges, the playhead and beats.");
    gridBtn.onClick = [this] { st.showGrid = gridBtn.getToggleState(); timeline.repaint(); };
    snapBox.addItemList (snapModeNames(), 1); snapBox.setSelectedItemIndex ((int) st.snap, juce::dontSendNotification);
    snapBox.onChange = [this] { st.snap = (SnapMode) snapBox.getSelectedItemIndex(); };
    chopBox.addItemList (chopModeNames(), 1); chopBox.setSelectedItemIndex (1, juce::dontSendNotification);
    styleBox.addItem ("(no style mode)", 1); styleBox.addItemList (styleModes(), 2); styleBox.setSelectedItemIndex (0, juce::dontSendNotification);
    for (auto& a : aspectPresets()) aspectBox.addItem (juce::String (a.name) + "  " + a.use, aspectBox.getNumItems() + 1);
    aspectBox.addItem ("Custom...", 999);
    aspectBox.setSelectedItemIndex (0, juce::dontSendNotification);
    aspectBox.onChange = [this]
    {
        if (aspectBox.getSelectedId() == 999)
        {
            askText ("Custom aspect ratio", juce::String (st.project.aspectW) + ":" + juce::String (st.project.aspectH), [this] (juce::String s)
            {
                int w = s.upToFirstOccurrenceOf (":", false, false).getIntValue(), h = s.fromFirstOccurrenceOf (":", false, false).getIntValue();
                if (w > 0 && h > 0 && w <= 10000 && h <= 10000) { st.edit ("Aspect"); st.project.aspectW = w; st.project.aspectH = h; refresh (false); }
            });
            return;
        }
        int i = aspectBox.getSelectedItemIndex();
        if (i < 0 || i >= (int) aspectPresets().size()) return;
        st.edit ("Aspect"); st.project.aspectW = aspectPresets()[(size_t) i].w; st.project.aspectH = aspectPresets()[(size_t) i].h; refresh (false);
    };
    seqBox.onChange = [this] { int i = seqBox.getSelectedItemIndex(); if (i >= 0 && i != st.project.active && i < (int) st.project.sequences.size()) { st.project.active = i; st.selected.clear(); refresh (true); } };

    undoBtn.setTooltip ("Undo (Ctrl/Cmd+Z)"); redoBtn.setTooltip ("Redo (Ctrl/Cmd+Shift+Z)");
    playBtn.setTooltip ("Play / pause (Space). Arrow keys step one frame, Shift+arrow one beat.");
    exportBtn.setTooltip ("Export MP4 (H.264 + AAC): 720p to 4K, 24-60 fps, any aspect ratio, whole sequence or the selected range, one or all versions.");
    genBtn.setTooltip ("Builds three separate editable sequences from your synced takes: Cinematic / Atmospheric, Underground / Experimental and High-Energy / Performance. Rules-based and repeatable, every cut stays editable.");
    chopBtn.setTooltip ("Switches camera angles on the chosen grid or detected drum hits inside the selected range (or the whole song). All angles stay in sync.");
    styleBox.setTooltip ("Style mode used by GENERATE 3 EDITS for the base treatment.");
    snapBox.setTooltip ("Snap grid for moving, trimming, cutting and painting."); snapBtn.setTooltip ("AUTO SNAP (S): moving, trimming and dropping clips snaps to the playhead, other clip edges and the grid");
    rippleBtn.setTooltip ("R: ripple delete closes the gap"); gridBtn.setTooltip ("B: show the beat grid");
    aspectBox.setTooltip ("Output aspect ratio. Clips reframe with Fill, Fit or Blur Fill (clip inspector).");
    seqBox.setTooltip ("Sequences in this project (generated edits appear here)");
    keepBtn.setTooltip ("Keep only the selected range (e.g. keep the first 15 s and cut the rest by hand)");
    delRangeBtn.setTooltip ("Delete the selected range on all video tracks");
    pluginsBtn.setTooltip ("Scan for and load third-party VST3/AU audio plug-ins on the master bus (used for playback and export)");
    presetsBtn.setTooltip ("Save and load your own looks, clip effects and edit templates");
    status.setColour (juce::Label::textColourId, theme::dim); status.setFont (12.0f);

    hscroll.addListener (this); hscroll.setAutoHide (false);
    timeline.onEdited = [this] { refresh (true); };
    timeline.onTrackLink = [this] (int tr) { if (onTrackLink) onTrackLink (tr); };
    fxlib.onApply = [this] (const juce::String& d, int scope, int side) { timeline.applyLibraryItem (d, scope, side); };
    timeline.onSeek = [this] (Ticks t) { seek (t); };
    timeline.onAskText = [this] (const juce::String& t, const juce::String& d, std::function<void (juce::String)> cb) { askText (t, d, cb); };
    timeline.onFilesDropped = [this] (const juce::Array<juce::File>& files, int tr, Ticks at)
    {
        importFiles (files, [this, files, tr, at]
        {
            st.edit ("Add clip");
            Ticks t = at; juce::StringArray added;
            for (auto& f : files)
                for (auto& m : st.project.media)
                    if (m.path == f.getFullPathName() && m.id != st.project.songId)
                    {
                        auto id = addMediaClip (st.project, m.id, tr, t);
                        if (auto* c = st.project.seq().findClip (id)) { t = c->end(); added.add (id); }
                        break;
                    }
            if (! added.isEmpty()) st.selected = added;
            refresh (true);
        });
    };
    // SFX never become the song and never land on the song's track: use the drop track if it is a free audio track, else another one, else a new "SFX" track
    timeline.onSfxDropped = [this] (const juce::File& f, int tr, Ticks at)
    {
        importFiles ({ f }, [this, f, tr, at]
        {
            juce::String mid;
            for (auto& m : st.project.media)
                if (m.path == f.getFullPathName() && m.id != st.project.songId) { mid = m.id; break; }
            if (mid.isEmpty()) return;
            st.edit ("Add sound");
            auto& s = st.project.seq();
            const auto* md = st.project.findMedia (mid);
            const Ticks end = at + std::max<Ticks> (1, md != nullptr ? md->duration : 1);
            auto usable = [&] (int i)
            {
                if (i < 0 || i >= (int) s.tracks.size() || s.tracks[(size_t) i].kind != TrackKind::Audio || s.tracks[(size_t) i].locked) return false;
                for (auto& c : s.clips)
                    if (c.track == i && (c.mediaId == st.project.songId || (c.start < end && c.end() > at))) return false;
                return true;
            };
            int track = tr;
            for (int i = 0; ! usable (track) && i < (int) s.tracks.size(); ++i) track = i;
            if (! usable (track)) track = s.addTrack (TrackKind::Audio, "SFX");
            auto id = addMediaClip (st.project, mid, track, at);
            if (id.isNotEmpty()) st.selected = { id };
            refresh (true);
        }, false);
    };
    inspector.onEdited = [this] { refresh (false); };
    preview.onEdited = [this] { refresh (false); timeline.repaint(); };
    preview.onSelect = [this] { refresh (true); };
    mediaTgl.setTooltip ("Show or collapse the Media + Effects column."); inspTgl.setTooltip ("Show or collapse the Inspector.");
    maxPrevBtn.setTooltip ("Maximize the preview (hides the side panels and timeline). Click again to restore.");
    maxTlBtn.setTooltip ("Maximize the timeline. Drag the bar above the timeline tools to resize it freely.");
    for (auto* b : { &mediaTgl, &inspTgl, &maxPrevBtn, &maxTlBtn }) b->setClickingTogglesState (true);
    mediaTgl.setToggleState (true, juce::dontSendNotification); inspTgl.setToggleState (true, juce::dontSendNotification);
    mediaTgl.onClick = [this] { showMedia = mediaTgl.getToggleState(); resized(); };
    inspTgl.onClick = [this] { showInspector = inspTgl.getToggleState(); resized(); };
    maxPrevBtn.onClick = [this] { maxPreview = maxPrevBtn.getToggleState(); if (maxPreview) { maxTimeline = false; maxTlBtn.setToggleState (false, juce::dontSendNotification); } resized(); };
    maxTlBtn.onClick = [this] { maxTimeline = maxTlBtn.getToggleState(); if (maxTimeline) { maxPreview = false; maxPrevBtn.setToggleState (false, juce::dontSendNotification); } resized(); };
    media.onImport = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Import media", juce::File(), "*.mp3;*.wav;*.flac;*.aif;*.aiff;*.m4a;*.mp4;*.mov;*.m4v;*.mkv;*.avi;*.mxf;*.webm;*.png;*.jpg;*.jpeg;*.cube;*.isfxpreset;*.isfxtemplate");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& fc) { if (! fc.getResults().isEmpty()) importFiles (fc.getResults()); });
    };
    media.onSyncAll = [this] { autoSync(); };
    media.onResync = [this] (const juce::String& id) { autoSync (id); };
    timeline.onStatus = [this] (const juce::String& m) { status.setText (m, juce::dontSendNotification); };
    timeline.onSyncClips = [this] (const juce::StringArray& ids) { syncClips (ids); };
    timeline.onTrackPlugins = [this] (int tr) { trackPluginsMenu (tr); };
    timeline.onStabilize = [this] (const juce::StringArray& ids, float sm) { stabilizeClips (ids, sm); };
    timeline.onSliceCuts = [this] (const juce::StringArray& ids) { sliceAtCuts (ids); };
    preview.onCacheChanged = [this] { timeline.repaint(); };
    player.setTrackFx (&trackRacks);
    media.onSetSong = [this] (const juce::String& id) { setSong (id); };
    media.onRelink = [this] (const juce::String& id) { relink (id); };
    media.onRemove = [this] (const juce::String& id) { removeMedia (id); };
    media.onAddToTimeline = [this] (const juce::String& id)
    {
        st.edit ("Add clip"); auto cid = addMediaClip (st.project, id, -1, st.playhead); if (cid.isNotEmpty()) st.selected = { cid }; refresh (true);
    };

    if (! embedded) devices.initialiseWithDefaultDevices (0, 2);
    devices.addAudioCallback (&player);
    rack.prepare (kMixRate, Player::kBlock);
    setWantsKeyboardFocus (true);
    setSize (1600, 960);
    refresh (true);
    startTimerHz (30);
}

void MainComponent::scoreMediaInBackground()
{   // SEQ/Generate shot scoring: off the import path, one low-priority pass after the clips are already usable
    std::vector<std::pair<juce::String, juce::File>> todo;
    for (auto& m : st.project.media) if (m.kind == MediaKind::Video && ! m.missing && m.moments.empty()) todo.push_back ({ m.id, juce::File (m.path) });
    if (todo.empty()) return;
    if (selfTest)
    {
        for (auto& [id, f] : todo) if (auto* mi = st.project.findMedia (id)) { mi->quality = scoreShotQuality (f); mi->moments = scoreMoments (f); }
        return;
    }
    juce::Component::SafePointer<MainComponent> sp (this);
    auto alive = bgAlive;
    juce::Thread::launch ([todo, sp, alive]
    {
        for (auto& [id, f] : todo)
        {
            if (! alive->load()) return;
            const float q = scoreShotQuality (f);
            auto mo = scoreMoments (f, [alive] { return ! alive->load(); });
            juce::MessageManager::callAsync ([sp, id = id, q, mo]
            {
                if (sp != nullptr) if (auto* mi = sp->st.project.findMedia (id)) { mi->quality = q; if (mi->moments.empty()) mi->moments = mo; }
            });
        }
    });
}

MainComponent::~MainComponent()
{
    bgAlive->store (false);
    stopTimer();
    devices.removeAudioCallback (&player);
    pluginWindows.clear();
    setLookAndFeel (nullptr);
}

ISFXLookAndFeel& sharedLookAndFeel() { static ISFXLookAndFeel l; return l; }

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    const int topH = getWidth() < 1860 ? 80 : 44;
    g.setGradientFill (juce::ColourGradient (juce::Colours::black, 0, 0, theme::panel, 0, (float) topH, false)); g.fillRect (0, 0, getWidth(), topH);
    g.setColour (theme::line); g.fillRect (0, 43, getWidth(), 1);
    drawLogo (g, { 12.0f, 0.0f, 40.0f, 44.0f });
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    // too narrow for one row (embedded in HumHouse VIDEO): transport/render/export move to a second row so nothing overlaps
    const bool twoRows = getWidth() < 1860;
    auto top = r.removeFromTop (twoRows ? 80 : 44).reduced (6, 7);
    auto right = twoRows ? top.removeFromBottom (30) : top;
    if (twoRows) top = top.removeFromTop (30);
    top.removeFromLeft (50);
    auto place = [] (juce::Rectangle<int>& row, juce::Component& c, int w) { c.setBounds (row.removeFromLeft (w)); row.removeFromLeft (5); };
    place (top, fileBtn, 56); place (top, saveBtn, 56); place (top, undoBtn, 30); place (top, redoBtn, 30); place (top, importBtn, 70); place (top, presetsBtn, 76); place (top, pluginsBtn, 80);
    top.removeFromLeft (6); place (top, mediaTgl, 58); place (top, inspTgl, 72); place (top, maxPrevBtn, 92); place (top, maxTlBtn, 98); place (top, movieBtn, 104);
    exportBtn.setBounds (right.removeFromRight (110)); right.removeFromRight (6);
    playBtn.setBounds (right.removeFromRight (70)); right.removeFromRight (4);
    startBtn.setBounds (right.removeFromRight (26).reduced (3, twoRows ? 3 : 6)); right.removeFromRight (6);
    qualityBox.setBounds (right.removeFromRight (70)); right.removeFromRight (6);
    renderBtn.setBounds (right.removeFromRight (104)); right.removeFromRight (10);
    aspectBox.setBounds (right.removeFromRight (230)); right.removeFromRight (6);
    seqBox.setBounds (right.removeFromRight (240)); right.removeFromRight (6);
    if (twoRows) right.removeFromLeft (50);
    status.setBounds (right);

    const bool showBottom = ! maxPreview;
    int bh = maxTimeline ? r.getHeight() - 120 : juce::jlimit (200, std::max (200, r.getHeight() - 160), (int) (r.getHeight() * timelineFrac));
    auto bottom = r.removeFromBottom (showBottom ? bh : 0);
    if (showBottom) r.removeFromBottom (6);   // drag bar
    splitY = bottom.getY() - 3;
    for (auto* c : std::initializer_list<juce::Component*> { &timeline, &hscroll, &zoomIn, &zoomOut, &snapBox, &snapBtn, &rippleBtn, &magnetBtn, &gridBtn,
                                                           &chopBox, &chopBtn, &styleBox, &genBtn, &randBtn, &seqBtn, &keepBtn, &delRangeBtn })
        c->setVisible (showBottom);
    for (auto& b : toolBtn) b.setVisible (showBottom);
    auto bar = bottom.removeFromTop (36).reduced (6, 5);
    for (int i = 0; i < 4; ++i) place (bar, toolBtn[i], 76);
    bar.removeFromLeft (8);
    place (bar, snapBox, 80); place (bar, snapBtn, 84); place (bar, rippleBtn, 76); place (bar, magnetBtn, 70); place (bar, gridBtn, 64);
    bar.removeFromLeft (8);
    place (bar, chopBox, 104); place (bar, chopBtn, 64);
    bar.removeFromLeft (8);
    place (bar, styleBox, 130); place (bar, genBtn, 150); place (bar, randBtn, 86); place (bar, seqBtn, 56);
    bar.removeFromLeft (8);
    place (bar, keepBtn, 92); place (bar, delRangeBtn, 100);
    zoomIn.setBounds (bar.removeFromRight (30)); bar.removeFromRight (4); zoomOut.setBounds (bar.removeFromRight (30));
    hscroll.setBounds (bottom.removeFromBottom (14));
    timeline.setBounds (bottom);
    const bool side = ! maxPreview;
    media.setVisible (side && showMedia); fxlib.setVisible (side && showMedia); inspector.setVisible (side && showInspector);
    if (side && showMedia)
    {
        auto left = r.removeFromLeft (juce::jlimit (180, std::max (180, getWidth() / 2), leftW));
        leftSplitX = r.getX() + 2; r.removeFromLeft (5);   // drag bar
        auto fxR = left.removeFromBottom (juce::jlimit (80, std::max (80, left.getHeight() - 80), (int) (left.getHeight() * fxFrac)));
        fxSplitY = fxR.getY() - 3; left.removeFromBottom (5);   // drag bar
        fxlib.setBounds (fxR);
        media.setBounds (left);
    }
    if (side && showInspector) inspector.setBounds (r.removeFromRight (340));
    preview.setBounds (r);
    preview.requestFrame (st.playhead);
}

// ============================================================ state refresh / transport
void MainComponent::refresh (bool inspectorToo)
{
    player.setSequence (playbackSequence());
    preview.setProject (st.project);
    preview.projectEdited (st.project);
    preview.requestFrame (st.playhead);
    preview.repaint();
    timeline.repaint();
    media.refresh();
    updateSeqBox();
    undoBtn.setEnabled (st.history.canUndo()); redoBtn.setEnabled (st.history.canRedo());
    for (size_t i = 0; i < aspectPresets().size(); ++i)
        if (aspectPresets()[i].w == st.project.aspectW && aspectPresets()[i].h == st.project.aspectH) aspectBox.setSelectedItemIndex ((int) i, juce::dontSendNotification);
    if (inspectorToo) inspector.rebuild();
    double dur = std::max (30.0, ticksToSec (st.project.seq().duration()) + 10);
    hscroll.setRangeLimits (0, dur, juce::dontSendNotification);
    hscroll.setCurrentRange (timeline.scrollSec, (timeline.getWidth() - 70) / timeline.pxPerSec, juce::dontSendNotification);
    if (onTitle) onTitle ((st.file.existsAsFile() ? st.file.getFileNameWithoutExtension() : st.project.name) + (st.dirty ? " *" : "") + " - InSequence FX");
}

void MainComponent::updateSeqBox()
{
    seqBox.clear (juce::dontSendNotification);
    for (auto& s : st.project.sequences) seqBox.addItem (s.name, seqBox.getNumItems() + 1);
    seqBox.setSelectedItemIndex (st.project.active, juce::dontSendNotification);
}

void MainComponent::seek (Ticks t)
{
    st.playhead = std::max<Ticks> (0, t);
    if (player.isPlaying()) player.play (st.playhead);
    preview.requestFrame (st.playhead);
    preview.repaint(); timeline.repaint();
}

void MainComponent::hostTransport (bool playing, double seconds)
{
    const auto t = secToTicks (juce::jmax (0.0, seconds));
    if (playing && ! player.isPlaying()) { st.playhead = t; togglePlay(); }
    else if (! playing && player.isPlaying()) togglePlay();
    else if (! playing && std::abs (ticksToSec (st.playhead) - seconds) > 0.02) seek (t);
}

std::vector<MainComponent::TrackRender> MainComponent::renderTracks (const juce::File& dir)
{
    std::vector<TrackRender> out;
    const auto& p = st.project;
    if (p.sequences.empty()) return out;
    const auto& seq = p.sequences[(size_t) juce::jlimit (0, (int) p.sequences.size() - 1, p.active)];
    const juce::int64 total = (juce::int64) std::llround ((double) seq.duration() * kMixRate / (double) kFlicks);
    if (total <= 0 || ! dir.createDirectory()) return out;
    constexpr int blk = 8192;
    std::vector<float> L (blk), R (blk);
    for (int t = 0; t < (int) seq.tracks.size(); ++t)
    {
        bool any = false;
        for (auto& c : seq.clips) any = any || (c.track == t && c.enabled && st.audio.has (c.mediaId));
        if (! any || seq.tracks[(size_t) t].mute) continue;
        Sequence one = seq;
        for (int k = 0; k < (int) one.tracks.size(); ++k) { one.tracks[(size_t) k].solo = false; one.tracks[(size_t) k].mute = k != t; }
        const auto name = seq.tracks[(size_t) t].name.isNotEmpty() ? seq.tracks[(size_t) t].name : "Track " + juce::String (t + 1);
        auto f = dir.getChildFile (juce::File::createLegalFileName (name) + ".wav").getNonexistentSibling();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), kMixRate, 2, 24, {}, 0));
        if (w == nullptr) continue;
        for (juce::int64 pos = 0; pos < total; pos += blk)
        {
            const int n = (int) std::min<juce::int64> (blk, total - pos);
            mixSequence (one, st.audio, pos, n, L.data(), R.data(), nullptr);
            const float* ch[2] { L.data(), R.data() };
            w->writeFromFloatArrays (ch, 2, n);
        }
        w.reset();
        out.push_back ({ name, f, (double) total / kMixRate });
    }
    return out;
}

void MainComponent::setTrackRecLane (int t, int lane)
{
    auto& seq = st.project.seq();
    if (t < 0 || t >= (int) seq.tracks.size() || seq.tracks[(size_t) t].recLane == lane) return;
    st.edit ("Route track to REC");
    seq.tracks[(size_t) t].recLane = lane;
    if (seq.tracks[(size_t) t].routeKey.isEmpty()) seq.tracks[(size_t) t].routeKey = juce::Uuid().toString().substring (0, 8);
    player.setSequence (playbackSequence());
}

Sequence MainComponent::playbackSequence() const
{
    Sequence q = st.project.seq();
    for (auto& t : q.tracks) if (t.recLane >= 0) t.mute = true;
    return q;
}

juce::StringArray MainComponent::trackSignatures() const
{
    juce::StringArray out;
    const auto& seq = st.project.seq();
    const auto v = toVar (st.project);
    const juce::var* sv = nullptr;
    if (auto* seqs = v["sequences"].getArray())
        for (auto& x : *seqs) if (x["id"].toString() == seq.id) sv = &x;
    for (int t = 0; t < (int) seq.tracks.size(); ++t)
    {
        juce::String sig = seq.tracks[(size_t) t].mute ? "M" : "-";
        if (sv != nullptr) if (auto* cl = (*sv)["clips"].getArray())
            for (auto& c : *cl) if ((int) c["track"] == t) sig << juce::JSON::toString (c, true);
        out.add (sig);
    }
    return out;
}

void MainComponent::renderTrackAsync (int t, const juce::File& f, std::function<void (double)> done)
{
    if (st.project.sequences.empty()) return;
    Sequence one = st.project.seq();
    if (t < 0 || t >= (int) one.tracks.size()) return;
    for (int k = 0; k < (int) one.tracks.size(); ++k) { one.tracks[(size_t) k].solo = false; one.tracks[(size_t) k].mute = k != t || one.tracks[(size_t) k].mute; }   // a muted track renders silent
    auto cache = std::make_shared<AudioCache>();   // just this track's decoded audio, shared (not copied), so the thread owns what it reads
    for (auto& c : one.clips) if (c.track == t) if (auto a = st.audio.get (c.mediaId)) cache->put (c.mediaId, a);
    juce::Thread::launch ([one = std::move (one), cache, f, done = std::move (done), safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        const juce::int64 total = (juce::int64) std::llround ((double) one.duration() * kMixRate / (double) kFlicks);
        double secs = 0.0;
        bool any = false;
        for (auto& c : one.clips) any = any || (c.track >= 0 && ! one.tracks[(size_t) c.track].mute && c.enabled && cache->has (c.mediaId));
        if (any && total > 0 && f.getParentDirectory().createDirectory())
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), kMixRate, 2, 24, {}, 0));
            if (w != nullptr)
            {
                constexpr int blk = 8192;
                std::vector<float> L (blk), R (blk);
                for (juce::int64 pos = 0; pos < total; pos += blk)
                {
                    const int n = (int) std::min<juce::int64> (blk, total - pos);
                    mixSequence (one, *cache, pos, n, L.data(), R.data(), nullptr);
                    const float* ch[2] { L.data(), R.data() };
                    w->writeFromFloatArrays (ch, 2, n);
                }
                secs = (double) total / kMixRate;
            }
        }
        juce::MessageManager::callAsync ([safe, done, secs] { if (safe != nullptr) done (secs); });
    });
}

std::vector<MainComponent::ClipRender> MainComponent::renderClips (const juce::File& dir, bool selectedOnly)
{
    std::vector<ClipRender> out;
    const auto& p = st.project;
    if (p.sequences.empty() || ! dir.createDirectory()) return out;
    const auto& seq = p.sequences[(size_t) juce::jlimit (0, (int) p.sequences.size() - 1, p.active)];
    constexpr int blk = 8192;
    std::vector<float> L (blk), R (blk);
    for (auto& c : seq.clips)
    {
        if (! c.enabled || c.length <= 0 || ! st.audio.has (c.mediaId) || (selectedOnly && ! st.selected.contains (c.id))) continue;
        if (c.track < 0 || c.track >= (int) seq.tracks.size() || seq.tracks[(size_t) c.track].mute) continue;
        Sequence one = seq;
        one.clips = { c };
        for (auto& t : one.tracks) { t.solo = false; t.mute = false; }
        const auto s0 = (juce::int64) std::llround (ticksToSec (c.start) * kMixRate);
        const auto s1 = (juce::int64) std::llround (ticksToSec (c.end()) * kMixRate);
        const auto* m = p.findMedia (c.mediaId);
        const auto name = c.name.isNotEmpty() ? c.name : m != nullptr && m->name.isNotEmpty() ? m->name : juce::String ("Clip");
        auto f = dir.getChildFile (juce::File::createLegalFileName (name) + ".wav").getNonexistentSibling();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), kMixRate, 2, 24, {}, 0));
        if (w == nullptr || s1 <= s0) continue;
        for (auto pos = s0; pos < s1; pos += blk)
        {
            const int n = (int) std::min<juce::int64> (blk, s1 - pos);
            mixSequence (one, st.audio, pos, n, L.data(), R.data(), nullptr);
            const float* ch[2] { L.data(), R.data() };
            w->writeFromFloatArrays (ch, 2, n);
        }
        w.reset();
        out.push_back ({ name, f, ticksToSec (c.start), (double) (s1 - s0) / kMixRate, c.track });
    }
    return out;
}

juce::File MainComponent::songFile() const
{
    if (auto* m = st.project.song_()) return juce::File (m->path);
    return {};
}

void MainComponent::togglePlay()
{
    if (player.isPlaying()) { player.stop(); playBtn.setButtonText ("Play"); preview.setLive (false); return; }
    preview.setLive (true);
    player.setSequence (playbackSequence());
    player.play (st.playhead);
    playBtn.setButtonText ("Pause");
}

void MainComponent::timerCallback()
{
    {
        const float pr = preview.renderAllProgress();
        juce::String txt = pr < 0 ? "RENDER" : pr >= 1.0f ? "RENDERED" : "RENDER " + juce::String ((int) (pr * 100)) + "%";
        if (renderBtn.getButtonText() != txt) renderBtn.setButtonText (txt);
    }
    // Space = play/stop everywhere: buttons, combos and sliders never keep keyboard focus (JUCE buttons eat Space)
    if (++focusSweep % 15 == 0)
    {
        std::function<void (juce::Component&)> sweep = [&] (juce::Component& c)
        {
            for (auto* ch : c.getChildren())
            {
                if (dynamic_cast<juce::Button*> (ch) || dynamic_cast<juce::ComboBox*> (ch) || dynamic_cast<juce::Slider*> (ch))
                { ch->setWantsKeyboardFocus (false); ch->setMouseClickGrabsKeyboardFocus (false); }
                sweep (*ch);
            }
        };
        sweep (*this);
        if (auto* f = juce::Component::getCurrentlyFocusedComponent(); f == nullptr || (! dynamic_cast<juce::TextEditor*> (f) && ! isParentOf (f) && f != this && isShowing()))
            if (f == nullptr && isShowing()) grabKeyboardFocus();
    }
    if (! player.isPlaying()) { preview.setLive (false); return; }
    auto t = player.position();
    auto dur = st.project.seq().duration();
    if (t >= dur) { player.stop(); playBtn.setButtonText ("Play"); t = dur; }
    st.playhead = t;
    preview.requestFrame (t);
    timeline.ensureVisible (t);
    hscroll.setCurrentRange (timeline.scrollSec, (timeline.getWidth() - 70) / timeline.pxPerSec, juce::dontSendNotification);
    timeline.repaint(); preview.repaint();
}

void MainComponent::mouseMove (const juce::MouseEvent& e)
{
    const bool side = ! maxPreview && showMedia && e.y < splitY;
    if (side && std::abs (e.x - leftSplitX) <= 4) { setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); return; }
    if (side && e.x < leftSplitX && std::abs (e.y - fxSplitY) <= 4) { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); return; }
    setMouseCursor (! maxPreview && std::abs (e.y - splitY) <= 4 ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}

void MainComponent::mouseDown (const juce::MouseEvent& e)
{
    const bool side = ! maxPreview && showMedia && e.y < splitY;
    dragPanel = side && std::abs (e.x - leftSplitX) <= 4 ? 1 : side && e.x < leftSplitX && std::abs (e.y - fxSplitY) <= 4 ? 2 : 0;
    draggingSplit = dragPanel == 0 && ! maxPreview && std::abs (e.y - splitY) <= 4;
}

void MainComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (dragPanel == 1) { leftW = juce::jlimit (180, std::max (180, getWidth() / 2), e.x - 2); resized(); return; }
    if (dragPanel == 2)
    {   // effects panel grows upward, media/import panel shrinks
        const float colTop = 44.0f, colH = (float) std::max (1, splitY - 44);
        fxFrac = juce::jlimit (0.15f, 0.85f, ((float) splitY - (float) e.y) / colH); juce::ignoreUnused (colTop);
        resized(); return;
    }
    if (! draggingSplit) return;
    float avail = (float) std::max (1, getHeight() - (getWidth() < 1860 ? 80 : 44));
    timelineFrac = juce::jlimit (0.15f, 0.85f, (float) (getHeight() - e.y) / avail);
    maxTimeline = false; maxTlBtn.setToggleState (false, juce::dontSendNotification);
    resized();
}

void MainComponent::scrollBarMoved (juce::ScrollBar*, double start) { timeline.scrollSec = start; timeline.repaint(); }

void MainComponent::setTool (Tool t)
{
    timeline.tool = t;
    for (int i = 0; i < 4; ++i) toolBtn[i].setToggleState (i == (int) t, juce::dontSendNotification);
}

bool MainComponent::keyPressed (const juce::KeyPress& k)
{
    const auto cmd = k.getModifiers().isCommandDown(), shift = k.getModifiers().isShiftDown();
    const int c = k.getKeyCode();
    auto& s = st.project.seq();
    if (cmd)
    {
        if (c == 'Z' && shift) { redoBtn.onClick(); return true; }
        if (c == 'Z') { undoBtn.onClick(); return true; }
        if (c == 'Y') { redoBtn.onClick(); return true; }
        if (c == 'S') { save (shift); return true; }
        if (c == 'O') { open(); return true; }
        if (c == 'I') { media.onImport(); return true; }
        if (c == 'E') { exportDialog(); return true; }
        if (c == 'N') { newProject(); return true; }
        if (c == 'A') { timeline.selectAll(); return true; }
        if (c == 'K') { st.edit ("Split"); splitAt (s, st.playhead, st.selected, st.project.songId); refresh (true); return true; }
        if (c == 'C') { st.clipboard = copyClips (s, st.selected); return true; }
        if (c == 'X' && ! st.selected.isEmpty()) { st.clipboard = copyClips (s, st.selected); st.edit ("Cut"); deleteClips (s, st.selected, false); st.selected.clear(); refresh (true); return true; }
        if (c == 'V' && k.getModifiers().isAltDown())
        {
            if (st.clipboard.size() == 1 && ! st.selected.isEmpty()) { st.edit ("Paste Attributes"); pasteAttributes (s, st.clipboard.front(), st.selected); refresh (true); }
            return true;
        }
        if (c == 'V' && ! st.clipboard.empty()) { st.edit ("Paste"); st.selected = pasteClips (s, st.clipboard, st.playhead); refresh (true); return true; }
        if (c == 'R' && st.selected.size() == 1) { timeline.clipMenuSpeed (st.selected[0]); return true; }
        return false;
    }
    auto ch = juce::CharacterFunctions::toLowerCase (k.getTextCharacter());
    if (ch == 'e' && shift)
    {
        if (! st.selected.isEmpty())
        {
            st.edit ("Enable"); bool on = true;
            if (auto* f = s.findClip (st.selected[0])) on = ! f->enabled;
            for (auto& id : st.selected) if (auto* cl = s.findClip (id)) cl->enabled = on;
            refresh (true);
        }
        return true;
    }
    if (ch == 'a' && ! shift) { timeline.selectAll(); return true; }
    if (ch == 'e') { setTool (Tool::Select); return true; }
    if (ch == 'p') { setTool (Tool::Paint); return true; }
    if (ch == 'd') { setTool (Tool::Delete); return true; }
    if (ch == 'c') { setTool (Tool::Cut); return true; }
    if (ch == 'r') { rippleBtn.setToggleState (! st.ripple, juce::sendNotification); st.ripple = rippleBtn.getToggleState(); return true; }
    if (ch == 's') { snapBtn.setToggleState (! st.snapOn, juce::sendNotification); st.snapOn = snapBtn.getToggleState(); return true; }
    if (ch == 'b') { gridBtn.setToggleState (! st.showGrid, juce::sendNotification); st.showGrid = gridBtn.getToggleState(); timeline.repaint(); return true; }
    if (ch == 'l') { st.edit ("Lock"); for (auto& id : st.selected) if (auto* cl = s.findClip (id)) cl->locked = ! cl->locked; refresh (false); return true; }
    if (ch == 'g')
    {
        if (st.selected.isEmpty()) return true;
        st.edit ("Group");
        bool allGrouped = true; int g0 = 0;
        for (auto& id : st.selected) if (auto* cl = s.findClip (id)) { allGrouped &= cl->group != 0 && (g0 == 0 || cl->group == g0); g0 = cl->group; }
        int ng = 1; for (auto& cl : s.clips) ng = std::max (ng, cl.group + 1);
        for (auto& id : st.selected) if (auto* cl = s.findClip (id)) cl->group = allGrouped ? 0 : ng;
        status.setText (allGrouped ? "Ungrouped" : "Grouped " + juce::String (st.selected.size()) + " clips", juce::dontSendNotification);
        refresh (false); return true;
    }
    if (ch == '+' || ch == '=') { zoomIn.onClick(); return true; }
    if (ch == '-') { zoomOut.onClick(); return true; }
    if (c == juce::KeyPress::spaceKey) { togglePlay(); return true; }
    if (c == juce::KeyPress::returnKey)
    {
        preview.renderRed();
        status.setText ("Rendering the red sections in the background. Green = rendered: Space plays them full quality in real time.", juce::dontSendNotification);
        return true;
    }
    if (c == juce::KeyPress::homeKey) { seek (0); return true; }
    if (c == juce::KeyPress::leftKey || c == juce::KeyPress::rightKey)
    {
        int dir = c == juce::KeyPress::leftKey ? -1 : 1;
        Ticks step = shift && st.project.song.valid ? secToTicks (st.project.song.beatSec()) : frameToTicks (1, st.project.fps);
        seek (shift ? st.snapT (st.playhead + dir * step) : quantizeToFrame (st.playhead, st.project.fps) + dir * step);
        timeline.ensureVisible (st.playhead); return true;
    }
    if (c == juce::KeyPress::deleteKey || c == juce::KeyPress::backspaceKey)
    {
        if (! st.selected.isEmpty()) { st.edit (shift ? "Ripple Delete" : "Delete"); deleteClips (s, st.selected, st.ripple || shift); st.selected.clear(); refresh (true); }
        else if (st.rangeB > st.rangeA) delRangeBtn.onClick();
        return true;
    }
    if (c == juce::KeyPress::escapeKey) { st.selected.clear(); st.rangeA = st.rangeB = 0; refresh (true); return true; }
    return false;
}

void MainComponent::showMsg (const juce::String& title, const juce::String& msg)
{
    if (selfTest) { std::cout << title << ": " << msg << "\n"; return; }
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, title, msg);
}

void MainComponent::askText (const juce::String& title, const juce::String& def, std::function<void (juce::String)> cb)
{
    dialog = std::make_unique<juce::AlertWindow> (title, juce::String(), juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor ("t", def);
    dialog->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    dialog->enterModalState (true, juce::ModalCallbackFunction::create ([this, cb] (int r)
    {
        auto text = dialog->getTextEditorContents ("t").trim();
        juce::MessageManager::callAsync ([this] { dialog.reset(); });
        if (r == 1 && text.isNotEmpty()) cb (text);
    }), false);
}

// ============================================================ media
void MainComponent::importFiles (const juce::Array<juce::File>& files, std::function<void()> then, bool allowSong)
{
    auto res = std::make_shared<ImportResult>();
    auto songId = st.project.songId; auto mono = songMono;
    bool makeSong = allowSong && st.project.songId.isEmpty();
    runTask ("Importing media", [res, files, songId, mono, makeSong] (ProgressCb p, std::function<bool()> c)
             { *res = decodeFiles (files, songId, mono, makeSong, p, c); },
             [this, res, then] { applyImport (*res); if (then) then(); scoreMediaInBackground(); });
}

void MainComponent::placeSongClip()
{
    auto* sm = st.project.song_();
    if (sm == nullptr) return;
    for (auto& s : st.project.sequences)
    {
        s.clips.erase (std::remove_if (s.clips.begin(), s.clips.end(), [&] (const Clip& c)
            { return c.track >= 0 && c.track < (int) s.tracks.size() && s.tracks[(size_t) c.track].kind == TrackKind::Audio && c.locked && c.start == 0 && c.mediaId != sm->id
                     && st.project.findMedia (c.mediaId) && st.project.findMedia (c.mediaId)->kind == MediaKind::Audio; }), s.clips.end());
        bool has = false; for (auto& c : s.clips) has |= c.mediaId == sm->id;
        if (has) continue;
        int at = -1;
        for (int i = 0; i < (int) s.tracks.size(); ++i) if (s.tracks[(size_t) i].kind == TrackKind::Audio) { at = i; break; }
        if (at < 0) at = s.addTrack (TrackKind::Audio, "SONG");
        Clip c; c.id = newId(); c.mediaId = sm->id; c.track = at; c.start = 0; c.length = sm->duration; c.volume = 1; c.locked = true;
        s.clips.push_back (c); s.sortClips();
    }
}

void MainComponent::applyImport (ImportResult& r)
{
    st.edit ("Import");
    for (auto& m : r.items) st.project.media.push_back (m);
    for (auto& [k, v] : r.thumbs) st.thumbs[k] = v;
    for (auto& [k, v] : r.waves) st.waves[k] = v;
    for (auto& [k, v] : r.stereo) st.audio.put (k, v);
    if (r.newSongId.isNotEmpty())
    {
        st.project.songId = r.newSongId; st.project.song = r.song; songMono = r.songMono;
        placeSongClip();
    }
    bool anySynced = false; int uncertain = 0;
    for (auto& m : r.items) { anySynced |= m.sync.status == SyncInfo::Synced; uncertain += m.sync.status == SyncInfo::Uncertain; }
    bool emptyVideo = true;
    for (auto& c : st.project.seq().clips) if (st.project.seq().tracks[(size_t) c.track].kind == TrackKind::Video) emptyVideo = false;
    if (anySynced && emptyVideo) layoutSyncedTakes (st.project);
    for (auto& f : r.luts) { auto dst = userDataDir().getChildFile ("LUTs"); dst.createDirectory(); f.copyFileTo (dst.getChildFile (f.getFileName())); }
    for (auto& f : r.presets) f.copyFileTo (userDataDir().getChildFile ("Presets").getChildFile (f.getFileName()));
    for (auto& f : r.templates) f.copyFileTo (userDataDir().getChildFile ("Templates").getChildFile (f.getFileName()));
    juce::String msg;
    msg << (int) r.items.size() << " item(s) imported";
    if (r.newSongId.isNotEmpty()) msg << ". Song: " << juce::String (r.song.bpm, 2) << " BPM, " << (int) r.song.events.size() << " drum hits";
    if (uncertain) msg << ". " << uncertain << " take(s) uncertain - check them (orange)";
    if (r.luts.size() + r.presets.size() + r.templates.size() > 0) msg << ". " << (r.luts.size() + r.presets.size() + r.templates.size()) << " LUT/preset/template file(s) added";
    status.setText (msg, juce::dontSendNotification);
    if (! r.errors.isEmpty()) showMsg ("Some files could not be imported", r.errors.joinIntoString ("\n"));
    refresh (true);
}

void MainComponent::setSong (const juce::String& id)
{
    auto* m = st.project.findMedia (id); if (m == nullptr) return;
    juce::File f (m->path);
    auto res = std::make_shared<std::pair<SongAnalysis, std::shared_ptr<std::vector<float>>>>();
    auto stereo = std::make_shared<std::vector<float>>();
    runTask ("Analyzing song", [f, res, stereo] (ProgressCb p, std::function<bool()>)
    {
        p (0.1, "Decoding"); juce::String e;
        auto mono = std::make_shared<std::vector<float>>();
        if (! decodeAudio (f, kAnalysisRate, 1, *mono, e)) return;
        decodeAudio (f, kMixRate, 2, *stereo, e);
        p (0.4, "Tempo, beats, drums, sections");
        res->first = analyzeSong (*mono, kAnalysisRate); res->second = mono;
    }, [this, id, res, stereo]
    {
        if (res->second == nullptr) { showMsg ("Song", "Could not decode the audio"); return; }
        st.edit ("Set song");
        st.project.songId = id; st.project.song = res->first; songMono = res->second;
        if (! stereo->empty()) st.audio.put (id, std::make_shared<const std::vector<float>> (std::move (*stereo)));
        placeSongClip();
        status.setText ("Song set: " + juce::String (st.project.song.bpm, 2) + " BPM. Press AUTO SYNC to place your takes.", juce::dontSendNotification);
        refresh (true);
    });
}

static Ticks songZero (const Project& p);

void MainComponent::autoSync (const juce::String& onlyId)
{
    if (songMono == nullptr || songMono->empty()) { showMsg ("Auto Sync", "Import the song first (the first audio file you import becomes the song)."); return; }
    std::vector<std::pair<juce::String, juce::File>> jobs;
    for (auto& m : st.project.media)
        if (m.kind == MediaKind::Video && m.hasAudio && ! m.missing && (onlyId.isEmpty() || m.id == onlyId)) jobs.push_back ({ m.id, juce::File (m.path) });
    if (jobs.empty()) { showMsg ("Auto Sync", "No video takes with audio to sync."); return; }
    auto out = std::make_shared<std::vector<std::pair<juce::String, SyncInfo>>>();
    auto mono = songMono;
    runTask ("Auto Sync", [jobs, out, mono] (ProgressCb p, std::function<bool()> cancel)
    {
        for (size_t i = 0; i < jobs.size() && ! cancel(); ++i)
        {
            p ((double) i / jobs.size(), "Matching " + jobs[i].second.getFileName() + " to the song");
            juce::String e; out->push_back ({ jobs[i].first, syncTake (jobs[i].second, *mono, e) });
        }
    }, [this, out]
    {
        st.edit ("Auto Sync");
        const Ticks zero = songZero (st.project);
        int ok = 0, unsure = 0, bad = 0;
        for (auto& [id, si] : *out) if (auto* m = st.project.findMedia (id))
        {
            if (m->sync.status == SyncInfo::Manual) continue;
            m->sync = si;
            m->sync.offset += zero;
            ok += si.status == SyncInfo::Synced; unsure += si.status == SyncInfo::Uncertain; bad += si.status == SyncInfo::Failed;
        }
        layoutSyncedTakes (st.project);
        status.setText (juce::String (ok) + " synced, " + juce::String (unsure) + " uncertain (orange: check placement), " + juce::String (bad) + " not matched", juce::dontSendNotification);
        refresh (true);
    });
}

static Ticks songZero (const Project& p)
{
    for (auto& c : p.seq().clips) if (c.mediaId == p.songId) return c.start - c.srcIn;
    return 0;
}

void MainComponent::syncClips (const juce::StringArray& ids)
{
    if (songMono == nullptr || songMono->empty()) { showMsg ("Sync to audio", "Import the song first (the first audio file you import becomes the song)."); return; }
    struct Job { juce::String clipId; juce::File file; Ticks srcIn, len; };
    std::vector<Job> jobs;
    int skipped = 0;
    for (auto& id : ids)
        if (auto* c = st.project.seq().findClip (id))
        {
            auto* m = st.project.findMedia (c->mediaId);
            const bool hasSound = m != nullptr && (m->kind == MediaKind::Audio || (m->kind == MediaKind::Video && m->hasAudio));
            if (! hasSound || c->mediaId == st.project.songId || m->missing || c->locked || c->reverse || std::abs (c->speed - 1.0) > 1e-6) { ++skipped; continue; }
            jobs.push_back ({ c->id, juce::File (m->path), c->srcIn, c->length });
        }
    if (jobs.empty()) { showMsg ("Sync to audio", "Pick clips that have their own sound and play at 100% (not reversed or slow-mo)."); return; }
    auto out = std::make_shared<std::vector<std::pair<juce::String, SyncResult>>>();
    auto mono = songMono;
    runTask ("Syncing clips", [jobs, out, mono] (ProgressCb p, std::function<bool()> cancel)
    {
        std::map<juce::String, std::vector<float>> audio;
        for (size_t i = 0; i < jobs.size() && ! cancel(); ++i)
        {
            auto& j = jobs[i];
            p ((double) i / jobs.size(), "Matching clip " + juce::String ((int) i + 1) + " of " + juce::String ((int) jobs.size()));
            auto key = j.file.getFullPathName();
            if (audio.find (key) == audio.end()) { juce::String e; decodeAudio (j.file, kAnalysisRate, 1, audio[key], e); }
            out->push_back ({ j.clipId, syncSegment (*mono, audio[key], kAnalysisRate, j.srcIn, j.len) });
        }
    }, [this, out, skipped]
    {
        st.edit ("Sync clips");
        auto& sq = st.project.seq();
        const Ticks zero = songZero (st.project);
        int ok = 0, weak = 0;
        juce::StringArray moved;
        for (auto& [id, r] : *out)
        {
            auto* c = sq.findClip (id);
            if (c == nullptr) continue;
            const Ticks ns = zero + r.offset + c->srcIn;
            if (! r.ok || r.confidence < 0.25f || ns < 0) { ++weak; continue; }
            c->start = ns; ++ok; moved.add (id);
        }
        // a synced video clip that lands on another clip moves up to a free layer instead of covering it
        for (auto& id : moved)
        {
            auto* c = sq.findClip (id);
            if (c == nullptr || sq.tracks[(size_t) c->track].kind != TrackKind::Video) continue;
            auto collides = [&sq, &id] (int tr, Ticks a, Ticks b)
            {
                for (auto& o : sq.clips) if (o.id != id && o.track == tr && o.start < b && o.end() > a) return true;
                return false;
            };
            if (! collides (c->track, c->start, c->end())) continue;
            int dest = -1;
            for (int t = c->track + 1; t < (int) sq.tracks.size() && dest < 0; ++t)
                if (sq.tracks[(size_t) t].kind == TrackKind::Video && ! sq.tracks[(size_t) t].locked && ! collides (t, c->start, c->end())) dest = t;
            if (dest < 0) { dest = insertVideoTrack (sq); c = sq.findClip (id); }
            c->track = dest;
        }
        sq.sortClips();
        status.setText (juce::String (ok) + " clip" + (ok == 1 ? "" : "s") + " synced to the song"
                        + (weak > 0 ? ", " + juce::String (weak) + " not confidently matched (left in place)" : juce::String())
                        + (skipped > 0 ? ", " + juce::String (skipped) + " skipped (no sound, locked, reversed or speed changed)" : juce::String()),
                        juce::dontSendNotification);
        refresh (true);
    });
}

void MainComponent::sliceAtCuts (const juce::StringArray& ids)
{
    struct Job { juce::String clipId; juce::File file; Ticks a, b; };
    std::vector<Job> jobs;
    for (auto& id : ids)
        if (auto* c = st.project.seq().findClip (id))
        {
            auto* m = st.project.findMedia (c->mediaId);
            if (m == nullptr || m->missing || m->kind != MediaKind::Video || c->locked || c->ramped() || c->text.isNotEmpty()) continue;
            const Ticks a = (Ticks) std::llround (c->srcPos (c->start)), b = (Ticks) std::llround (c->srcPos (c->end()));
            jobs.push_back ({ c->id, juce::File (m->path), std::min (a, b), std::max (a, b) });
        }
    if (jobs.empty()) { showMsg ("Slice at cuts", "Pick video clips (not locked or speed-ramped)."); return; }
    auto out = std::make_shared<std::vector<std::pair<juce::String, std::vector<Ticks>>>>();
    runTask ("Finding cuts", [jobs, out] (ProgressCb prog, std::function<bool()> cancel)
    {
        for (size_t ji = 0; ji < jobs.size() && ! cancel(); ++ji)
        {
            auto& j = jobs[ji];
            const auto label = "Finding cuts in clip " + juce::String ((int) ji + 1) + " of " + juce::String ((int) jobs.size());
            out->push_back ({ j.clipId, detectCuts (j.file, j.a, j.b, cancel, [&] (double x) { prog (((double) ji + x) / (double) jobs.size(), label); }) });
        }
    }, [this, out]
    {
        st.edit ("Slice at cuts");
        auto& S = st.project.seq();
        int pieces = 0;
        for (auto& [id, cuts] : *out)
        {
            auto* c = S.findClip (id);
            if (c == nullptr || cuts.empty()) continue;
            const int track = c->track; const Ticks c0 = c->start, c1 = c->end();
            const double s0 = c->srcPos (c0), s1 = c->srcPos (c1);
            std::vector<Ticks> at;
            for (auto src : cuts)
            {
                const double f = (src - s0) / (s1 - s0);
                const Ticks t = quantizeToFrame (c0 + (Ticks) std::llround (f * (double) (c1 - c0)), st.project.fps);
                if (t > c0 && t < c1) at.push_back (t);
            }
            std::sort (at.begin(), at.end());
            at.erase (std::unique (at.begin(), at.end()), at.end());
            for (auto t : at)
                for (auto& k : S.clips)
                    if (k.track == track && k.start < t && k.end() > t && k.start >= c0 && k.end() <= c1)
                    {
                        if (splitAt (S, t, { k.id })) ++pieces;
                        break;
                    }
        }
        S.sortClips();
        status.setText (pieces > 0 ? juce::String (pieces) + " cuts found and sliced. Every shot is its own clip now."
                                   : juce::String ("No hard cuts found in that clip."), juce::dontSendNotification);
        refresh (true);
    });
}

void MainComponent::stabilizeClips (const juce::StringArray& ids, float smoothSec)
{
    struct Job { juce::String clipId; juce::File file; Ticks srcIn, srcLen; double speed; };
    std::vector<Job> jobs;
    for (auto& id : ids)
        if (auto* c = st.project.seq().findClip (id))
        {
            auto* m = st.project.findMedia (c->mediaId);
            if (m == nullptr || m->missing || m->kind != MediaKind::Video || c->locked || c->reverse || c->ramped() || c->text.isNotEmpty()) continue;
            jobs.push_back ({ c->id, juce::File (m->path), c->srcIn, (Ticks) ((double) c->length * c->speed), c->speed });
        }
    if (jobs.empty()) { showMsg ("Warp Stabilizer", "Pick video clips (not reversed, locked or speed-ramped)."); return; }
    struct Res { juce::String clipId; ShakeFix fix; };
    auto out = std::make_shared<std::vector<Res>>();
    runTask ("Warp Stabilizer", [jobs, out, smoothSec] (ProgressCb prog, std::function<bool()> cancel)
    {
        for (size_t ji = 0; ji < jobs.size() && ! cancel(); ++ji)
        {
            auto& j = jobs[ji];
            Res res; res.clipId = j.clipId;
            const auto label = "Stabilizing clip " + juce::String ((int) ji + 1) + " of " + juce::String ((int) jobs.size());
            if (analyzeShake (j.file, j.srcIn, j.srcLen, j.speed, smoothSec, res.fix, cancel,
                              [&] (double x) { prog (((double) ji + x) / (double) jobs.size(), label); }))
                out->push_back (std::move (res));
        }
    }, [this, out]
    {
        st.edit ("Warp Stabilizer");
        int ok = 0;
        for (auto& r : *out)
        {
            auto* c = st.project.seq().findClip (r.clipId);
            if (c == nullptr) continue;
            const float old = c->fx.get ("stabZoom", 1.0f);
            const float sc = c->fx.get ("scale", 1.0f) / std::max (0.01f, old) * r.fix.zoom;
            c->fx.set ("scale", sc); c->fx.set ("stabZoom", r.fix.zoom); c->fx.set ("stab", 1.0f);
            auto& kx = c->keys["posX"]; auto& ky = c->keys["posY"]; auto& kr = c->keys["rotation"]; kx.clear(); ky.clear(); kr.clear();
            const float baseRot = c->fx.get ("rotation", 0.0f);
            for (size_t i = 0; i < r.fix.t.size(); i += 2)
            {
                const Ticks t = r.fix.t[i];
                if (t < 0 || t > c->length) continue;
                Clip::Key a; a.t = t; a.interp = 0;
                a.v = juce::jlimit (-1.0f, 1.0f, 2.0f * sc * r.fix.dx[i]); kx.push_back (a);
                a.v = juce::jlimit (-1.0f, 1.0f, 2.0f * sc * r.fix.dy[i]); ky.push_back (a);
                a.v = baseRot + r.fix.rot[i]; kr.push_back (a);
            }
            ++ok;
        }
        status.setText (juce::String (ok) + " clip" + (ok == 1 ? "" : "s") + " stabilized (right-click > Warp Stabilizer > Remove to undo)", juce::dontSendNotification);
        refresh (true);
    });
}

// FCP 7 XML (xmeml v4): Premiere Pro, DaVinci Resolve and After Effects (via Premiere import) rebuild the cuts from it
static bool writeFcpXml (const Project& p, const Sequence& s, double fps, int W, int H, const juce::File& out)
{
    const int tb = (int) std::lround (fps);
    const bool ntsc = std::abs (fps - tb) > 0.01;
    auto fr = [&] (Ticks t) { return (juce::int64) std::llround (ticksToSec (t) * fps); };
    auto esc = [] (const juce::String& x) { return x.replace ("&", "&amp;").replace ("<", "&lt;").replace (">", "&gt;").replace ("\"", "&quot;"); };
    const juce::String rate = "<rate><timebase>" + juce::String (tb) + "</timebase><ntsc>" + (ntsc ? "TRUE" : "FALSE") + "</ntsc></rate>";
    std::set<juce::String> written;
    int ci = 0;
    auto clipItem = [&] (const Clip& c, bool audio)
    {
        auto* m = p.findMedia (c.mediaId);
        if (m == nullptr) return juce::String();
        // speed / reverse / ramps: source range actually used, plus Premiere's Time Remap speed (ramps become their average speed)
        const double sA = c.srcPos (c.start), sB = c.srcPos (c.end());
        const juce::int64 durF = std::max<juce::int64> (1, fr (c.end()) - fr (c.start));
        const juce::int64 srcIn = fr ((Ticks) std::min (sA, sB));
        const juce::int64 srcOut = std::max (srcIn + 1, fr ((Ticks) std::max (sA, sB)));
        const double spd = (double) (srcOut - srcIn) / (double) durF * 100.0;
        const bool rev = sB < sA;
        juce::String x;
        x << "<clipitem id=\"ci" << ++ci << "\"><name>" << esc (m->name) << "</name><enabled>" << (c.enabled ? "TRUE" : "FALSE") << "</enabled>"
          << "<duration>" << fr (m->duration) << "</duration>" << rate
          << "<start>" << fr (c.start) << "</start><end>" << fr (c.end()) << "</end>"
          << "<in>" << srcIn << "</in><out>" << srcOut << "</out>";
        if (written.insert (m->id).second)
            x << "<file id=\"f-" << esc (m->id) << "\"><name>" << esc (m->name) << "</name><pathurl>" << esc (juce::URL (juce::File (m->path)).toString (false))
              << "</pathurl>" << rate << "<duration>" << fr (m->duration) << "</duration><media>"
              << (m->kind == MediaKind::Video ? "<video><samplecharacteristics><width>" + juce::String (m->width) + "</width><height>" + juce::String (m->height) + "</height></samplecharacteristics></video>" : juce::String())
              << ((m->kind == MediaKind::Audio || m->hasAudio) ? "<audio><channelcount>2</channelcount></audio>" : "") << "</media></file>";
        else
            x << "<file id=\"f-" << esc (m->id) << "\"/>";
        if (audio) x << "<sourcetrack><mediatype>audio</mediatype><trackindex>1</trackindex></sourcetrack>";
        if (std::abs (spd - 100.0) > 0.5 || rev)
            x << "<filter><effect><name>Time Remap</name><effectid>timeremap</effectid><effectcategory>motion</effectcategory><effecttype>motion</effecttype>"
              << "<mediatype>" << (audio ? "audio" : "video") << "</mediatype>"
              << "<parameter authoringApp=\"PremierePro\"><parameterid>variablespeed</parameterid><name>variablespeed</name><valuemin>0</valuemin><valuemax>1</valuemax><value>0</value></parameter>"
              << "<parameter authoringApp=\"PremierePro\"><parameterid>speed</parameterid><name>speed</name><valuemin>-100000</valuemin><valuemax>100000</valuemax><value>"
              << juce::String (rev ? -spd : spd, 2) << "</value></parameter>"
              << "<parameter authoringApp=\"PremierePro\"><parameterid>reverse</parameterid><name>reverse</name><value>" << (rev ? "TRUE" : "FALSE") << "</value></parameter>"
              << "<parameter authoringApp=\"PremierePro\"><parameterid>frameblending</parameterid><name>frameblending</name><value>" << (c.frameInterp > 0 ? "TRUE" : "FALSE") << "</value></parameter>"
              << "</effect></filter>";
        x << "</clipitem>";
        return x;
    };
    juce::String v, a;
    for (int t = 0; t < (int) s.tracks.size(); ++t)   // track 0 is the bottom layer, as in the XML
    {
        const bool vid = s.tracks[(size_t) t].kind == TrackKind::Video;
        juce::String items;
        for (auto& c : s.clips) if (c.track == t && c.mediaId.isNotEmpty() && c.text.isEmpty()) items << clipItem (c, ! vid);
        (vid ? v : a) << "<track>" << items << "</track>";
    }
    juce::String x;
    x << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE xmeml>\n<xmeml version=\"4\"><sequence id=\"seq1\"><name>" << esc (s.name) << "</name>"
      << "<duration>" << fr (s.duration()) << "</duration>" << rate << "<media><video><format><samplecharacteristics>" << rate
      << "<width>" << W << "</width><height>" << H << "</height></samplecharacteristics></format>" << v << "</video><audio>" << a << "</audio></media></sequence></xmeml>\n";
    return out.replaceWithText (x);
}

void MainComponent::loadMediaCaches()
{
    auto items = st.project.media; auto songId = st.project.songId;
    auto res = std::make_shared<ImportResult>();
    runTask ("Loading media", [items, songId, res] (ProgressCb p, std::function<bool()> cancel)
    {
        for (size_t i = 0; i < items.size() && ! cancel(); ++i)
        {
            auto& m = items[i]; juce::File f (m.path);
            if (m.missing || ! f.existsAsFile()) continue;
            p ((double) i / items.size(), "Loading " + m.name);
            juce::String e;
            if (m.kind == MediaKind::Audio)
            {
                std::vector<float> mono, stv;
                if (decodeAudio (f, kAnalysisRate, 1, mono, e)) res->waves[m.id] = buildWaveform (mono, kAnalysisRate);
                if (decodeAudio (f, kMixRate, 2, stv, e)) res->stereo[m.id] = std::make_shared<const std::vector<float>> (std::move (stv));
                if (m.id == songId) res->songMono = std::make_shared<std::vector<float>> (std::move (mono));
            }
            else
            {
                res->thumbs[m.id] = makeThumbnail (f, m.kind == MediaKind::Video ? secToTicks (std::min (1.0, ticksToSec (m.duration) / 2)) : 0, 72);
                if (m.kind == MediaKind::Video && m.moments.empty() && m.id != songId) res->moments[m.id] = scoreMoments (f, cancel);
                std::vector<float> mono;
                if (m.kind == MediaKind::Video && m.hasAudio && decodeAudio (f, kAnalysisRate, 1, mono, e)) res->waves[m.id] = buildWaveform (mono, kAnalysisRate);
                if (m.kind == MediaKind::Video && std::min (m.width, m.height) > kProxyHeight)
                {
                    auto pf = proxyFileFor (f, kProxyHeight);
                    if (! pf.existsAsFile())
                        makeProxy (f, pf, kProxyHeight, ticksToSec (m.duration), [&] (double x)
                        {
                            p ((i + juce::jlimit (0.0, 1.0, x)) / items.size(), "Optimizing " + m.name + " for smooth playback");
                            return ! cancel();
                        }, e);
                    if (pf.existsAsFile()) res->proxies[m.id] = pf.getFullPathName();
                }
            }
        }
    }, [this, res]
    {
        for (auto& [k, v] : res->proxies) if (auto* mi = st.project.findMedia (k)) mi->proxyPath = v;
        for (auto& [k, v] : res->moments) if (auto* mi = st.project.findMedia (k)) mi->moments = v;
        for (auto& [k, v] : res->thumbs) st.thumbs[k] = v;
        for (auto& [k, v] : res->waves) st.waves[k] = v;
        for (auto& [k, v] : res->stereo) st.audio.put (k, v);
        if (res->songMono) songMono = res->songMono;
        preview.sourcesChanged();
        refresh (true);
    });
}

void MainComponent::relink (const juce::String& id)
{
    chooser = std::make_unique<juce::FileChooser> ("Locate media", juce::File());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, id] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult(); auto* m = st.project.findMedia (id);
        if (! f.existsAsFile() || m == nullptr) return;
        auto pr = probeMedia (f);
        if (! pr.ok) { showMsg ("Relink", pr.error); return; }
        st.edit ("Relink");
        // relink every other missing file found in the same folder by name
        juce::File oldDir = juce::File (m->path).getParentDirectory();
        m->path = f.getFullPathName(); m->missing = false;
        int extra = 0;
        for (auto& o : st.project.media)
            if (o.missing) { auto cand = f.getParentDirectory().getChildFile (juce::File (o.path).getFileName()); if (cand.existsAsFile()) { o.path = cand.getFullPathName(); o.missing = false; ++extra; } }
        juce::ignoreUnused (oldDir);
        if (extra) status.setText ("Relinked " + juce::String (extra + 1) + " files from " + f.getParentDirectory().getFullPathName(), juce::dontSendNotification);
        loadMediaCaches();
    });
}

void MainComponent::removeMedia (const juce::String& id)
{
    st.edit ("Remove media");
    auto& p = st.project;
    for (auto& s : p.sequences) s.clips.erase (std::remove_if (s.clips.begin(), s.clips.end(), [&] (const Clip& c) { return c.mediaId == id; }), s.clips.end());
    p.media.erase (std::remove_if (p.media.begin(), p.media.end(), [&] (const MediaItem& m) { return m.id == id; }), p.media.end());
    if (p.songId == id) { p.songId = {}; p.song = SongAnalysis(); songMono.reset(); }
    st.selected.clear();
    refresh (true);
}

// ============================================================ edit generation
void MainComponent::generate()
{
    int synced = 0;
    for (auto& m : st.project.media) synced += m.sync.status == SyncInfo::Synced || m.sync.status == SyncInfo::Manual;
    if (! st.project.song.valid) { showMsg ("Generate 3 Edits", "Import a song first."); return; }
    if (synced == 0) { showMsg ("Generate 3 Edits", "No synced takes yet. Import your performance takes (with their own audio) and press AUTO SYNC."); return; }
    st.edit ("Generate 3 Edits");
    auto style = styleBox.getSelectedItemIndex() > 0 ? styleModes()[styleBox.getSelectedItemIndex() - 1] : juce::String();
    auto idx = generateEdits (st.project, genSeed++, style);
    if (! idx.empty()) st.project.active = idx.front();
    st.selected.clear();
    status.setText ("Generated " + juce::String ((int) idx.size()) + " editable sequences (pick them in the sequence list). Press again for new variations.", juce::dontSendNotification);
    refresh (true);
}

void MainComponent::chop()
{
    if (! st.project.song.valid) { showMsg ("Chop", "Import a song first."); return; }
    Ticks a = st.rangeB > st.rangeA ? st.rangeA : 0, b = st.rangeB > st.rangeA ? st.rangeB : st.project.song.duration;
    st.edit ("Chop");
    int n = chopRange (st.project, a, b, (ChopMode) chopBox.getSelectedItemIndex(), genSeed++);
    status.setText (n > 0 ? juce::String (n) + " angle switches (" + chopBox.getText() + ")" : juce::String ("No synced takes cover that range"), juce::dontSendNotification);
    if (n <= 0) st.history.undo (st.project);
    refresh (true);
}

// ============================================================ files
void MainComponent::save (bool as, std::function<void()> after)
{
    auto doSave = [this, after] (juce::File f)
    {
        if (f.getFileExtension() != ".isfx") f = f.withFileExtension ("isfx");
        st.project.plugins = rack.toVar();
        trackRacks.store (st.project);
        juce::String err;
        if (! saveProject (st.project, f, err)) { showMsg ("Save failed", err); return; }
        st.file = f; st.dirty = false;
        noteRecentProject (f);
        status.setText ("Saved " + f.getFullPathName(), juce::dontSendNotification);
        refresh (false);
        if (after) after();
    };
    if (! as && st.file != juce::File()) { doSave (st.file); return; }
    chooser = std::make_unique<juce::FileChooser> ("Save project", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (st.project.name + ".isfx"), "*.isfx");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [doSave] (const juce::FileChooser& fc) { if (fc.getResult() != juce::File()) doSave (fc.getResult()); });
}

void MainComponent::openFile (const juce::File& f)
{
    Project p; juce::String err;
    if (! loadProject (f, p, err)) { showMsg ("Open failed", err); return; }
    player.stop(); playBtn.setButtonText ("Play");
    pluginWindows.clear();
    noteRecentProject (f);
    st.project = p; st.file = f; st.dirty = false; st.history.clear(); st.selected.clear(); st.playhead = 0;
    st.thumbs.clear(); st.waves.clear(); songMono.reset();
    juce::StringArray perr; rack.fromVar (p.plugins, perr); rack.prepare (kMixRate, Player::kBlock);
    trackRacks.load (st.project, perr);
    int missing = 0; for (auto& m : p.media) missing += m.missing;
    juce::String msg;
    if (missing) msg << missing << " media file(s) are missing (red). Right-click them in Media > Locate. ";
    if (! perr.isEmpty()) msg << "Plug-ins not loaded: " << perr.joinIntoString (", ");
    if (msg.isNotEmpty()) showMsg ("Project opened with problems", msg);
    preview.sourcesChanged();
    loadMediaCaches();
    refresh (true);
}

void MainComponent::open()
{
    tryQuit ([this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Open project", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.isfx");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) openFile (fc.getResult()); });
    });
}

void MainComponent::newProject()
{
    tryQuit ([this]
    {
        player.stop(); st.project = Project::makeNew(); st.file = juce::File(); st.dirty = false; st.history.clear(); st.selected.clear();
        st.thumbs.clear(); st.waves.clear(); songMono.reset(); st.playhead = 0; preview.sourcesChanged(); refresh (true);
    });
}

void MainComponent::tryQuit (std::function<void()> quit)
{
    if (! st.dirty || selfTest) { quit(); return; }
    juce::AlertWindow::showYesNoCancelBox (juce::MessageBoxIconType::QuestionIcon, "Save changes?", "Save changes to \"" + st.project.name + "\" first?",
        "Save", "Don't Save", "Cancel", this, juce::ModalCallbackFunction::create ([this, quit] (int r)
        {
            if (r == 1) save (false, quit);
            else if (r == 2) quit();
        }));
}

void MainComponent::fileMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "New project  (Ctrl+N)"); m.addItem (2, "Open...  (Ctrl+O)"); m.addItem (3, "Save  (Ctrl+S)"); m.addItem (4, "Save as...  (Ctrl+Shift+S)"); m.addItem (9, "Save page / templates...");
    m.addSeparator(); m.addItem (5, "Import media...  (Ctrl+I)"); m.addItem (6, "Export video...  (Ctrl+E)");
    m.addSeparator(); m.addItem (7, "Audio device settings...");
    m.addItem (8, "Keyboard shortcuts");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&fileBtn), [this] (int r)
    {
        if (r == 1) newProject();
        if (r == 2) open();
        if (r == 3) save (false);
        if (r == 4) save (true);
        if (r == 9) savePage();
        if (r == 5) media.onImport();
        if (r == 6) exportDialog();
        if (r == 7)
        {
            auto* sel = new juce::AudioDeviceSelectorComponent (devices, 0, 0, 2, 2, false, false, true, false);
            sel->setSize (520, 360);
            juce::DialogWindow::LaunchOptions o; o.content.setOwned (sel); o.dialogTitle = "Audio device"; o.dialogBackgroundColour = theme::panel;
            o.useNativeTitleBar = true; o.resizable = false; o.launchAsync();
        }
        if (r == 8)
            showMsg ("Keyboard shortcuts",
                     "E select/move/trim   P paint   D delete   C cut   M/T: drag clips / clip edges\n"
                     "R ripple   S snap   B beat grid   G group/ungroup   L lock\n"
                     "Space play/pause   Left/Right one frame   Shift+Left/Right one beat   Home start\n"
                     "Ctrl/Cmd: Z undo, Shift+Z redo, K split at playhead, S save, O open, I import, E export\n"
                     "Ctrl+wheel or pinch: zoom (down to single frames)   Right-click a clip: Auto Cut / Roll");
    });
}

// ============================================================ presets / templates
void MainComponent::saveTemplateDialog (std::function<void()> after)
{
    askText ("Save as template", st.project.seq().name, [this, after] (juce::String name)
    {
        auto f = userDataDir().getChildFile ("Templates").getChildFile (juce::File::createLegalFileName (name) + ".isfxtemplate");
        f.getParentDirectory().createDirectory();
        juce::String err;
        if (! saveEditTemplate (st.project, st.project.seq(), name, f, err)) showMsg ("Template", err);
        else status.setText ("Saved template \"" + name + "\": cuts are stored in beats, so it re-locks to any song", juce::dontSendNotification);
        if (after) after();
    });
}

void MainComponent::applyTemplateFile (const juce::File& f)
{
    st.edit ("Apply template");
    juce::String err;
    if (! applyEditTemplate (st.project, f, err)) { st.history.undo (st.project); showMsg ("Template", err); }
    else status.setText ("New sequence from template \"" + templateName (f) + "\", locked to " + juce::String (st.project.song.bpm, 1) + " BPM", juce::dontSendNotification);
    refresh (true);
}

namespace
{
struct FileRows : juce::ListBoxModel
{
    juce::Array<juce::File> files;
    std::function<juce::String (const juce::File&)> label;
    std::function<void (int)> activate;
    int getNumRows() override { return files.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override
    {
        if (sel) g.fillAll (theme::accent.withAlpha (0.35f));
        g.setColour (theme::text); g.setFont (14.0f);
        g.drawText (label ? label (files[row]) : files[row].getFileNameWithoutExtension(), 8, 0, w - 16, h, juce::Justification::centredLeft, true);
    }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { if (activate) activate (row); }
    void returnKeyPressed (int row) override { if (activate) activate (row); }
};

class SavePage : public juce::Component
{
public:
    std::function<void()> onSave, onSaveAs, onSaveTemplate, onOpenOther;
    std::function<void (const juce::File&)> onOpen, onApply;
    std::function<void (const juce::File&, std::function<void()>)> onRename;

    SavePage()
    {
        for (auto* l : { &projTitle, &tplTitle, &recentTitle }) { l->setColour (juce::Label::textColourId, theme::text); addAndMakeVisible (l); }
        projTitle.setText ("PROJECT", juce::dontSendNotification);
        recentTitle.setText ("Recent projects (double-click to open)", juce::dontSendNotification);
        tplTitle.setText ("MY TEMPLATES (double-click to use on this song)", juce::dontSendNotification);
        recent.label = [] (const juce::File& f) { return f.getFileNameWithoutExtension() + "   -   " + f.getParentDirectory().getFullPathName(); };
        tpls.label = [] (const juce::File& f) { return templateName (f); };
        recent.activate = [this] (int r) { if (r >= 0 && r < recent.files.size()) { auto f = recent.files[r]; close(); if (onOpen) onOpen (f); } };
        tpls.activate = [this] (int r) { if (r >= 0 && r < tpls.files.size()) { auto f = tpls.files[r]; close(); if (onApply) onApply (f); } };
        recentList.setModel (&recent); tplList.setModel (&tpls);
        for (auto* lb : { &recentList, &tplList })
        { lb->setRowHeight (26); lb->setColour (juce::ListBox::backgroundColourId, theme::bg); lb->setColour (juce::ListBox::outlineColourId, theme::line); lb->setOutlineThickness (1); addAndMakeVisible (lb); }
        for (auto* b : { &saveB, &saveAsB, &saveTplB, &openB, &openRecentB, &applyB, &renameB, &deleteB, &folderB }) addAndMakeVisible (b);
        saveB.onClick = [this] { close(); if (onSave) onSave(); };
        saveAsB.onClick = [this] { close(); if (onSaveAs) onSaveAs(); };
        openB.onClick = [this] { close(); if (onOpenOther) onOpenOther(); };
        saveTplB.onClick = [this] { if (onSaveTemplate) onSaveTemplate(); };
        openRecentB.onClick = [this] { recent.activate (recentList.getSelectedRow()); };
        applyB.onClick = [this] { tpls.activate (tplList.getSelectedRow()); };
        renameB.onClick = [this]
        {
            int r = tplList.getSelectedRow();
            juce::Component::SafePointer<SavePage> sp (this);
            std::function<void()> done = [sp] { if (sp != nullptr) sp->reload(); };
            if (r >= 0 && r < tpls.files.size() && onRename) onRename (tpls.files[r], done);
        };
        deleteB.onClick = [this]
        {
            int r = tplList.getSelectedRow();
            if (r < 0 || r >= tpls.files.size()) return;
            auto f = tpls.files[r];
            juce::Component::SafePointer<SavePage> sp (this);
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon).withTitle ("Delete template")
                                              .withMessage ("Delete \"" + templateName (f) + "\"?").withButton ("Delete").withButton ("Cancel").withAssociatedComponent (this),
                                          [sp, f] (int ok) { if (ok == 1) f.moveToTrash(); if (sp != nullptr) sp->reload(); });
        };
        folderB.onClick = [] { auto d = userDataDir().getChildFile ("Templates"); d.createDirectory(); d.revealToUser(); };
        reload();
        setSize (820, 480);
    }

    void reload()
    {
        recent.files = recentProjects();
        tpls.files.clear(); for (auto& f : listEditTemplates()) tpls.files.add (f);
        recentList.updateContent(); tplList.updateContent(); repaint();
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14);
        auto left = r.removeFromLeft (r.getWidth() / 2 - 7); r.removeFromLeft (14);
        projTitle.setBounds (left.removeFromTop (24));
        auto row = left.removeFromTop (32);
        const int bw = (row.getWidth() - 18) / 4;
        for (auto* b : { &saveB, &saveAsB, &saveTplB, &openB }) { b->setBounds (row.removeFromLeft (bw)); row.removeFromLeft (6); }
        left.removeFromTop (12);
        recentTitle.setBounds (left.removeFromTop (22));
        openRecentB.setBounds (left.removeFromBottom (30).removeFromLeft (140)); left.removeFromBottom (6);
        recentList.setBounds (left);
        tplTitle.setBounds (r.removeFromTop (24));
        auto br = r.removeFromBottom (30); r.removeFromBottom (6);
        const int tw = (br.getWidth() - 18) / 4;
        for (auto* b : { &applyB, &renameB, &deleteB, &folderB }) { b->setBounds (br.removeFromLeft (tw)); br.removeFromLeft (6); }
        tplList.setBounds (r);
    }

private:
    void close()
    {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<juce::DialogWindow> (dw)] { if (sp) sp->exitModalState (0); });
    }
    juce::Label projTitle, recentTitle, tplTitle;
    juce::TextButton saveB { "Save" }, saveAsB { "Save As..." }, saveTplB { "Save as Template..." }, openB { "Open..." }, openRecentB { "Open selected" },
                     applyB { "Use template" }, renameB { "Rename" }, deleteB { "Delete" }, folderB { "Show folder" };
    FileRows recent, tpls;
    juce::ListBox recentList { "recent" }, tplList { "templates" };
};
}

void MainComponent::savePage()
{
    juce::Component::SafePointer<MainComponent> me (this);
    auto* pg = new SavePage();
    pg->onSave = [me] { if (me) me->save (false); };
    pg->onSaveAs = [me] { if (me) me->save (true); };
    pg->onOpenOther = [me] { if (me) me->open(); };
    pg->onOpen = [me] (const juce::File& f) { if (me) me->tryQuit ([me, f] { if (me) me->openFile (f); }); };
    pg->onApply = [me] (const juce::File& f) { if (me) me->applyTemplateFile (f); };
    juce::Component::SafePointer<SavePage> pgp (pg);
    pg->onSaveTemplate = [me, pgp] { if (me) me->saveTemplateDialog ([pgp] { if (pgp) pgp->reload(); }); };
    pg->onRename = [me] (const juce::File& f, std::function<void()> done)
    {
        if (me) me->askText ("Rename template", templateName (f), [me, f, done] (juce::String name)
        {
            juce::String err;
            if (! renameEditTemplate (f, name, err) && me) me->showMsg ("Template", err);
            if (done) done();
        });
    };
    juce::DialogWindow::LaunchOptions o; o.content.setOwned (pg); o.dialogTitle = "Save";
    o.dialogBackgroundColour = theme::panel; o.useNativeTitleBar = true; o.resizable = true;
    o.componentToCentreAround = this; o.launchAsync();
}
void MainComponent::presetsMenu()
{
    auto templates = listEditTemplates();
    juce::PopupMenu apply;
    for (size_t i = 0; i < templates.size(); ++i) apply.addItem (100 + (int) i, templateName (templates[i]));
    if (templates.empty()) apply.addItem (99, "(no templates saved yet)", false);
    Clip* clip = st.selected.size() == 1 ? st.project.seq().findClip (st.selected[0]) : nullptr;
    juce::PopupMenu m;
    m.addItem (1, "Save sequence look as preset...");
    m.addItem (2, "Save selected clip's effects as preset...", clip != nullptr);
    m.addItem (3, "Save this edit as a template...");
    m.addSubMenu ("New sequence from template", apply, ! templates.empty());
    m.addSeparator();
    m.addItem (4, "Import Preset... (.prfpset, .cube, .isfxpreset, .isfxtemplate)");
    m.addItem (5, "Show presets folder");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetsBtn), [this, templates] (int r)
    {
        if (r == 1 || r == 2)
            askText (r == 1 ? "Save look preset" : "Save clip preset", st.project.seq().name + (r == 1 ? " look" : " clip"), [this, r] (juce::String name)
            {
                UserPreset up; up.name = name; up.clip = r == 2;
                if (r == 1) up.fx = st.project.seq().fx;
                else if (auto* c = st.selected.size() == 1 ? st.project.seq().findClip (st.selected[0]) : nullptr) { up.fx = c->fx; up.keys = c->keys; up.length = c->length; }
                juce::File f; juce::String err;
                if (! saveUserPreset (up, f, err)) showMsg ("Preset", err);
                else { status.setText ("Saved preset \"" + name + "\" (it is in the Look list in every project)", juce::dontSendNotification); inspector.rebuild(); }
            });
        if (r == 3) saveTemplateDialog();
        if (r >= 100 && r - 100 < (int) templates.size()) applyTemplateFile (templates[(size_t) (r - 100)]);
        if (r == 4)
        {
            auto dl = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads");
            chooser = std::make_unique<juce::FileChooser> ("Import presets", dl.isDirectory() ? dl : juce::File(), "*.prfpset;*.ffx;*.cube;*.isfxpreset;*.isfxtemplate");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                                  [this] (const juce::FileChooser& fc)
            {
                auto files = fc.getResults(); if (files.isEmpty()) return;
                auto res = std::make_shared<juce::String>(); auto n = std::make_shared<int> (0);
                runTask ("Importing presets", [files, res, n] (ProgressCb, std::function<bool()>) { *res = importPresetFiles (files, *n); },
                         [this, res, n]
                {
                    fxlib.reload();
                    status.setText ("Imported " + juce::String (*n) + " presets. They're in Effects > My Presets: drag one onto a clip.", juce::dontSendNotification);
                    showMsg ("Import Preset", *res);
                });
            });
        }
        if (r == 5) userDataDir().getChildFile ("Presets").revealToUser();
    });
}

// ============================================================ plug-ins
void MainComponent::pluginsMenu()
{
    juce::PopupMenu add;
    auto types = rack.known().getTypes();
    for (int i = 0; i < types.size(); ++i) add.addItem (1000 + i, types[i].name + "  (" + types[i].manufacturerName + ", " + types[i].pluginFormatName + ")");
    if (types.isEmpty()) add.addItem (999, "(scan first)", false);
    juce::PopupMenu m;
    m.addItem (1, "Scan for plug-ins...");
    m.addSubMenu ("Add to master bus", add, ! types.isEmpty());
    if (rack.size() > 0) m.addSeparator();
    for (int i = 0; i < rack.size(); ++i)
    {
        juce::PopupMenu s;
        s.addItem (2000 + i, "Open editor");
        s.addItem (3000 + i, "Bypass", true, rack.bypassed (i));
        s.addItem (4000 + i, "Remove");
        m.addSubMenu (juce::String (i + 1) + ". " + rack.name (i), s);
    }
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&pluginsBtn), [this, types] (int r)
    {
        if (r == 1)
        {
            auto failed = std::make_shared<juce::StringArray>();
            runTask ("Scanning plug-ins", [this, failed] (ProgressCb p, std::function<bool()>)
                     { *failed = rack.scan ([p] (float f, const juce::String& n) { p (f, n); }); },
                     [this, failed]
                     {
                         juce::String msg; msg << rack.known().getNumTypes() << " plug-ins found.";
                         if (! failed->isEmpty()) msg << "\nSkipped (failed to load): " << failed->joinIntoString (", ");
                         showMsg ("Plug-in scan", msg);
                     });
        }
        if (r >= 1000 && r < 2000 && r - 1000 < types.size())
        {
            juce::String err;
            if (! rack.add (types[r - 1000], err)) showMsg ("Plug-in", types[r - 1000].name + " could not be loaded: " + err);
            else { st.dirty = true; if (auto* inst = rack.instance (rack.size() - 1)) pluginWindows.push_back (std::make_unique<PluginWindow> (*inst)); }
        }
        if (r >= 2000 && r < 3000) if (auto* inst = rack.instance (r - 2000)) pluginWindows.push_back (std::make_unique<PluginWindow> (*inst));
        if (r >= 3000 && r < 4000) { rack.setBypass (r - 3000, ! rack.bypassed (r - 3000)); st.dirty = true; }
        if (r >= 4000 && r < 5000) { pluginWindows.clear(); rack.remove (r - 4000); st.dirty = true; }
        st.project.plugins = rack.toVar();
    });
}

void MainComponent::trackPluginsMenu (int tr)
{
    auto& seq = st.project.seq();
    if (tr < 0 || tr >= (int) seq.tracks.size()) return;
    if (seq.tracks[(size_t) tr].id.isEmpty()) seq.tracks[(size_t) tr].id = newId();
    const juce::String id = seq.tracks[(size_t) tr].id;
    auto& r = trackRacks.get (id);
    juce::PopupMenu add;
    auto types = rack.known().getTypes();
    for (int i = 0; i < types.size(); ++i) add.addItem (1000 + i, types[i].name + "  (" + types[i].manufacturerName + ", " + types[i].pluginFormatName + ")");
    juce::PopupMenu m;
    m.addSectionHeader ("Track plug-ins: " + (seq.tracks[(size_t) tr].name.isNotEmpty() ? seq.tracks[(size_t) tr].name : "Track " + juce::String (tr + 1)));
    if (types.isEmpty()) m.addItem (999, "No plug-ins yet: use Plug-ins > Scan first", false);
    else m.addSubMenu ("Add VST3 / AU effect", add);
    for (int i = 0; i < r.size(); ++i)
    {
        juce::PopupMenu s;
        s.addItem (2000 + i, "Open editor");
        s.addItem (3000 + i, "Bypass", true, r.bypassed (i));
        s.addItem (4000 + i, "Remove");
        m.addSubMenu (juce::String (i + 1) + ". " + r.name (i) + (r.bypassed (i) ? "  (bypassed)" : ""), s);
    }
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [this, id, types] (int res)
    {
        auto& rk = trackRacks.get (id);
        if (res >= 1000 && res < 2000 && res - 1000 < types.size())
        {
            juce::String err;
            if (! rk.add (types[res - 1000], err)) showMsg ("Plug-in", types[res - 1000].name + " could not be loaded: " + err);
            else if (auto* inst = rk.instance (rk.size() - 1)) pluginWindows.push_back (std::make_unique<PluginWindow> (*inst));
        }
        if (res >= 2000 && res < 3000) if (auto* inst = rk.instance (res - 2000)) pluginWindows.push_back (std::make_unique<PluginWindow> (*inst));
        if (res >= 3000 && res < 4000) rk.setBypass (res - 3000, ! rk.bypassed (res - 3000));
        if (res >= 4000 && res < 5000) { pluginWindows.clear(); rk.remove (res - 4000); }
        if (res > 0) { st.dirty = true; trackRacks.store (st.project); refresh (false); }
    });
}

// ============================================================ export
bool MainComponent::exportSequence (const Project& p, int seqIndex, const ExportJob& jobIn, ProgressCb progress, std::function<bool()> cancelled, juce::String& err)
{
    auto& s = p.sequences[(size_t) seqIndex];
    Renderer r;
    std::atomic<bool> cancel { false };
    ExportJob job = jobIn;
    rack.reset();
    job.audioFx = [this] (float* L, float* R, int n) { rack.process (L, R, n); };
    if (job.soundtrack.existsAsFile())   // EXPORT ALL: the host's Record mix replaces the sequence's own sound
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::shared_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (job.soundtrack));
        auto pos = std::make_shared<juce::int64> ((juce::int64) std::llround (ticksToSec (job.rangeStart) * (rd ? rd->sampleRate : kMixRate)));
        job.audioFx = [rd, pos] (float* L, float* R, int n)
        {
            std::fill (L, L + n, 0.0f); std::fill (R, R + n, 0.0f);
            if (rd == nullptr) return;
            float* ch[2] { L, R };
            rd->read (ch, rd->numChannels > 1 ? 2 : 1, *pos, n);
            if (rd->numChannels < 2) std::copy (L, L + n, R);
            *pos += n;
        };
    }
    trackRacks.reset();
    job.trackFx = &trackRacks;
    juce::String codec;
    bool ok = runExport (s, st.audio, job, [&] (Ticks t, std::vector<juce::uint8>& px)
    {
        if (cancelled && cancelled()) cancel = true;
        return r.render (p, s, t, job.width, job.height, px, job.height);
    }, [&] (double f) { if (progress) progress (f, s.name + ": " + juce::String ((int) (f * 100)) + "%"); }, cancel, err, &codec);
    return ok;
}

void MainComponent::exportDialog()
{
    if (st.project.seq().clips.empty()) { showMsg ("Export", "The sequence is empty."); return; }
    player.stop(); playBtn.setButtonText ("Play");
    dialog = std::make_unique<juce::AlertWindow> ("Export video", "Every frame is rendered at full quality (no proxies, no dropped frames). MOV formats keep the audio uncompressed (24-bit PCM).", juce::MessageBoxIconType::NoIcon);
    static const char* allCodecs[] = { "h264", "h265", "h265_lossless", "h264_lossless", "prores_hq", "prores_4444" };
    static const char* allNames[] = { "H.264 MP4 (AAC, for posting)", "H.265 / HEVC 10-bit, near lossless (MOV, PCM)", "H.265 / HEVC lossless (MOV, PCM)",
                                      "H.264 lossless (MOV, PCM)", "ProRes 422 HQ (MOV, PCM)", "ProRes 4444 (MOV, PCM)" };
    auto codecs = std::make_shared<juce::StringArray>(); juce::StringArray fmtNames;
    for (int i = 0; i < 6; ++i) if (exportCodecAvailable (allCodecs[i])) { codecs->add (allCodecs[i]); fmtNames.add (allNames[i]); }
    dialog->addComboBox ("fmt", fmtNames, "Format");
    dialog->addComboBox ("res", { "720p", "1080p", "1440p", "4K (2160p)", "Source (largest clip, no resize)" }, "Resolution (short side)");
    dialog->addComboBox ("fps", { "23.976", "24", "25", "29.97", "30", "50", "59.94", "60" }, "Frame rate");
    dialog->addComboBox ("q", { "Standard (12 Mbps per 1080p)", "High (20 Mbps per 1080p)", "Master (40 Mbps per 1080p)" }, "Quality");
    dialog->addComboBox ("range", { "Whole sequence", "Selected range" }, "Range");
    dialog->addComboBox ("which", { "This sequence (one full video)", "All sequences (one file each)", "Each cut as its own file" }, "Versions");
    dialog->addComboBox ("xml", { "Video only", "Video + timeline for Premiere / DaVinci / After Effects (FCP XML)", "Timeline only (FCP XML, links your original media)" }, "Editor hand-off");
    dialog->getComboBoxComponent ("res")->setSelectedItemIndex (1);
    dialog->getComboBoxComponent ("fps")->setSelectedItemIndex (juce::jlimit (0, 7, st.project.fps > 59 ? 7 : st.project.fps > 29 ? 4 : st.project.fps > 24.5 ? 2 : 1));
    dialog->getComboBoxComponent ("q")->setSelectedItemIndex (1);
    dialog->getComboBoxComponent ("range")->setSelectedItemIndex (st.rangeB > st.rangeA ? 1 : 0);
    dialog->addButton ("Export...", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    dialog->enterModalState (true, juce::ModalCallbackFunction::create ([this, codecs] (int r)
    {
        const int res[] = { 720, 1080, 1440, 2160 };
        const double fpsv[] = { 24000.0 / 1001, 24, 25, 30000.0 / 1001, 30, 50, 60000.0 / 1001, 60 };
        const int mb[] = { 12, 20, 40 };
        const int ri = dialog->getComboBoxComponent ("res")->getSelectedItemIndex();
        int shortSide = ri < 4 ? res[ri] : 0;
        if (ri >= 4) for (auto& m : st.project.media) if (m.kind != MediaKind::Audio && ! m.missing) shortSide = std::max (shortSide, std::min (m.width, m.height));
        if (shortSide <= 0) shortSide = 1080;
        const juce::String codec = (*codecs)[juce::jmax (0, dialog->getComboBoxComponent ("fmt")->getSelectedItemIndex())];
        double fps = fpsv[dialog->getComboBoxComponent ("fps")->getSelectedItemIndex()];
        int q = dialog->getComboBoxComponent ("q")->getSelectedItemIndex();
        bool range = dialog->getComboBoxComponent ("range")->getSelectedItemIndex() == 1 && st.rangeB > st.rangeA;
        const int which = dialog->getComboBoxComponent ("which")->getSelectedItemIndex();
        bool all = which == 1; const bool cuts = which == 2;
        const int xml = dialog->getComboBoxComponent ("xml")->getSelectedItemIndex();
        juce::MessageManager::callAsync ([this] { dialog.reset(); });
        const auto track = std::exchange (soundtrack, juce::File());
        if (r != 1) return;
        ExportJob job;
        job.soundtrack = track;
        outputSize (st.project, shortSide, job.width, job.height);
        job.width &= ~1; job.height &= ~1; job.fps = fps;
        job.mbps = (int) std::lround (mb[q] * (double) job.width * job.height / (1920.0 * 1080.0) * fps / 30.0);
        if (range) { job.rangeStart = st.rangeA; job.rangeEnd = st.rangeB; }
        job.codec = codec;
        const juce::String ext = codec == "h264" ? "mp4" : "mov";
        chooser = std::make_unique<juce::FileChooser> ("Export to", juce::File::getSpecialLocation (juce::File::userMoviesDirectory)
                                                           .getChildFile (juce::File::createLegalFileName (st.project.name + " - " + st.project.seq().name) + "." + ext), "*." + ext);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this, job, all, ext, cuts, xml] (const juce::FileChooser& fc)
        {
            auto out = fc.getResult(); if (out == juce::File()) return;
            out = out.withFileExtension (ext);
            auto p = std::make_shared<Project> (st.project);
            auto err = std::make_shared<juce::String>(); auto done = std::make_shared<juce::StringArray>();
            runTask ("Exporting", [this, p, job, all, out, err, done, cuts, xml] (ProgressCb prog, std::function<bool()> cancel)
            {
                if (xml > 0)
                    for (int i = 0; i < (int) p->sequences.size(); ++i)
                    {
                        if (! all && i != p->active) continue;
                        auto xf = all ? out.getSiblingFile (out.getFileNameWithoutExtension() + " - " + juce::File::createLegalFileName (p->sequences[(size_t) i].name) + ".xml")
                                      : out.withFileExtension ("xml");
                        if (writeFcpXml (*p, p->sequences[(size_t) i], job.fps, job.width, job.height, xf)) done->add (xf.getFullPathName());
                    }
                if (xml == 2) return;
                if (cuts)
                {
                    const auto& sq = p->sequences[(size_t) p->active];
                    std::vector<const Clip*> cl;
                    for (auto& c : sq.clips)
                        if (c.track >= 0 && c.track < (int) sq.tracks.size() && sq.tracks[(size_t) c.track].kind == TrackKind::Video && c.enabled
                            && c.mediaId.isNotEmpty() && c.mediaId != p->songId && c.text.isEmpty()) cl.push_back (&c);
                    std::stable_sort (cl.begin(), cl.end(), [] (const Clip* x, const Clip* y) { return x->start < y->start; });
                    for (size_t k = 0; k < cl.size() && ! cancel(); ++k)
                    {
                        ExportJob j = job;
                        j.rangeStart = cl[k]->start; j.rangeEnd = cl[k]->end();
                        j.output = out.getSiblingFile (out.getFileNameWithoutExtension() + " - " + juce::String ((int) k + 1).paddedLeft ('0', 3) + out.getFileExtension());
                        if (! exportSequence (*p, p->active, j, prog, cancel, *err)) return;
                        done->add (j.output.getFullPathName());
                    }
                    return;
                }
                for (int i = 0; i < (int) p->sequences.size(); ++i)
                {
                    if (! all && i != p->active) continue;
                    ExportJob j = job;
                    j.output = all ? out.getSiblingFile (out.getFileNameWithoutExtension() + " - " + juce::File::createLegalFileName (p->sequences[(size_t) i].name) + out.getFileExtension()) : out;
                    if (! exportSequence (*p, i, j, prog, cancel, *err)) return;
                    done->add (j.output.getFullPathName());
                    if (cancel()) return;
                }
            }, [this, err, done]
            {
                if (err->isNotEmpty()) showMsg ("Export failed", *err);
                else if (! done->isEmpty()) { status.setText ("Exported " + done->joinIntoString (", "), juce::dontSendNotification); juce::File ((*done)[0]).revealToUser(); }
            });
        });
    }), false);
}

// ============================================================ self test (CI / --selftest)
int MainComponent::runSelfTest (const juce::File& outDir)
{
    selfTest = true;
    outDir.createDirectory();
    int fails = 0;
    auto logF = outDir.getChildFile ("selftest.log"); logF.replaceWithText ({});
    auto check = [&] (bool ok, const juce::String& what)
    {
        juce::String line = (ok ? "PASS " : "FAIL ") + what;
        std::cout << line << std::endl;
        logF.appendText (line + "\n");
        fails += ok ? 0 : 1;
    };
    // 1. synthetic 20 s song at 120 BPM with kick / snare / hats and a varying melody (so sync has a unique answer)
    const int sr = kMixRate; const double dur = 20;
    std::vector<float> song ((size_t) (dur * sr));
    juce::Random rng (7);
    for (int b = 0; b < (int) (dur * 2); ++b)
    {
        size_t at = (size_t) (b * 0.5 * sr);
        for (size_t i = 0; i < (size_t) (0.15 * sr) && at + i < song.size(); ++i)
            song[at + i] += 0.8f * std::sin (2 * juce::MathConstants<float>::pi * (55.0f + 60 * std::exp (-(float) i / 900)) * (float) i / sr) * std::exp (-(float) i / 4000.0f);
        if (b % 2 == 1) for (size_t i = 0; i < (size_t) (0.12 * sr) && at + i < song.size(); ++i) song[at + i] += 0.4f * (rng.nextFloat() * 2 - 1) * std::exp (-(float) i / 2500.0f);
        float note = 220.0f * std::pow (2.0f, (float) rng.nextInt (12) / 12.0f);
        for (size_t i = 0; i < (size_t) (0.5 * sr) && at + i < song.size(); ++i) song[at + i] += 0.12f * std::sin (2 * juce::MathConstants<float>::pi * note * (float) i / sr);
    }
    // a non-repeating lead line every 1/8 (random pitch, length and level) so the song has one unique alignment
    for (int n = 0; n < (int) (dur * 4); ++n)
    {
        if (rng.nextFloat() < 0.3f) continue;
        size_t at = (size_t) ((n * 0.25 + rng.nextFloat() * 0.05) * sr);
        float note = 330.0f * std::pow (2.0f, (float) rng.nextInt (24) / 12.0f), amp = 0.1f + 0.25f * rng.nextFloat();
        size_t len = (size_t) ((0.05 + 0.2 * rng.nextFloat()) * sr);
        for (size_t i = 0; i < len && at + i < song.size(); ++i)
            song[at + i] += amp * std::sin (2 * juce::MathConstants<float>::pi * note * (float) i / sr) * (1.0f - (float) i / (float) len);
    }
    for (int h = 0; h < (int) (dur * 4); ++h)
    {
        size_t at = (size_t) (h * 0.25 * sr);
        for (size_t i = 0; i < (size_t) (0.03 * sr) && at + i < song.size(); ++i) song[at + i] += 0.15f * (rng.nextFloat() * 2 - 1) * std::exp (-(float) i / 300.0f);
    }
    auto songF = outDir.getChildFile ("selftest_song.wav"); songF.deleteFile();
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (songF), sr, 1, 16, {}, 0));
        juce::AudioBuffer<float> buf (1, (int) song.size()); buf.copyFrom (0, 0, song.data(), (int) song.size());
        check (w != nullptr && w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples()), "write synthetic song");
    }
    // 2. two performance takes recorded from 3 s and 7 s into the song
    juce::Array<juce::File> files { songF };
    for (int k = 0; k < 2; ++k)
    {
        double off = k == 0 ? 3.0 : 7.0, len = 9.0;
        auto f = outDir.getChildFile ("selftest_take" + juce::String (k + 1) + ".mp4"); f.deleteFile();
        VideoEncoder enc; juce::String err;
        bool ok = enc.open (f, 320, 180, 30, 2, true, err);
        std::vector<juce::uint8> px (320 * 180 * 4);
        for (int fr = 0; ok && fr < (int) (len * 30); ++fr)
        {
            for (int y = 0; y < 180; ++y) for (int x = 0; x < 320; ++x)
            {
                auto* p = &px[((size_t) y * 320 + x) * 4];
                bool bar = std::abs (x - (fr * 4) % 320) < 12;
                p[0] = (juce::uint8) (k == 0 ? 200 : 40 + y / 2); p[1] = (juce::uint8) (bar ? 255 : 60 + x / 4); p[2] = (juce::uint8) (k == 0 ? 60 : 210); p[3] = 255;
            }
            ok = enc.writeVideo (px.data(), fr);
        }
        std::vector<float> L ((size_t) (len * sr)), R;
        for (size_t i = 0; i < L.size(); ++i) L[i] = 0.7f * song[(size_t) (off * sr) + i] + 0.02f * (rng.nextFloat() * 2 - 1);
        R = L;
        ok = ok && enc.writeAudio (L.data(), R.data(), (int) L.size()) && enc.finish (err);
        check (ok, "write synthetic take " + juce::String (k + 1) + " " + err);
        files.add (f);
    }
    // 3. the real import path: probe, analysis, auto-sync
    importFiles (files);
    check (st.project.song.valid && std::abs (st.project.song.bpm - 120) < 0.6, "song analyzed at " + juce::String (st.project.song.bpm, 2) + " BPM");
    int synced = 0;
    for (auto& m : st.project.media)
        if (m.kind == MediaKind::Video)
        {
            double expect = m.name.contains ("take1") ? 3.0 : 7.0;
            bool ok = m.sync.status == SyncInfo::Synced && std::abs (ticksToSec (m.sync.offset) - expect) < 0.03;
            synced += ok;
            check (ok, m.name + " synced at " + juce::String (ticksToSec (m.sync.offset), 3) + " s (conf " + juce::String (m.sync.confidence, 2) + ")");
        }
    // 4. generate, auto cut, look
    generate();
    check (st.project.sequences.size() >= 4, "generated 3 editable sequences (" + juce::String ((int) st.project.sequences.size()) + " total)");
    for (auto& l : lookLibrary()) if (l.name == "Night Teal Flip") applyLook (st.project.seq().fx, l, 1.0f);
    juce::String vid;
    for (auto& c : st.project.seq().clips) if (st.project.seq().tracks[(size_t) c.track].kind == TrackKind::Video && ! c.locked) { vid = c.id; break; }
    check (vid.isNotEmpty() && autoCutClip (st.project, vid, AutoCut::Roll, false) > 1, "right-click Auto Cut roll on a clip");
    refresh (true);
    // 5. preview frame through the real UI + window snapshot
    st.playhead = secToTicks (8);
    {
        Renderer r; std::vector<juce::uint8> px; int W, H; outputSize (st.project, 360, W, H);
        bool ok = r.render (st.project, st.project.seq(), st.playhead, W, H, px, 360);
        double sum = 0; for (size_t i = 0; i < px.size(); i += 4) sum += px[i] + px[i + 1] + px[i + 2];
        check (ok && sum / (px.size() / 4) > 30, "preview frame renders synced footage (mean " + juce::String (sum / (px.size() / 4) / 3, 1) + ")");
        int PW, PH; outputSize (st.project, 540, PW, PH);
        auto b0 = juce::Time::getMillisecondCounterHiRes();
        for (int k = 0; k < 24; ++k) r.render (st.project, st.project.seq(), st.playhead + secToTicks (k / 24.0), PW, PH, px, 540);
        double ms = (juce::Time::getMillisecondCounterHiRes() - b0) / 24.0;
        check (ms < 1000, "preview playback render " + juce::String (PW) + "x" + juce::String (PH) + " " + juce::String (ms, 1) + " ms/frame");
    }
    preview.requestFrame (st.playhead);
    juce::Thread::sleep (800);
    auto snap = getParentComponent() != nullptr ? getParentComponent()->createComponentSnapshot (getParentComponent()->getLocalBounds()) : createComponentSnapshot (getLocalBounds());
    {
        auto f = outDir.getChildFile ("selftest_window.png"); f.deleteFile();
        juce::FileOutputStream os (f); juce::PNGImageFormat png;
        check (png.writeImageToStream (snap, os), "window snapshot " + juce::String (snap.getWidth()) + "x" + juce::String (snap.getHeight()));
    }
    // 6. export 4 s at 9:16 through the same renderer
    st.project.aspectW = 9; st.project.aspectH = 16;
    ExportJob job; outputSize (st.project, 360, job.width, job.height); job.width &= ~1; job.height &= ~1;
    job.fps = 30; job.mbps = 3; job.rangeStart = secToTicks (4); job.rangeEnd = secToTicks (8);
    job.output = outDir.getChildFile ("selftest.mp4"); job.output.deleteFile();
    juce::String err;
    bool ok = exportSequence (st.project, st.project.active, job, nullptr, [] { return false; }, err);
    auto pr = probeMedia (job.output);
    check (ok && pr.ok && pr.hasAudio && pr.width == job.width && std::abs (ticksToSec (pr.duration) - 4.0) < 0.15,
           "export 9:16 MP4 " + juce::String (pr.width) + "x" + juce::String (pr.height) + " " + juce::String (ticksToSec (pr.duration), 2) + " s " + err);
    // 7. save / reopen
    auto pf = outDir.getChildFile ("selftest.isfx");
    check (saveProject (st.project, pf, err), "save project");
    Project back; check (loadProject (pf, back, err) && back.sequences.size() == st.project.sequences.size(), "reopen project");
    st.dirty = false;
    std::cout << (fails == 0 ? "SELFTEST PASSED" : "SELFTEST FAILED") << std::endl;
    return fails == 0 ? 0 : 1;
}
}
