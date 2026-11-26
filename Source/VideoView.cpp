#include "UI.h"
#if JUCE_MAC || JUCE_WINDOWS || JUCE_IOS
 #include <juce_video/juce_video.h>
 #define HHS_HAS_VIDEO 1
#else
 #define HHS_HAS_VIDEO 0
#endif

#if HHS_WITH_ISFX
#include "ISFX/MainComponent.h"
namespace hhs
{
namespace
{
// VIDEO tab = the full InSequence FX editor (looks, FX, transitions, AUTO SYNC, GENERATE, export) in HumHouse colors.
// It plays through HumHouse's own audio device as one more callback; decoding/pre-roll run on its prefetch threads,
// so nothing waits on the audio thread or blocks the UI. POP OUT detaches it into its own window.
class EditorView : public juce::Component, private juce::Timer
{
public:
    explicit EditorView (Studio& st) : s (st)
    {
        ed = std::make_unique<isfx::MainComponent> (&s.engine.deviceManager);
        addAndMakeVisible (*ed);
        ed->onTrackLink = [this] (int tr) { linkMenu (tr); };
        ed->routedOnHost = [this] (const juce::String& key)
        {
            return std::any_of (s.p().clips.begin(), s.p().clips.end(), [&] (const Clip& c) { return isLinkClip (c, key); });
        };
        setLookAndFeel (&isfx::sharedLookAndFeel());   // same buttons/fonts/colours as the InSequence FX app
        for (auto* b : { &push, &exportAll, &pop, &follow, &toRecord, &toMixer }) addAndMakeVisible (b);
        push.setTooltip ("PUSH the whole VIDEO sequence or the selected clips onto the Record timeline as editable clips; the video pops out and follows the playhead");
        push.onClick = [this] { pushMenu(); };
        exportAll.setTooltip ("EXPORT ALL: one video file with the picture and the whole Record timeline's mix (every clip, track, mixer and FX) as its sound");
        exportAll.onClick = [this] { exportAllWithRecord(); };
        videoExportAllHook() = [this] { exportAllWithRecord(); };
        addAndMakeVisible (info);
        follow.setClickingTogglesState (true); follow.setToggleState (true, juce::dontSendNotification);
        follow.setTooltip ("The video plays and scrubs with HumHouse's playhead (Beat / Record / Mix)");
        pop.setTooltip ("Detach the video editor into its own window (click again to dock it)");
        toRecord.setTooltip ("Put the video's song on a new Record track as a normal clip you can move and edit");
        info.setColour (juce::Label::textColourId, isfx::theme::dim);
        info.setMinimumHorizontalScale (0.5f);
        info.setText ("Import a song and takes, AUTO SYNC, GENERATE 3 EDITS - FOLLOW keeps it locked to the playhead", juce::dontSendNotification);
        pop.onClick = [this] { popped() ? dock() : popOut(); };
        videoPopHook() = [this] { if (! popped()) popOut(); else if (win != nullptr) win->toFront (true); };
        toRecord.onClick = [this] { songToRecord(); };
        toMixer.setTooltip ("Every VIDEO track with sound becomes its own Record clip, routed to a new mixer insert or the insert number you pick");
        toMixer.onClick = [this] { tracksMenu(); };
        startTimerHz (30);
    }
    ~EditorView() override { videoPopHook() = nullptr; videoExportAllHook() = nullptr; stopTimer(); setLookAndFeel (nullptr); if (win) win->clearContentComponent(); win.reset(); ed.reset(); }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (isfx::theme::bg);
        g.setGradientFill (juce::ColourGradient (juce::Colours::black, 0, 0, isfx::theme::panel, 0, (float) bar, false)); g.fillRect (0, 0, getWidth(), bar);
        if (popped()) { g.setColour (isfx::theme::dim); g.setFont (juce::FontOptions (15.0f)); g.drawText ("Video editor is popped out - click DOCK to bring it back", getLocalBounds().withTrimmedTop (bar), juce::Justification::centred); }
    }
    void resized() override
    {
        auto r = getLocalBounds(); auto b = r.removeFromTop (bar).reduced (4);
        push.setBounds (b.removeFromLeft (80)); b.removeFromLeft (4);
        exportAll.setBounds (b.removeFromLeft (110)); b.removeFromLeft (4);
        pop.setBounds (b.removeFromLeft (100)); b.removeFromLeft (4);
        follow.setBounds (b.removeFromLeft (96)); b.removeFromLeft (4);
        toRecord.setBounds (b.removeFromLeft (170)); b.removeFromLeft (4);
        toMixer.setBounds (b.removeFromLeft (180)); b.removeFromLeft (8);
        info.setBounds (b);
        if (! popped()) ed->setBounds (r);
    }
private:
    struct Win : juce::DocumentWindow
    {
        std::function<void()> onClose;
        Win() : juce::DocumentWindow ("HumHouse VIDEO", isfx::theme::bg, juce::DocumentWindow::allButtons) {}
        void closeButtonPressed() override { if (onClose) onClose(); }
    };
    bool popped() const { return win != nullptr; }
    void popOut()
    {
        removeChildComponent (ed.get());
        win = std::make_unique<Win>();
        win->setUsingNativeTitleBar (true);
        useHumHouseTitleBar (*win, juce::DocumentWindow::allButtons);
        win->setContentNonOwned (ed.get(), false);
        win->setResizable (true, true);
        win->centreWithSize (juce::jmax (1180, getWidth()), juce::jmax (720, getHeight()));
        win->onClose = [safe = juce::Component::SafePointer<EditorView> (this)] { juce::MessageManager::callAsync ([safe] { if (safe) safe->dock(); }); };
        win->setVisible (true);
        pop.setButtonText ("DOCK"); repaint();
    }
    void dock()
    {
        if (win) { win->clearContentComponent(); win.reset(); }
        addAndMakeVisible (*ed); pop.setButtonText ("POP OUT"); resized(); repaint();
    }
    void songToRecord()
    {
        const auto f = ed->songFile();
        if (! f.existsAsFile()) { info.setText ("Import a song in the editor first", juce::dontSendNotification); return; }
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
        if (rd == nullptr || rd->sampleRate <= 0) { info.setText ("Can't read that song's audio here; drag the WAV onto the Record timeline", juce::dontSendNotification); return; }
        const double secs = (double) rd->lengthInSamples / rd->sampleRate;
        s.edit ("Video song to Record", [&] (Project& p) {
            Clip c; c.uid = p.newUid(); c.audioFile = f; c.patternIndex = -1;
            c.start = 0.0; c.length = secondsToBeats (secs, p.bpm);
            c.lane = p.pushLane (0, c.start, c.start + c.length, f.getFileNameWithoutExtension() + " (video)");
            p.clips.push_back (c);
        });
        info.setText ("Song is on the Record timeline - edit it there, the video follows the playhead", juce::dontSendNotification);
    }
    void pushMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, "PUSH whole sequence to REC (sound + picture)");
        m.addItem (2, "PUSH selected clips to REC", ed->hasSelection());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&push),
            [safe = juce::Component::SafePointer<EditorView> (this)] (int r) { if (safe && r > 0) safe->pushToRecord (r == 2); });
    }
    // every VIDEO clip with sound lands on Record at its own spot, one lane + mixer insert per VIDEO track;
    // the picture pops out and follows the Record playhead so you edit to it
    void pushToRecord (bool selectedOnly)
    {
        const auto dir = hhsHome().getChildFile ("Video Push").getChildFile (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S"));
        const auto cl = ed->renderClips (dir, selectedOnly);
        if (cl.empty()) { info.setText (selectedOnly ? "None of the selected clips has sound" : "No VIDEO clip has sound yet - import audio or video with sound first", juce::dontSendNotification); return; }
        s.edit (selectedOnly ? "PUSH clips to REC" : "PUSH sequence to REC", [&] (Project& p) {
            // video tracks V1, V2... land on Track 1, 2... going down (the first tracks free over their time)
            std::map<int, std::pair<double, double>> span;
            for (auto& c : cl)
            {
                const double a = secondsToBeats (c.start, p.bpm), b = a + secondsToBeats (c.seconds, p.bpm);
                auto it = span.find (c.track);
                if (it == span.end()) span[c.track] = { a, b };
                else it->second = { juce::jmin (it->second.first, a), juce::jmax (it->second.second, b) };
            }
            std::map<int, int> laneOf;
            int next = 0;
            for (auto& [trk, ab] : span) { laneOf[trk] = p.pushLane (next, ab.first, ab.second, "VIDEO V" + juce::String (trk + 1)); next = laneOf[trk] + 1; }
            for (auto& c : cl)
            {
                Clip k; k.uid = p.newUid(); k.lane = laneOf[c.track]; k.audioFile = c.file; k.patternIndex = -1;
                k.start = secondsToBeats (c.start, p.bpm); k.length = secondsToBeats (c.seconds, p.bpm);
                p.clips.push_back (k);
            }
        });
        follow.setToggleState (true, juce::dontSendNotification);
        if (! popped()) popOut();
        info.setText (juce::String ((int) cl.size()) + " clip(s) pushed to REC - edit them there, the popped-out video follows the playhead", juce::dontSendNotification);
    }
    juce::uint32 linkPollMs = 0;
    std::map<juce::String, juce::String> linkSig;   // routeKey -> the track signature its REC clip was rendered from
    std::map<juce::String, int> linkIns;            // routeKey -> VIDEO's insert at the last render (REC-side changes win until VIDEO's changes)
    std::map<juce::String, int> linkGen;            // routeKey -> newest render; older renders finishing late are dropped
    static juce::File linkDir() { return hhsHome().getChildFile ("Video Tracks").getChildFile ("Routed"); }
    static juce::String linkPrefix (const juce::String& key) { return "V-" + key + " "; }
    static bool isLinkClip (const Clip& c, const juce::String& key)
    { return key.isNotEmpty() && c.audioFile.getParentDirectory() == linkDir() && c.audioFile.getFileName().startsWith (linkPrefix (key)); }
    int trackOfKey (const juce::String& key) const
    {
        const auto& tr = ed->sequence().tracks;
        for (int t = 0; t < (int) tr.size(); ++t) if (tr[(size_t) t].routeKey == key) return t;
        return -1;
    }
    // right-click a VIDEO audio track: route it to any REC track (default: the same number going down) and that track's insert
    void linkMenu (int tr)
    {
        const auto& tracks = ed->sequence().tracks;
        if (tr < 0 || tr >= (int) tracks.size()) return;
        int def = 0;
        for (int k = 0; k < tr; ++k) def += tracks[(size_t) k].kind == isfx::TrackKind::Audio ? 1 : 0;
        const int cur = tracks[(size_t) tr].recLane;
        const auto& lanes = s.p().lanes; const auto& mx = s.p().mixer;
        auto laneName = [&] (int i) { return "Track " + juce::String (i + 1) + (i < (int) lanes.size() && lanes[(size_t) i].name.isNotEmpty() ? "  " + lanes[(size_t) i].name : juce::String()); };
        juce::PopupMenu m, rec, ins;
        for (int i = 0; i < juce::jmax ((int) lanes.size(), def + 1); ++i) rec.addItem (1000 + i, laneName (i), true, cur == i);
        rec.addSeparator(); rec.addItem (1999, "New REC track");
        const int lane = cur >= 0 ? cur : def;
        for (int i = 1; i < (int) mx.size(); ++i)
            ins.addItem (2000 + i, "Insert " + juce::String (i) + "  " + mx[(size_t) i].name, true, tracks[(size_t) tr].recInsert == i);
        m.addSectionHeader ("Plays live on REC: automate it, mix it, EXPORT ALL has it");
        m.addItem (1000 + def, "Route to " + laneName (def) + " (same number)", true, cur == def);
        m.addSubMenu ("Route to REC track", rec);
        m.addSubMenu ("Route to Mixer insert", ins, mx.size() > 1);
        m.addItem (3000, "Unroute", cur >= 0);
        m.showMenuAsync (juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<EditorView> (this), tr, lane] (int r)
        {
            if (safe == nullptr || r <= 0) return;
            auto& self = *safe;
            if (r == 3000)
            {
                const auto key = self.ed->sequence().tracks[(size_t) tr].routeKey;
                self.ed->setTrackRoute (tr, -1); ++self.linkGen[key]; self.linkSig[key] = "unrouted";   // Undo re-routes it: the poll sees a change and rebuilds the REC clip
                self.removeLink (key); return;
            }
            if (r >= 2000)
            {   // same REC track: just move its clip to the insert (its REC edits stay); otherwise render it there
                const auto& t0 = self.ed->sequence().tracks[(size_t) tr];
                const bool same = t0.recLane == lane; const auto key = t0.routeKey;
                self.ed->setTrackRoute (tr, lane, r - 2000);
                if (! (same && self.retarget (key, r - 2000))) self.syncLink (tr);
                return;
            }
            self.ed->setTrackRoute (tr, r == 1999 ? (int) self.s.p().lanes.size() : r - 1000);
            self.syncLink (tr);
        });
    }
    bool retarget (const juce::String& key, int insert)
    {
        linkIns[key] = insert;
        if (std::none_of (s.p().clips.begin(), s.p().clips.end(), [&] (const Clip& c) { return isLinkClip (c, key); })) return false;
        s.edit ("VIDEO track insert", [&] (Project& p) {
            for (auto& c : p.clips) if (isLinkClip (c, key)) c.mixerTrack = insert > 0 && insert < (int) p.mixer.size() ? insert : -1;
        });
        return true;
    }
    void removeLink (const juce::String& key)
    {
        if (std::none_of (s.p().clips.begin(), s.p().clips.end(), [&] (const Clip& c) { return isLinkClip (c, key); })) return;
        s.edit ("Unroute VIDEO track", [&] (Project& p) {
            p.clips.erase (std::remove_if (p.clips.begin(), p.clips.end(), [&] (const Clip& c) { return isLinkClip (c, key); }), p.clips.end());
        });
        ed->refreshPlayback();
    }
    void syncLink (int t)
    {
        const auto key = ed->sequence().tracks[(size_t) t].routeKey;
        if (key.isEmpty()) return;
        const auto sig = ed->trackSignatures();
        linkSig[key] = t < sig.size() ? sig[t] : juce::String();
        const int gen = ++linkGen[key];
        const auto f = linkDir().getChildFile (linkPrefix (key) + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S") + ".wav").getNonexistentSibling();
        info.setText ("Routing VIDEO track " + juce::String (t + 1) + " to REC...", juce::dontSendNotification);
        ed->renderTrackAsync (t, f, [safe = juce::Component::SafePointer<EditorView> (this), key, gen, f] (double secs)
        {
            if (safe == nullptr) return;
            auto& self = *safe;
            const int t = self.trackOfKey (key);   // the track may have moved while it rendered
            const auto& tracks = self.ed->sequence().tracks;
            const int lane = t >= 0 ? tracks[(size_t) t].recLane : -1;
            if (lane < 0 || self.linkGen[key] != gen) { f.deleteFile(); return; }   // unrouted, or a newer render is on its way
            if (secs < 0)
            {   // the WAV couldn't be written: keep what REC has and try again next second
                self.linkSig[key] = {};
                self.info.setText ("Couldn't write VIDEO track " + juce::String (t + 1) + "'s audio - retrying", juce::dontSendNotification); return;
            }
            if (secs <= 0)
            {   // nothing audible left (clips deleted or the track muted): its REC clip goes too
                f.deleteFile(); self.removeLink (key);
                self.info.setText ("VIDEO track " + juce::String (t + 1) + " has no sound - nothing on REC", juce::dontSendNotification); return;
            }
            const int vidIns = tracks[(size_t) t].recInsert;
            int keepInsert = vidIns;
            if (auto li = self.linkIns.find (key); li != self.linkIns.end() && li->second == vidIns)
                for (auto& c : self.s.p().clips) if (isLinkClip (c, key)) { keepInsert = c.mixerTrack; break; }   // the insert picked on REC stays
            self.linkIns[key] = vidIns;
            self.s.edit ("VIDEO track to REC", [&] (Project& p) {
                p.clips.erase (std::remove_if (p.clips.begin(), p.clips.end(), [&] (const Clip& c) { return isLinkClip (c, key); }), p.clips.end());
                while ((int) p.lanes.size() <= lane)
                {
                    Lane l; l.name = "Track " + juce::String ((int) p.lanes.size() + 1);
                    l.mixerTrack = p.addMixerTrack (l.name);
                    p.lanes.push_back (l);
                }
                Clip c; c.uid = p.newUid(); c.lane = lane; c.audioFile = f; c.patternIndex = -1;
                if (keepInsert > 0 && keepInsert < (int) p.mixer.size()) c.mixerTrack = keepInsert;   // only this clip: the lane's other audio keeps its insert
                c.start = 0.0; c.length = secondsToBeats (secs, p.bpm);
                p.clips.push_back (c);
            });
            self.ed->refreshPlayback();
            const int mt = keepInsert > 0 ? keepInsert : lane < (int) self.s.p().lanes.size() ? self.s.p().lanes[(size_t) lane].mixerTrack : 0;
            self.info.setText ("VIDEO track " + juce::String (t + 1) + " plays on REC Track " + juce::String (lane + 1) + " -> insert " + juce::String (mt)
                               + ": automate and mix it there, EXPORT ALL includes it", juce::dontSendNotification);
        });
    }
    void tracksMenu()
    {
        juce::PopupMenu m, to;
        m.addItem (1, "Each track -> its own new mixer insert");
        const auto& mx = s.p().mixer;
        for (int i = 1; i < (int) mx.size(); ++i) to.addItem (100 + i, "Insert " + juce::String (i) + "  " + mx[(size_t) i].name);
        m.addSubMenu ("All tracks -> insert #", to, mx.size() > 1);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&toMixer),
            [safe = juce::Component::SafePointer<EditorView> (this)] (int r) { if (safe && r > 0) safe->tracksToMixer (r == 1 ? -1 : r - 100); });
    }
    void tracksToMixer (int insert)
    {
        const auto dir = hhsHome().getChildFile ("Video Tracks").getChildFile (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S"));
        const auto tr = ed->renderTracks (dir);
        if (tr.empty()) { info.setText ("No VIDEO track has sound yet - import audio or video with sound first", juce::dontSendNotification); return; }
        s.edit ("Video tracks to mixer", [&] (Project& p) {
            for (auto& t : tr)
            {
                Lane l; l.name = t.name + " (video)";
                l.mixerTrack = insert > 0 && insert < (int) p.mixer.size() ? insert : p.addMixerTrack (l.name);
                p.lanes.push_back (l);
                Clip c; c.uid = p.newUid(); c.lane = (int) p.lanes.size() - 1; c.audioFile = t.file; c.patternIndex = -1;
                c.start = 0.0; c.length = secondsToBeats (t.seconds, p.bpm);
                p.clips.push_back (c);
            }
        });
        info.setText (juce::String ((int) tr.size()) + " VIDEO track(s) on Record, each routed to " + (insert > 0 ? "insert " + juce::String (insert) : juce::String ("its own insert")), juce::dontSendNotification);
    }
    void timerCallback() override
    {
        const bool hostOn = s.engine.isPlaying();
        if (isfx::hostPlaying().exchange (hostOn) != hostOn) ed->refreshPlayback();   // routed tracks never sound from both
        if (juce::Time::getMillisecondCounter() - linkPollMs > 1000 && ! s.engine.isRecording())
        {   // once a second: a routed track whose clips or mute changed re-renders in the background and replaces its REC clip
            linkPollMs = juce::Time::getMillisecondCounter();
            const auto& tr = ed->sequence().tracks;
            bool anyRouted = false;
            for (auto& t : tr) anyRouted = anyRouted || t.recLane >= 0;
            if (anyRouted)
            {
                const auto sig = ed->trackSignatures();
                for (int t = 0; t < (int) tr.size() && t < sig.size(); ++t)
                {
                    if (tr[(size_t) t].recLane < 0) continue;
                    const auto key = tr[(size_t) t].routeKey;
                    auto it = linkSig.find (key);
                    if (it == linkSig.end())
                    {   // first sight (a VIDEO project just opened): its REC clip is already in the song it was routed in; never add it to another
                        if (! sig[t].fromFirstOccurrenceOf ("|", false, false).upToFirstOccurrenceOf ("|", false, false).containsChar ('-')) { linkSig[key] = sig[t]; linkIns[key] = tr[(size_t) t].recInsert; }
                    }
                    else if (it->second != sig[t]) syncLink (t);
                    else if (auto li = linkIns.find (key); li != linkIns.end() && li->second != tr[(size_t) t].recInsert)
                        retarget (key, tr[(size_t) t].recInsert);   // Undo/Redo of an insert change: move the clip, keep its REC edits
                }
            }
        }
        isfx::hostBusy() = s.engine.isRecording();
        ed->setActive (isShowing() || popped());
        if (! follow.getToggleState() || (! isShowing() && ! popped())) { hostWasPlaying = s.engine.isPlaying(); return; }
        const bool playing = s.engine.isPlaying();
        const double t = beatsToSeconds (s.engine.positionBeats(), s.p().bpm);
        if (playing != hostWasPlaying || (! playing && std::abs (t - lastT) > 0.02)) ed->hostTransport (playing, t);
        hostWasPlaying = playing; lastT = t;
    }
    Studio& s;
    std::unique_ptr<isfx::MainComponent> ed;
    std::unique_ptr<Win> win;
    juce::TextButton exportAll { "EXPORT ALL" };
    struct MixJob : juce::ThreadWithProgressWindow   // renders the Record mix off the message thread, then hands it to the video export
    {
        MixJob (Engine& e, juce::File f) : juce::ThreadWithProgressWindow ("EXPORT ALL: rendering the Record mix...", true, true), engine (e), file (std::move (f)) {}
        void run() override { ok = engine.renderToWav (file, true, [this] (float p) { setProgress (p); return ! threadShouldExit(); }, -1, 24, (double) isfx::kMixRate, 0); }
        void threadComplete (bool cancelled) override { if (onDone) onDone (ok && ! cancelled); }
        Engine& engine; juce::File file; bool ok = false; std::function<void (bool)> onDone;
    };
    std::unique_ptr<MixJob> mixJob;
    void exportAllWithRecord()
    {
        if (mixJob != nullptr) return;
        auto wav = hhsHome().getChildFile ("Video Export").getChildFile ("Record mix.wav");
        wav.getParentDirectory().createDirectory();
        s.engine.stop();
        mixJob = std::make_unique<MixJob> (s.engine, wav);
        mixJob->onDone = [safe = juce::Component::SafePointer<EditorView> (this), wav] (bool ok)
        {
            juce::MessageManager::callAsync ([safe, wav, ok] {
                if (safe == nullptr) return;
                safe->mixJob = nullptr;
                if (! ok) { safe->info.setText ("EXPORT ALL stopped: the Record mix didn't render", juce::dontSendNotification); return; }
                if (! safe->popped()) safe->popOut();
                safe->ed->exportWithSoundtrack (wav);
            });
        };
        mixJob->launchThread();
    }
    juce::TextButton push { "PUSH" }, pop { "POP OUT" }, follow { "FOLLOW" }, toRecord { "SONG TO RECORD" }, toMixer { "TRACKS TO MIXER" };
    juce::Label info;
    bool hostWasPlaying = false; double lastT = -1.0;
    static constexpr int bar = 34;
};
}
std::unique_ptr<juce::Component> createVideoView (Studio& s) { return std::make_unique<EditorView> (s); }
}
#else
// VIDEO tab: score to picture. The video follows the song playhead; decoding runs on the OS player's own threads
// and the sync check is a 30 Hz message-thread timer, so nothing touches the audio thread or blocks the UI.
namespace hhs
{
namespace
{
class VideoView : public juce::Component, private juce::Timer
{
public:
    explicit VideoView (Studio& st) : s (st)
    {
        fm.registerBasicFormats();
        for (auto* b : { &load, &toRecord, &sound }) addAndMakeVisible (b);
        addAndMakeVisible (offset); addAndMakeVisible (info);
        offset.setRange (-600.0, 600.0, 0.01); offset.setValue (0.0); offset.setTextValueSuffix (" s");
        offset.setSliderStyle (juce::Slider::LinearHorizontal); offset.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
        offset.setTooltip ("Where the video starts on the song timeline (seconds)");
        sound.setClickingTogglesState (true); sound.setTooltip ("Hear the video's own audio");
        info.setColour (juce::Label::textColourId, col::dim);
        info.setText ("IMPORT a video, then score it with your tracks and mixer. The picture follows the playhead.", juce::dontSendNotification);
        load.onClick = [this] { pick(); };
        toRecord.onClick = [this] { audioToRecord(); };
        sound.onClick = [this] {
           #if HHS_HAS_VIDEO
            video.setAudioVolume (sound.getToggleState() ? 1.0f : 0.0f);
           #endif
        };
       #if HHS_HAS_VIDEO
        addAndMakeVisible (video);
       #endif
        startTimerHz (30);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
        if (! HHS_HAS_VIDEO || file == juce::File())
        {
            g.setColour (col::dim); g.setFont (juce::FontOptions (15.0f));
            g.drawText (HHS_HAS_VIDEO ? "No video loaded" : "Video playback needs the Mac, Windows or iPhone build", screen, juce::Justification::centred);
        }
    }
    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop (phoneUI ? 44 : 34).reduced (4);
        load.setBounds (bar.removeFromLeft (90)); bar.removeFromLeft (4);
        toRecord.setBounds (bar.removeFromLeft (150)); bar.removeFromLeft (4);
        sound.setBounds (bar.removeFromLeft (110)); bar.removeFromLeft (8);
        offset.setBounds (bar.removeFromLeft (juce::jmin (320, bar.getWidth() / 2)));
        info.setBounds (bar);
        screen = r;
       #if HHS_HAS_VIDEO
        video.setBounds (r);
       #endif
    }
private:
    void pick()
    {
        chooser = std::make_unique<juce::FileChooser> ("Import video", juce::File(), "*.mp4;*.mov;*.m4v;*.avi;*.wmv");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe = juce::Component::SafePointer<VideoView> (this)] (const juce::FileChooser& fc) {
            if (safe == nullptr || ! fc.getResult().existsAsFile()) return;
            safe->file = fc.getResult();
            safe->info.setText (safe->file.getFileName(), juce::dontSendNotification);
           #if HHS_HAS_VIDEO
            safe->video.loadAsync (juce::URL (safe->file), [safe] (const juce::URL&, juce::Result r) {
                if (safe == nullptr) return;
                if (r.failed()) safe->info.setText ("Can't open this video: " + r.getErrorMessage(), juce::dontSendNotification);
                else safe->video.setAudioVolume (safe->sound.getToggleState() ? 1.0f : 0.0f);
            });
           #endif
            safe->repaint();
        });
    }
    // the video's audio lands on a new Record track as a normal clip at the video's start: move it anywhere after
    void audioToRecord()
    {
        if (! file.existsAsFile()) { info.setText ("Import a video first", juce::dontSendNotification); return; }
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (file));
        if (rd == nullptr || rd->sampleRate <= 0) { info.setText ("This system can't read the video's audio; export it as WAV and drag it in", juce::dontSendNotification); return; }
        const double secs = (double) rd->lengthInSamples / rd->sampleRate, at = juce::jmax (0.0, offset.getValue());
        const auto f = file;
        s.edit ("Video audio to Record", [&] (Project& p) {
            Clip c; c.uid = p.newUid(); c.audioFile = f; c.patternIndex = -1;
            c.start = secondsToBeats (at, p.bpm); c.length = secondsToBeats (secs, p.bpm);
            c.lane = p.pushLane (0, c.start, c.start + c.length, f.getFileNameWithoutExtension() + " (video)");
            p.clips.push_back (c);
        });
        info.setText ("Video audio is on the Record timeline", juce::dontSendNotification);
    }
    void timerCallback() override
    {
       #if HHS_HAS_VIDEO
        if (! isShowing() || ! video.isVideoOpen()) { if (video.isVideoOpen() && video.isPlaying()) video.stop(); return; }
        const double t = beatsToSeconds (s.engine.positionBeats(), s.p().bpm) - offset.getValue();
        const double len = video.getVideoDuration();
        const bool inside = t >= 0.0 && t < len;
        if (s.engine.isPlaying() && inside)
        {
            if (! video.isPlaying()) { video.setPlayPosition (t); video.play(); }
            else if (std::abs (video.getPlayPosition() - t) > 0.08) video.setPlayPosition (t);   // drift > 2 frames: resync
        }
        else
        {
            if (video.isPlaying()) video.stop();
            const double want = juce::jlimit (0.0, juce::jmax (0.0, len - 0.01), t);
            if (std::abs (video.getPlayPosition() - want) > 0.02) video.setPlayPosition (want);   // scrub with the playhead
        }
       #endif
    }
    Studio& s;
    juce::AudioFormatManager fm;
    juce::File file;
    juce::Rectangle<int> screen;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TextButton load { "IMPORT" }, toRecord { "AUDIO TO RECORD" }, sound { "VIDEO SOUND" };
    juce::Slider offset;
    juce::Label info;
   #if HHS_HAS_VIDEO
    juce::VideoComponent video { false };
   #endif
};
}
std::unique_ptr<juce::Component> createVideoView (Studio& s) { return std::make_unique<VideoView> (s); }
}
#endif

namespace hhs
{
std::function<void()>& videoPopHook() { static std::function<void()> hook; return hook; }
std::function<void()>& videoExportAllHook() { static std::function<void()> hook; return hook; }
}
