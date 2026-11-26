#pragma once
#include <set>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "Model.h"
#include "Media.h"
#include "Analysis.h"
#include "Editing.h"
#include "Looks.h"
#include "AudioMix.h"
#include "Exporter.h"
#include "Renderer.h"
#include "Plugins.h"
#include "Presets.h"

namespace isfx
{
// the host is recording: background video renders wait so audio + UI get the machine
inline std::atomic<bool>& hostBusy() { static std::atomic<bool> b { false }; return b; }
// ---- theme
namespace theme
{
    // chrome + electric blue, matching the InSequence FX logo (VIDEO tab looks exactly like the FX app)
    const juce::Colour bg { 0xff060709 }, panel { 0xff0d0f14 }, panel2 { 0xff141821 }, line { 0xff232a38 },
                       text { 0xffe4e9f2 }, dim { 0xff8890a2 }, accent { 0xff3d7bff }, accent2 { 0xff9cc0ff },
                       good { 0xff3ddc84 }, warn { 0xffffb020 }, bad { 0xffff4d4d };
}
void drawLogo (juce::Graphics&, juce::Rectangle<float> area);  // chrome InSequence FX mark + wordmark
juce::Typeface::Ptr logoTypeface();
juce::Font headerFont (float height);                            // wide logo-style face (Michroma) for titles

class ISFXLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ISFXLookAndFeel();
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
};

// ---- shared app state
struct AppState
{
    Project project = Project::makeNew();
    History history;
    AudioCache audio;
    std::map<juce::String, Waveform> waves;
    std::map<juce::String, juce::Image> thumbs;
    juce::File file;
    bool dirty = false;
    // selection / transport
    juce::StringArray selected;
    Ticks playhead = 0, rangeA = 0, rangeB = 0;
    SnapMode snap = SnapMode::Sixteenth;
    bool snapOn = true, ripple = false, showGrid = true, magnetic = true;
    std::vector<Clip> clipboard;
    juce::ChangeBroadcaster changed;   // project edited -> repaint everything
    struct RenderCache   // Enter renders red sections to JPEG frames; live playback shows them instead of rendering
    {
        juce::CriticalSection lock;
        std::map<juce::int64, juce::MemoryBlock> frames;
        std::atomic<juce::int64> sig { 0 };
        int w = 0, h = 0; size_t bytes = 0;
    } rcache;

    void edit (const juce::String& what) { history.push (project, what); dirty = true; }
    void notify() { changed.sendChangeMessage(); }
    Ticks snapT (Ticks t) const { return snapOn ? snapTime (t, project, snap) : t; }
};

// ---- playback: song + clip audio through mixSequence and the master plug-in rack; the audio clock drives the preview
class Player : public juce::AudioIODeviceCallback
{
public:
    Player (AppState& s, PluginRack& r) : st (s), rack (r) {}
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int nOut, int n, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* d) override { rate = d->getCurrentSampleRate(); }
    void audioDeviceStopped() override {}
    void play (Ticks from);
    void stop() { playing = false; }
    bool isPlaying() const { return playing.load(); }
    Ticks position() const { return secToTicks (posSec.load()); }
    void setSequence (const Sequence& s) { const juce::ScopedLock sl (lock); seq = s; }
    void setTrackFx (TrackFxHost* h) { const juce::ScopedLock sl (lock); trackFx = h; }
    static constexpr int kBlock = 512;
private:
    AppState& st;
    PluginRack& rack;
    TrackFxHost* trackFx = nullptr;
    juce::CriticalSection lock;
    Sequence seq;
    std::atomic<bool> playing { false }, restart { false };
    std::atomic<double> posSec { 0 };
    std::atomic<juce::int64> startSample { 0 };
    double rate = 48000;
    // 48 kHz mixed + plug-in processed FIFO, read with a fractional cursor (device rate may differ)
    std::vector<float> fL, fR, bL, bR;
    juce::int64 fifoStart = 0, mixPos = 0;
    double readPos = 0;
};

// ---- preview: renders on a background thread, so the UI never blocks
class Preview : public juce::Component, private juce::Thread, private juce::AsyncUpdater
{
public:
    explicit Preview (AppState& s);
    ~Preview() override;
    void paint (juce::Graphics&) override;
    void requestFrame (Ticks t);
    juce::Rectangle<int> frameArea() const;
    void sourcesChanged() { clearReq = true; requestFrame (lastReq); }
    void setProject (const Project& p);
    double renderMs() const { return lastMs; }
    // Playback resolution like Premiere: 1 = Full, 2 = 1/2, 4 = 1/4, 8 = 1/8 (paused frames always render sharp)
    void setQuality (int div) { quality = juce::jmax (1, div); requestFrame (lastReq); }
    int getQuality() const { return quality; }
    void setLive (bool b) { if (live.exchange (b) && ! b) requestFrame (lastReq); }
    // RENDER: the whole timeline rendered ahead at 1080/div; playback then shows finished frames (no skipping)
    void startRenderAll (int div);
    void cancelRenderAll();
    float renderAllProgress() const;   // < 0 none, 0..1 rendering, 1 done
    void projectEdited (const Project&);   // an edit that changes the project drops the render
    void renderRed();                      // Enter: render every red section in the background at full quality
    bool rendering() const { return baker != nullptr && baker->isThreadRunning(); }
    std::function<void()> onCacheChanged;
    // on-video transform: drag inside = move, corners = scale (Shift = free X/Y), edges = crop, knob = rotate (Shift = 15 deg)
    std::function<void()> onEdited, onSelect;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
private:
    enum class Handle { None, Move, Corner, Edge, Rotate };
    Handle hitHandle (const ClipGeom&, juce::Point<float>, int& idx) const;
    bool geomFor (const Clip&, ClipGeom&) const;
    const Clip* shownClip (const juce::String& id) const;
    Handle handle = Handle::None;
    int handleIdx = 0;
    bool movedHandle = false;
    juce::String editId;
    Project before;
    FxStack fx0;
    ClipGeom g0;
    juce::Point<float> down;
    void run() override;
    void handleAsyncUpdate() override;
    AppState& st;
    Renderer renderer;
    juce::CriticalSection lock;
    juce::WaitableEvent wake;
    Ticks want = -1, lastReq = 0;
    std::shared_ptr<const Project> snap;
    int wantW = 0, wantH = 0;
    std::atomic<bool> clearReq { false }, live { false };
    int wantDecodeH = 0;
    juce::Image shown;
    std::atomic<double> lastMs { 0 }, avgMs { 0 };
    std::atomic<int> quality { 2 };
    double fps = 0, fpsT0 = 0; int fpsCount = 0;
    struct RenderAll;
    std::shared_ptr<RenderAll> rall;
    int pausedW = 0, pausedH = 0;
    void bake();
    struct Baker : juce::Thread { explicit Baker (Preview& p) : juce::Thread ("ISFX render"), owner (p) {} void run() override { owner.bake(); } Preview& owner; };
    Renderer bakeRenderer;
    std::unique_ptr<Baker> baker;
};

// ---- timeline
enum class Tool { Select, Paint, Delete, Cut };
class Timeline : public juce::Component, public juce::DragAndDropTarget, public juce::FileDragAndDropTarget, public juce::SettableTooltipClient
{
public:
    explicit Timeline (AppState& s);
    std::function<void (const juce::Array<juce::File>&, int track, Ticks at)> onFilesDropped;   // from Finder/Explorer
    std::function<void (const juce::File&, int track, Ticks at)> onSfxDropped;                  // from the built-in SFX library
    std::function<void (const juce::String& title, const juce::String& def, std::function<void (juce::String)>)> onAskText;
    std::function<void (const juce::StringArray& clipIds)> onSyncClips;
    std::function<void (const juce::StringArray& clipIds, float smoothSec)> onStabilize;
    std::function<void (const juce::StringArray& clipIds)> onSliceCuts;
    std::function<void (const juce::String&)> onStatus;
    std::function<void (int track)> onTrackLink;                          // right-click an audio track header: Route to REC track / Mixer
    std::function<void (int track)> onTrackPlugins;                       // right-click an audio track header   // right-click Sync clip(s) to audio
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray&, int x, int y) override;
    std::function<void()> onEdited;          // after any edit
    std::function<void (Ticks)> onSeek;
    Tool tool = Tool::Select;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void selectRangeClips();
    void selectAll();   // FL-style pattern select: every clip under the highlighted range
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float) override;
    bool isInterestedInDragSource (const SourceDetails&) override { return true; }
    void itemDropped (const SourceDetails&) override;
    void itemDragMove (const SourceDetails& d) override { updateGhost (d); }
    void itemDragExit (const SourceDetails&) override { ghost.on = false; repaint(); }
    void zoomBy (float f, int anchorX);
    void ensureVisible (Ticks t);
    void revealClip (const juce::String& id);   // scroll so a new layer / text clip is on screen
    void clipMenu (const juce::String& id);
    void emptyMenu (int track, Ticks at);   // right-click an empty spot: add adjustment layer
public:
    void clipMenuSpeed (const juce::String& id);   // Ctrl+R Speed/Duration dialog
    bool switchAngles = true;
    double pxPerSec = 60;   // zoom
    double scrollSec = 0;
    int vScroll = 0;        // track rows scrolled up (deep layer stacks)
private:
    static constexpr int kRuler = 28, kHeader = 70;
    int trackY (int i) const;
    std::vector<int> rowOrder() const;
    struct Ghost { Ticks at = 0, len = 0; int track = -1; bool on = false; } ghost;
    void updateGhost (const SourceDetails&);
    struct CutDot { juce::String clipId; int x; juce::Point<int> centre; bool full; };
    std::vector<CutDot> cutDots() const;
public:
    void addAdjustmentLayer (Ticks at);
    // Effects panel right-click: scope 0 = selected clips, 1 = all clips, 2 = adjustment layer; side 0 = in, 1 = out, 2 = both (transitions)
    void applyLibraryItem (const juce::String& desc, int scope, int side);
private:
    int trackAt (int y) const;
    double xToSec (int x) const { return scrollSec + (x - kHeader) / pxPerSec; }
    int secToX (double s) const { return kHeader + (int) std::lround ((s - scrollSec) * pxPerSec); }
    const Clip* clipAt (juce::Point<int> p, int& edge) const;
    AppState& st;
    enum class Drag { None, Move, TrimL, TrimR, Range, Scrub, Song, TransLen, Box, Vol } drag = Drag::None;
    juce::String dragClip;
    juce::Rectangle<int> box; juce::StringArray boxBase;
    Ticks paintNext = -1;   // paint-drag with a multi-clip selection: next free stamp time   // drag-box multi-select
    Ticks dragStart = 0, dragOrig = 0; int dragTrack = 0;
    Ticks snapGuide = -1;
    bool dragOut = false;   // a Move dragged off the tracks: release pulls the clips out
    Ticks magnetSnap (Ticks t, Ticks len);
    Project beforeDrag;
};

// ---- panels
class MediaPanel : public juce::Component, public juce::DragAndDropContainer, private juce::ListBoxModel
{
public:
    explicit MediaPanel (AppState& s);
    void refresh() { list.updateContent(); list.repaint(); }
    void resized() override;
    void paint (juce::Graphics&) override;
    std::function<void()> onImport, onSyncAll;
    std::function<void (const juce::String&)> onAddToTimeline, onSetSong, onRelink, onRemove, onResync;
private:
    int getNumRows() override;
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>&) override;
    void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override;
    juce::String getTooltipForRow (int) override;
    void listBoxItemClicked (int, const juce::MouseEvent&) override;
    AppState& st;
    juce::ListBox list;
    juce::TextButton importBtn { "Import" }, syncBtn { "Auto Sync" };
};

// drag source for transitions (drop on a cut) and effects/looks (drop on a clip or empty space = effect layer)
// length for new transitions, in beats (VIDEO library slider: short to long)
inline std::atomic<float>& transitionBeats() { static std::atomic<float> b { 0.5f }; return b; }

class FxLibrary : public juce::Component, public juce::DragAndDropContainer, private juce::ListBoxModel,
                  private juce::Thread, private juce::AsyncUpdater
{
public:
    explicit FxLibrary (AppState& s);
    ~FxLibrary() override;
    void reload();   // re-scan library folders (after Import Preset)
    void resized() override;
    void paint (juce::Graphics&) override;
    void paintExample (juce::Graphics&, juce::Rectangle<int>);
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void popOut (bool on);
    std::function<void (const juce::String& desc, int scope, int side)> onApply;
private:
    void exRepaint();
    bool onDivider (int y) const { return ! exArea.isEmpty() && std::abs (y - (exArea.getY() - 3)) <= 5; }
    float exFrac = 0.4f;
    bool dragDiv = false;
    juce::TextButton popBtn { "Pop out" };
    juce::TextEditor search;
    std::unique_ptr<juce::DocumentWindow> exWin;
    // folder tree like Premiere's Effects panel: Video Transitions / Video Effects / Sound FX, each with subfolders
    struct Row { juce::String label, desc, help; bool header = false; int level = 0; juce::String folder, parent; int count = 0; };
    std::vector<Row> all, rows;   // all rows / rows inside open folders
    std::set<juce::String> open;
    void refilter();
    int getNumRows() override { return (int) rows.size(); }
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override;
    void listBoxItemClicked (int, const juce::MouseEvent&) override;
    void selectedRowsChanged (int) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>&) override;
    juce::String getTooltipForRow (int) override;
    // example of the selected item rendered on the user's own footage, off the UI thread
    void run() override;
    void handleAsyncUpdate() override;
    AppState& st;
    juce::ListBox list;
    juce::Rectangle<int> exArea;
    juce::CriticalSection exLock;
    juce::WaitableEvent exWake;
    juce::String exWant, exName, exNote;
    std::shared_ptr<const Project> exProj;
    Ticks exAt = 0;
    juce::Image example;
    Renderer exRenderer;
};

class Inspector : public juce::Component
{
public:
    explicit Inspector (AppState& s);
    void rebuild();            // selection changed
    void resized() override;
    void paint (juce::Graphics&) override;
    std::function<void()> onEdited;
private:
    AppState& st;
    juce::Viewport view;
    juce::Component content;
    juce::OwnedArray<juce::Component> items;
    juce::Label title;
    juce::String target;       // clip id or "" = sequence
};
}
