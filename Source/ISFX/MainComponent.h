#pragma once
#include "App.h"

namespace isfx
{
struct ImportResult
{
    std::vector<MediaItem> items;
    std::map<juce::String, juce::Image> thumbs;
    std::map<juce::String, Waveform> waves;
    std::map<juce::String, std::shared_ptr<const std::vector<float>>> stereo;
    std::map<juce::String, juce::String> proxies;
    juce::String newSongId;
    SongAnalysis song;
    std::shared_ptr<std::vector<float>> songMono;
    juce::StringArray errors;
    juce::Array<juce::File> luts, presets, templates;
};
using ProgressCb = std::function<void (double, const juce::String&)>;
// Worker-thread import: probe, thumbnails, waveforms, song analysis and take sync (no project access)
ImportResult decodeFiles (const juce::Array<juce::File>&, const juce::String& songId, std::shared_ptr<std::vector<float>> songMono,
                          bool makeSong, ProgressCb, std::function<bool()> cancelled);
SyncInfo syncTake (const juce::File& take, const std::vector<float>& songMono, juce::String& err);

ISFXLookAndFeel& sharedLookAndFeel();

class MainComponent : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer, private juce::ScrollBar::Listener
{
public:
    // external: the host app's device (HumHouse VIDEO tab) - adds our player as one more callback, never reopens the device
    explicit MainComponent (juce::AudioDeviceManager* external = nullptr);
    void hostTransport (bool playing, double seconds);   // follow the host's play/stop + playhead
    double positionSeconds() const { return ticksToSec (player.isPlaying() ? player.position() : st.playhead); }
    juce::File songFile() const;
    struct TrackRender { juce::String name; juce::File file; double seconds = 0; };
    std::vector<TrackRender> renderTracks (const juce::File& dir);   // each audible track with sound -> its own stereo WAV
    struct ClipRender { juce::String name; juce::File file; double start = 0, seconds = 0; int track = 0; };
    std::vector<ClipRender> renderClips (const juce::File& dir, bool selectedOnly);   // PUSH: each clip with sound -> WAV at its timeline spot
    bool hasSelection() const { return ! st.selected.isEmpty(); }
    ~MainComponent() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void tryQuit (std::function<void()> quit);
    void openFile (const juce::File&);
    int runSelfTest (const juce::File& outDir);
    std::function<void (const juce::String&)> onTitle;
private:
    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar*, double) override;
    void refresh (bool inspectorToo);
    void seek (Ticks t);
    void togglePlay();
    void setTool (Tool t);
    void runTask (const juce::String& title, std::function<void (ProgressCb, std::function<bool()>)> work, std::function<void()> done);
    void importFiles (const juce::Array<juce::File>&, std::function<void()> then = {});
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& paths, int, int) override
    {
        juce::Array<juce::File> fs; for (auto& f : paths) fs.add (juce::File (f));
        importFiles (fs);
    }
    void applyImport (ImportResult&);
    void placeSongClip();
    void setSong (const juce::String& id);
    void autoSync (const juce::String& onlyId = {});
    void syncClips (const juce::StringArray& clipIds);   // each clip matched to the song by its own sound
    void relink (const juce::String& id);
    void removeMedia (const juce::String& id);
    void loadMediaCaches();
    void generate();
    void chop();
    void save (bool as, std::function<void()> after = {});
    void open();
    void newProject();
    void exportDialog();
    bool exportSequence (const Project& p, int seqIndex, const ExportJob& job, ProgressCb, std::function<bool()> cancelled, juce::String& err);
    void fileMenu();
    void presetsMenu();
    void savePage();
    void saveTemplateDialog (std::function<void()> after = {});
    void applyTemplateFile (const juce::File&);
    void pluginsMenu();
    void trackPluginsMenu (int track);
    void askText (const juce::String& title, const juce::String& def, std::function<void (juce::String)> cb);
    void updateSeqBox();
    void showMsg (const juce::String& title, const juce::String& msg);

    ISFXLookAndFeel& lnf = sharedLookAndFeel();   // outlives every window; App::shutdown clears the default
    AppState st;
    PluginRack rack;
    TrackRacks trackRacks;
    juce::AudioDeviceManager ownDevices;
    juce::AudioDeviceManager& devices;
    const bool embedded;
    Player player { st, rack };
    Preview preview { st };
    Timeline timeline { st };
    MediaPanel media { st };
    FxLibrary fxlib { st };
    Inspector inspector { st };
    juce::TooltipWindow tips { this, 450 };
    juce::ScrollBar hscroll { false };
    juce::ShapeButton undoBtn { "Undo", juce::Colour (0xffc9ced8), juce::Colours::white, juce::Colour (0xff9b5cff) },
        redoBtn { "Redo", juce::Colour (0xffc9ced8), juce::Colours::white, juce::Colour (0xff9b5cff) },
        startBtn { "Go to start", juce::Colour (0xffc9ced8), juce::Colours::white, juce::Colour (0xff9b5cff) };
    juce::ComboBox qualityBox;
    juce::TextButton renderBtn { "RENDER" };
    juce::TextButton fileBtn { "File" }, saveBtn { "Save" }, importBtn { "Import" }, presetsBtn { "Presets" },
        pluginsBtn { "Plug-ins" }, playBtn { "Play" }, exportBtn { "EXPORT" },
        toolBtn[4] { juce::TextButton ("E Select"), juce::TextButton ("P Paint"), juce::TextButton ("D Delete"), juce::TextButton ("C Cut") },
        snapBtn { "S Snap" }, rippleBtn { "R Ripple" }, magnetBtn { "Magnet" }, gridBtn { "B Grid" }, chopBtn { "CHOP" }, genBtn { "GENERATE 3 EDITS" },
        keepBtn { "Keep range" }, delRangeBtn { "Delete range" }, zoomIn { "+" }, zoomOut { "-" };
    juce::ComboBox seqBox, aspectBox, snapBox, chopBox, styleBox;
    juce::Label status;
    std::vector<std::unique_ptr<juce::DocumentWindow>> pluginWindows;
    std::shared_ptr<std::vector<float>> songMono;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::AlertWindow> dialog;
    bool selfTest = false;
    bool showMedia = true, showInspector = true, maxPreview = false, maxTimeline = false;
    float timelineFrac = 0.38f;
    int splitY = 0; bool draggingSplit = false;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { draggingSplit = false; }
    juce::TextButton mediaTgl { "Media" }, inspTgl { "Inspector" }, maxPrevBtn { "Max Preview" }, maxTlBtn { "Max Timeline" }, movieBtn { "Movie Border" };
    juce::VBlankAttachment vblank { this, [this] { if (player.isPlaying()) timerCallback(); } };
    juce::uint32 genSeed = 1;
};
}
