#pragma once
#include <condition_variable>
#include <mutex>
#include "Model.h"
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
    std::atomic<long> workerJobs { 0 }, runs { 0 };
private:
    void workerLoop();
    std::vector<std::thread> threads;
    std::atomic<bool> quit { false };
    std::atomic<int> generation { 0 }, next { 0 }, done { 0 }, total { 0 };
    std::atomic<Job> job { nullptr };
    std::atomic<void*> ctx { nullptr };
    std::mutex m;
    std::condition_variable cv;
};

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
    double positionBeats() const { return position.load(); }
    void setPositionBeats (double b);
    std::atomic<bool> metronomeOn { false }, countInOn { true };
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

    // plugins
    std::unique_ptr<juce::AudioPluginInstance> createPlugin (const juce::String& identifier, juce::String& error);
    juce::AudioPluginInstance* channelPlugin (int channelId);
    juce::AudioPluginInstance* fxPlugin (int track, int slotUid);
    BuiltinEffect* builtinFx (int track, int slotUid);
    void storePluginStates();

    // offline
    bool renderToWav (const juce::File& out, bool songMode, std::function<bool (float)> progress, int track = -1);
    bool freezeTrack (int track);

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
    void renderSegment (double b0, double b1, int off, int n);
    void renderChannels (int n);
    void mixTracks (const float* const* in, int numIn, int n);
    void startRecordingWriters();
    void primeClipReaders (double beat);
    void finishRecordingWriters();
    void click (float* l, float* r, int n, double b0, double b1);
    void updateSolo();
    void processTrack (int track);
    double loopLength() const;

    Project proj;
    juce::CriticalSection lock;
    double sampleRate = 44100.0;
    int maxBlock = 512;

    std::map<int, std::unique_ptr<ChannelRuntime>> channelRt;
    std::vector<ChannelRuntime*> channelOrder;
    std::vector<std::unique_ptr<TrackRuntime>> trackRt;
    std::map<int, std::unique_ptr<ClipRuntime>> clipRt;
    std::map<juce::String, SharedSample> sampleCache;
    std::vector<std::vector<int>> levels;
    std::vector<std::vector<std::pair<int, float>>> sources; // per track: (source track, gain)
    std::vector<char> soloMuted;

    std::atomic<bool> playing { false }, recording { false }, countingIn { false };
    std::atomic<double> position { 0.0 };
    std::atomic<float> declickGain { 1.0f };   // 4 ms fade-in after starting / jumping mid-sound
    std::atomic<bool> chasePending { true };   // start notes already sounding under the playhead
    double countInRemaining = 0.0;
    std::unique_ptr<WorkerPool> pool;
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
        bool started = false;
        juce::int64 samplesWritten = 0;
        std::array<std::pair<juce::int64, double>, 256> passes {}; int numPasses = 0; // loop-record pass boundaries
        std::array<float, 32768> peaks {}; std::atomic<int> numPeaks { 0 }; float peakAcc = 0; int peakN = 0;   // live waveform
    };
    std::vector<std::unique_ptr<Take>> takes;
    juce::SpinLock takeLock;

    juce::AudioBuffer<float> scratch, inputScratch;
    struct NormState { float rms = 0.0f, gain = 1.0f; };
    std::array<NormState, 256> normState {};
    juce::AudioBuffer<float> normScratch { 2, 8192 };
    int idleThresholdBlocks = 0;
    int currentBlock = 0, currentLevel = 0;
    PlayContext ctx;
    std::atomic<bool> offline { false };
    int renderTrackOnly = -1;
    friend struct EngineJobs;
};
// Plugin scanning runs in a child copy of the app so a crashing plugin can't take the DAW down.
bool isPluginScanChild (const juce::String& commandLine);
int runPluginScanChild (const juce::String& commandLine);
}
