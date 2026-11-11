#pragma once
#include <set>
#include <condition_variable>
#include <mutex>
#include "Model.h"
#include "Drummer/DrummerCore.h"
#include "Instruments.h"
#include "Effects.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <thread>

namespace hhs
{
// Real-time worker pool: the audio thread fans independent tracks out across cores.
class WorkerPool
{
public:
    using Job = void (*) (void* ctx, int index);
    explicit WorkerPool (int numWorkers);
    ~WorkerPool();
    void run (int count, Job job, void* ctx);   // blocks until all indices are done
    int size() const { return (int) threads.size(); }
    // macOS: workers join the audio device's workgroup so the OS schedules them like the audio thread
    // (other apps such as OBS can't starve them mid-block)
    void setWorkgroup (const juce::AudioWorkgroup& wg);
    std::atomic<long> workerJobs { 0 }, runs { 0 };
private:
    struct Worker : juce::Thread
    {
        explicit Worker (WorkerPool& p) : juce::Thread ("HHS audio worker"), pool (p) {}
        void run() override { pool.workerLoop(); }
        WorkerPool& pool;
    };
    void workerLoop();
    std::vector<std::unique_ptr<Worker>> threads;
    std::atomic<bool> quit { false };
    std::atomic<int> generation { 0 }, next { 0 }, done { 0 }, total { 0 }, wgGeneration { 0 };
    std::atomic<Job> job { nullptr };
    std::atomic<void*> ctx { nullptr };
    std::mutex m, wgMutex;
    std::condition_variable cv;
    juce::AudioWorkgroup workgroup;
};

struct FxRuntime;
class Engine : public juce::AudioIODeviceCallback,
               public juce::AudioPlayHead,
               public juce::ChangeBroadcaster
{
public:
    Engine();
    ~Engine() override;

    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList knownPlugins;

    void initialiseAudio (const juce::XmlElement* savedState);
    Project& project() { return proj; }
    void publishProject();
    std::atomic<bool> pluginSandbox { false };   // opt-in: a process round trip per plug-in per block is too slow for big chains
    std::atomic<float> renderLoad { 0.0f };     // third-party plug-ins run crash-protected in a helper process
    std::atomic<bool> anticipativeOn { true };   // render playback ahead (like Reaper's anticipative FX) while nothing is live
    std::atomic<int> antUnderruns { 0 };   // hand the audio thread a fresh copy of the song (after value edits that need no rebuild)
    juce::CriticalSection& getLock() { return lock; }

    // Rebuild runtime objects (instruments, plugins, fx, clip readers) to match the project. Message thread.
    void sync();
    void newProject (Project p);

    // transport
    void play();
    void stop();
    void togglePlay() { isPlaying() ? stop() : play(); }
    bool isPlaying() const { return playing.load(); }
    void setRecording (bool shouldRecord);
    bool isRecording() const { return recording.load(); }
    // Retro Record: the last 60 s of mic input is always kept (armed or not); this drops it on REC as a clip
    bool retroRecord();
    double positionBeats() const { return std::max (0.0, position.load() - antAheadBeats.load()); }   // what you hear (render-ahead aware)
    void setPositionBeats (double b);
    std::atomic<bool> metronomeOn { false }, countInOn { true };
    std::atomic<bool> dualOut { false };
    std::atomic<float> blockLoadFrac { 0.3f };   // recent peak share of the block deadline the audio callback used
    std::atomic<float> swapUsLast { 0 }, swapUsMax { 0 };   // how long the last / longest edit held the audio lock (microseconds)   // Options > Dual outputs: the master plays on 1-2 AND 3-4, the very same samples (no phasing)
    void setDualOutput (bool on);
    std::atomic<int> recordLatency { 0 };   // device in+out latency (samples): new takes move earlier by this so they sit on time
    std::atomic<int> duckAutoSrc { -1 };   // vocal insert the one-knob duck follows when no SC source is set
    std::atomic<float> masterPeakL { 0 }, masterPeakR { 0 };
    float trackPeak (int track) const;
    ScopeTap* trackScope (int t) { return t >= 0 && t < maxScopeTracks ? &(*trackScopes)[(size_t) t] : nullptr; }
    static constexpr int maxScopeTracks = 64;
    std::unique_ptr<std::array<ScopeTap, maxScopeTracks>> trackScopes = std::make_unique<std::array<ScopeTap, maxScopeTracks>>();
    double cpuLoad() const;
    // performance (Options > Performance): threads = total processing threads (0 = auto, all cores)
    void setPerformance (int threads, bool multithreaded, bool smartDisable);
    int  processingThreads() const { return (pool ? pool->size() : 0) + 1; }
    long poolStat (bool workers) const { return pool ? (workers ? pool->workerJobs.load() : pool->runs.load()) : 0; }
    float fxLoad (int track, int slotUid) const;      // share of the audio block this slot used (0..1)
    // live recording waveform: (lane, start beat, peaks, count, beats per peak) for each take being recorded
    void forEachLiveTake (const std::function<void (int, double, const float*, int, double)>& fn);
    bool isCountingIn() const { return countingIn.load(); }

    // preview (computer keyboard / clicking keys / browser)
    void previewNote (int channelId, int key, float velocity, bool on);
    void previewFile (const juce::File&);
    void stopPreview (juce::uint32 olderThanMs = 0) { if (juce::Time::getMillisecondCounter() - previewStarted.load() >= olderThanMs) previewPos = -1; }
    std::atomic<juce::uint32> previewStarted { 0 };

    // plugins
    std::unique_ptr<juce::AudioPluginInstance> createPlugin (const juce::String& identifier, juce::String& error);
    juce::AudioPluginInstance* channelPlugin (int channelId);
    juce::AudioPluginInstance* fxPlugin (int track, int slotUid);
    BuiltinEffect* builtinFx (int track, int slotUid);
    void storePluginStates();
    void releaseMemory();   // low-memory warning: RAM takes stream from disk, unused samples are freed

    // offline
    bool renderToWav (const juce::File& out, bool songMode, std::function<bool (float)> progress, int track = -1, int bits = 24, double rate = 0, int format = 0);   // bits 16/24/32 float/64 float, rate 0 = engine rate, format 0 WAV / 1 MP3 / 2 M4A
    int exportBits = 24; double exportRate = 0; int exportFormat = 0;
    float lastRenderPeak = 0;   // peak of the last render (stems: silent tracks are skipped)
    bool freezeTrack (int track);
    // FL-style consolidate: renders just these clips (audio, pattern, Drummer) through their inserts, from the
    // first one's start, into one WAV; master FX are left out so the new clip can play on the master
    bool consolidateClips (const std::vector<int>& clipUids, const juce::File& out, double& fromBeat, double& lenBeats);

    // headless processing (tests / offline)
    void prepare (double sampleRate, int maxBlock);
    void processBlock (const float* const* in, int numIn, float* const* out, int numOut, int n);
    double getSampleRate() const { return sampleRate; }

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

    // AudioPlayHead
    juce::Optional<PositionInfo> getPosition() const override;

    juce::File recordingFolder;
    std::function<void()> onRecordingAdded;
    std::function<void (juce::AudioProcessor*)> onPluginDiscard;   // message thread, before a plugin instance is deleted

    // crash guard: a marker file names the plugin being loaded; if the app dies, it is blacklisted next launch
    void beginRiskyPluginCall (const juce::PluginDescription&);
    void endRiskyPluginCall();
    juce::String recoverFromPluginCrash();   // returns the name of a plugin that crashed last run (now blacklisted)

    struct ChannelRuntime;
    struct TrackRuntime;
    struct ClipRuntime;
private:
    void prepareRuntimes();
    void rememberTake (const juce::String& path);
    void renderSegment (double b0, double b1, int off, int n);
    void renderChannels (int n);
    void mixTracks (const float* const* in, int numIn, int n);
    void startRecordingWriters (bool surprise = false);
    void primeClipReaders (double beat);
    void finishRecordingWriters();
    void click (float* l, float* r, int n, double b0, double b1);
    using Sources = std::vector<std::vector<std::pair<int, float>>>;
    void computeSolo (const Project&, const Sources&, std::vector<char>&) const;
    void processTrack (int track);
    void applyMiniEq (TrackRuntime&, const MixerTrack&, int n);
    void applyCtlLinks();
    void applyAutomation (double beat);
    double loopLength() const;
    static double loopLengthOf (const Project&);

    Project proj;              // the UI's song
    Project aproj;             // the audio thread's own copy: the UI publishes fresh ones, so neither side waits
    juce::CriticalSection lock;   // UI vs device setup only; the audio thread never takes it
    struct Staged;
    std::unique_ptr<Staged> staged;
    std::atomic<int> handoff { 0 };   // 0 free, 1 ready, 2 audio adopting, 3 adopted (leftovers inside), 4 UI writing, 5 UI adopting
    std::atomic<bool> inBlock { false }, panicPending { false };
    std::atomic<double> lastBlockMs { -1.0e9 };
    // the UI's view of the runtimes: the same objects the audio uses, in separate containers
    std::map<int, std::shared_ptr<ChannelRuntime>> uiChannelRt;
    std::vector<int> uiChannelIds;
    std::vector<std::shared_ptr<TrackRuntime>> uiTrackRt;
    std::vector<std::vector<std::shared_ptr<FxRuntime>>> uiFx;
    std::vector<juce::File> uiFrozenFile;
    std::vector<size_t> uiEdges;
    std::map<int, std::shared_ptr<ClipRuntime>> uiClipRt;
    Sources uiSources;
    int uiPoolWorkers = -1;
    bool freshLayout = false;   // newProject: pending per-track swaps belong to the old song
    struct AutoWrite { int kind, index, sub; float value; int gen; };
    int uiGen = 0, audioGen = 0;   // song generation: automation from the previous song never lands on the next
    juce::AbstractFifo autoFifo { 1024 };
    std::array<AutoWrite, 1024> autoWrites {};
    // render-ahead: a worker renders playback into a ring; the device callback only copies it out
    struct AntThread;
    std::unique_ptr<AntThread> antThread;
    std::atomic<int> antState { 0 };   // 0 real time, 3 requested, 1 rendering ahead, 2 draining back to real time
    std::atomic<bool> antBusy { false }, antDiscard { false }, antPrimed { false };
    std::atomic<double> antAheadBeats { 0.0 }, antBpm { 120.0 }, callbackMs { -1.0e9 };
    std::atomic<int> retroRl { 0 }, retroC0 { 0 }, retroC1 { 0 };
    juce::AbstractFifo antFifo { 16384 };
    juce::AudioBuffer<float> antRing { 2, 16384 }, antTmp { 2, 512 };
    std::atomic<int> antTarget { 2048 };
    juce::WaitableEvent antWake;
    bool antRenderOnce();
    void antExit (bool discard, bool wait);
    void antForceRealtime();
    void processRealtime (const float* const* in, int numIn, float* const* out, int numOut, int n, int from);
    void writeRetro (const float* il, const float* ir, int n, double newEnd, double blockBeats, int rl);
    Staged& beginStage();
    void endStage();
    void stageProject (Staged&);
    void adoptStaged();
    void adoptNow();
    void pushAuto (int kind, int index, int sub, float v);
    void tick();
    struct Publisher : juce::Timer { Engine& e; explicit Publisher (Engine& en) : e (en) {} void timerCallback() override { e.tick(); } };
    std::unique_ptr<Publisher> publisher;
    int tickCount = 0;
    double sampleRate = 44100.0;
    int maxBlock = 512;

    std::map<int, std::shared_ptr<ChannelRuntime>> channelRt;
    std::vector<ChannelRuntime*> channelOrder;
    std::vector<std::shared_ptr<TrackRuntime>> trackRt;
    std::map<int, std::shared_ptr<ClipRuntime>> clipRt;
    std::map<int, std::shared_ptr<const std::vector<DrumHit>>> drumHits;   // Drummer region uid -> what it plays
    std::map<juce::String, SharedSample> sampleCache;
    std::set<juce::String> freshTakes;   // takes recorded this session: played from RAM
    std::vector<std::vector<int>> levels;
    std::vector<std::vector<std::pair<int, float>>> sources; // per track: (source track, gain)
    std::vector<char> soloMuted;

    std::atomic<bool> playing { false }, recording { false }, countingIn { false };
    std::atomic<double> position { 0.0 };
    std::atomic<float> declickGain { 1.0f };   // 4 ms fade-in after starting / jumping mid-sound
    std::atomic<bool> editChase { false };     // chase caused by an edit while playing: keep the mix running, no fade, no re-hit
    std::atomic<bool> chasePending { true };   // start notes already sounding under the playhead
    double countInRemaining = 0.0;
    std::unique_ptr<WorkerPool> pool;
    juce::AudioWorkgroup deviceWorkgroup;
    std::atomic<bool> mtOn { true }, smartOn { true };
    std::vector<float> trackCost, chanCost;
    std::vector<int> chanJobOrder;
    juce::TimeSliceThread diskThread { "HHS disk" };
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    // preview FIFO
    struct PreviewEvent { int channelId, key; float vel; bool on; };
    juce::AbstractFifo previewFifo { 256 };
    std::array<PreviewEvent, 256> previewEvents {};
    SharedSample previewSample; std::atomic<int> previewPos { -1 };
    juce::SpinLock previewLock;

    // metronome
    double clickPhase = 0, clickFreq = 1000; int clickLeft = 0;

    // recording
    struct Take
    {
        int lane = 0; juce::File file; double startBeat = 0;
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        bool started = false, surprise = false;   // surprise: armed + playing with record off (kept as a hidden take)
        juce::int64 samplesWritten = 0;
        std::array<std::pair<juce::int64, double>, 256> passes {}; int numPasses = 0; // loop-record pass boundaries
        std::array<float, 32768> peaks {}; std::atomic<int> numPeaks { 0 }; float peakAcc = 0; int peakN = 0;   // live waveform
    };
    std::vector<std::unique_ptr<Take>> takes;
    double lastTakeStart = -1.0;
    juce::SpinLock takeLock;

    juce::AudioBuffer<float> scratch, inputScratch;
    juce::AudioBuffer<float> retroBuf;
    std::atomic<int> retroWrite { 0 }, retroLane { 0 };
    std::atomic<juce::int64> retroTotal { 0 }, retroRunFrom { 0 };   // runFrom: retroTotal when the current straight playback run began
    std::atomic<double> retroEndBeat { -1.0 };   // beat where the newest retro sample was heard (-1: stopped)
    struct NormState { float rms = 0.0f, gain = 1.0f; };
    std::array<NormState, 256> normState {};
    juce::AudioBuffer<float> normScratch { 2, 8192 };
    int idleThresholdBlocks = 0;
    std::vector<double> patLen;
    int currentBlock = 0, currentLevel = 0;
    PlayContext ctx;
    std::atomic<bool> offline { false };
    int renderTrackOnly = -1;
    double renderFromBeats = 0.0, renderLenBeats = -1.0;   // consolidate: one range instead of the whole song
    friend struct EngineJobs;
};
// Plugin scanning runs in a child copy of the app so a crashing plugin can't take the DAW down.
bool isPluginScanChild (const juce::String& commandLine);
int runPluginScanChild (const juce::String& commandLine);
}
