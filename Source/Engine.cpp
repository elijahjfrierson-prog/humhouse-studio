#include "PluginSandbox.h"
#include "Engine.h"
#include <thread>
#include "DrumSynth.h"
#include "AudioEncode.h"
#include <algorithm>
#include <set>
#include <bitset>

namespace hhs
{
// =============================================================== worker pool
WorkerPool::WorkerPool (int n)
{
    for (int i = 0; i < n; ++i)
    {
        auto& t = threads.emplace_back (std::make_unique<Worker> (*this));
        // real-time priority so a busy machine (OBS, browsers, screen capture) can't preempt a worker mid-block
        if (! t->startRealtimeThread (juce::Thread::RealtimeOptions{}.withPriority (10)))
            t->startThread (juce::Thread::Priority::highest);
    }
}
void WorkerPool::setWorkgroup (const juce::AudioWorkgroup& wg)
{
    { const std::lock_guard<std::mutex> lk (wgMutex); workgroup = wg; }
    wgGeneration.fetch_add (1);
}
WorkerPool::~WorkerPool()
{
    quit = true;
    generation.fetch_add (1);
    cv.notify_all();
    for (auto& t : threads) t->stopThread (4000);
}
void WorkerPool::workerLoop()
{
    juce::FloatVectorOperations::disableDenormalisedNumberSupport();   // denormals in reverb tails cause CPU spikes
    int seen = generation.load();
    int wgSeen = -1;
    juce::WorkgroupToken token;
    auto lastWork = std::chrono::steady_clock::now();
    auto spinFor = std::chrono::microseconds (300);
    while (! quit.load())
    {
        if (const int g = wgGeneration.load(); g != wgSeen)
        {
            wgSeen = g;
            token.reset();
            juce::AudioWorkgroup wg;
            { const std::lock_guard<std::mutex> lk (wgMutex); wg = workgroup; }
            if (wg) wg.join (token);
        }
        while (generation.load (std::memory_order_acquire) == seen && ! quit.load())
        {
            // stay hot while blocks keep arriving (sleep_for can oversleep 1-15 ms on Windows / macOS)
            if (std::chrono::steady_clock::now() - lastWork < spinFor) std::this_thread::yield();   // short spin, then sleep: long spins starve plugin threads + UI
            else
            {
                std::unique_lock<std::mutex> lk (m);
                cv.wait_for (lk, std::chrono::milliseconds (2), [&] { return generation.load() != seen || quit.load(); });
            }
        }
        {
            // stay awake across the gap between audio blocks (a cv wake can land late on a busy machine);
            // once blocks stop arriving (transport idle, device closed) the worker sleeps again
            const auto now = std::chrono::steady_clock::now();
            const auto gap = std::chrono::duration_cast<std::chrono::microseconds> (now - lastWork);
            // only with cores to spare: on 2-core machines a spinning worker starves the UI thread mid-edit
            // never spin across the whole gap: N real-time workers spinning pin N cores, starving the UI and plug-in processes
            juce::ignoreUnused (gap);
            spinFor = std::chrono::microseconds (300);
            lastWork = now;
        }
        seen = generation.load();
        if (quit.load()) return;
        for (;;)
        {
            const int i = next.fetch_add (1);
            if (i >= total.load()) break;
            job.load() (ctx.load(), i);
            workerJobs.fetch_add (1, std::memory_order_relaxed);
            done.fetch_add (1);
        }
    }
}
void WorkerPool::run (int count, Job j, void* c)
{
    if (count <= 0) return;
    if (threads.empty() || count == 1) { for (int i = 0; i < count; ++i) j (c, i); return; }
    runs.fetch_add (1, std::memory_order_relaxed);
    job = j; ctx = c; done = 0; total = count; next = 0;
    generation.fetch_add (1, std::memory_order_release);
    cv.notify_all();
    for (;;)
    {
        const int i = next.fetch_add (1);
        if (i >= count) break;
        j (c, i);
        done.fetch_add (1);
    }
    while (done.load (std::memory_order_acquire) < count) std::this_thread::yield();
    total = 0;
}

// =============================================================== runtimes
struct Event { int offset; int key; float vel; bool on; bool slide; double startSec = 0.0; };

struct Engine::ChannelRuntime
{
    int id = 0;
    juce::String signature;
    std::unique_ptr<ChannelInstrument> inst;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::AudioBuffer<float> buf;
    juce::MidiBuffer midi;
    std::array<Event, 512> events {}; int numEvents = 0;
    struct Active { int key; double remaining; };
    std::array<Active, 128> active {}; int numActive = 0;
    int silentBlocks = 0;
    float gainL = -1.0f, gainR = -1.0f;
    int cutGroup = 0;
    // plug-in instruments: keys we sent a note-on for, so stop / loop / seek / choke can end every one of them
    std::bitset<128> held; bool panic = false, cutTails = false;
    // CUT note ends: a 10 ms fade at the note end (no click), then all-sound-off once it's silent
    float cutGain = 1.0f, cutTarget = 1.0f; bool soundOffNext = false;
    std::array<std::pair<int, float>, 64> gainEv {}; int numGainEv = 0;
    void gainAt (int at, float g) { if (numGainEv < (int) gainEv.size()) gainEv[(size_t) numGainEv++] = { at, g }; }
    void addEvent (const Event& e) { if (numEvents < (int) events.size()) events[(size_t) numEvents++] = e; }
    bool holds (int key) const { for (int k = 0; k < numActive; ++k) if (active[(size_t) k].key == key) return true; return false; }
};

// plugin delay compensation: a ring buffer that delays a stereo signal by a changing number of samples
static constexpr int kPdcSize = 16384;   // ~0.34 s at 48 kHz
struct PdcDelay
{
    juce::AudioBuffer<float> buf; int w = 0, mask = 0;
    void prepare (int size) { buf.setSize (2, size); buf.clear(); w = 0; mask = size - 1; }
    bool ready() const { return buf.getNumSamples() > 0; }
    void delayInPlace (juce::AudioBuffer<float>& x, int n, int d)
    {
        d = juce::jlimit (0, mask, d);
        for (int c = 0; c < 2; ++c)
        {
            auto* s = x.getWritePointer (c); auto* r = buf.getWritePointer (c);
            for (int i = 0, ww = w; i < n; ++i, ww = (ww + 1) & mask) { r[ww] = s[i]; s[i] = r[(ww - d) & mask]; }
        }
        w = (w + n) & mask;
    }
    void addDelayed (const juce::AudioBuffer<float>& x, juce::AudioBuffer<float>& dst, int n, int d, float g)
    {
        d = juce::jlimit (0, mask, d);
        for (int c = 0; c < 2; ++c)
        {
            auto* s = x.getReadPointer (c); auto* o = dst.getWritePointer (c); auto* r = buf.getWritePointer (c);
            for (int i = 0, ww = w; i < n; ++i, ww = (ww + 1) & mask) { r[ww] = s[i]; o[i] += g * r[(ww - d) & mask]; }
        }
        w = (w + n) & mask;
    }
};

struct FxRuntime
{
    int uid = 0;
    juce::String signature;
    std::unique_ptr<BuiltinEffect> builtin;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::AudioBuffer<float> buf;   // sized to the plugin's own channel count
    PdcDelay dryDelay;              // Mix knob: dry waits for a latent wet, so blending never comb-filters
    juce::MidiBuffer midi;
    double tail = 0.0;
    std::atomic<float> load { 0.0f };
    int deadSamples = 0;               // sound goes in, (near) silence or garbage comes out: the dry sound passes instead
    int quietSamples = 0;              // smart disable: silence in for longer than its tail -> it isn't run
    std::atomic<bool> silentBypass { false };   // dead for 0.5 s: the plug-in is skipped so it can never mute the song
};

static int pluginChannels (const juce::AudioPluginInstance& p)
{
    return std::max (2, std::max (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()));
}

// Ask for plain stereo main buses with every extra (sidechain / aux) bus off, so the host buffer always
// matches what the plugin will touch. Plugins that refuse keep their default layout; buffers are sized to it.
static void configureBuses (juce::AudioPluginInstance& p)
{
    auto layout = p.getBusesLayout();
    for (int i = 1; i < layout.inputBuses.size(); ++i) layout.inputBuses.getReference (i) = juce::AudioChannelSet::disabled();
    for (int i = 1; i < layout.outputBuses.size(); ++i) layout.outputBuses.getReference (i) = juce::AudioChannelSet::disabled();
    auto stereo = layout;
    if (! stereo.inputBuses.isEmpty()) stereo.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    if (! stereo.outputBuses.isEmpty()) stereo.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    if (! p.setBusesLayout (stereo)) p.setBusesLayout (layout);
}

// =============================================================== out-of-process plugin scanning
namespace
{
juce::String enc (const juce::String& s) { return juce::Base64::toBase64 (s); }
juce::String dec (const juce::String& s)
{
    juce::MemoryOutputStream m;
    juce::Base64::convertFromBase64 (m, s.unquoted());
    return m.toString();
}

#if ! JUCE_IOS   // iOS can't launch child processes: AUv3 plug-ins are scanned in-process there
class ChildProcessScanner : public juce::KnownPluginList::CustomScanner
{
public:
    bool findPluginTypesFor (juce::AudioPluginFormat& format, juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override
    {
        const auto out = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getNonexistentChildFile ("hhs-scan", ".xml");
        juce::ChildProcess proc;
        const juce::StringArray args { juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName(),
                                       "--hhs-scan", enc (format.getName()), enc (fileOrIdentifier), enc (out.getFullPathName()) };
        if (! proc.start (args, 0)) return false;
        const auto deadline = juce::Time::getMillisecondCounter() + 120000;
        while (proc.isRunning())
        {
            if (shouldExit() || juce::Time::getMillisecondCounter() > deadline) { proc.kill(); out.deleteFile(); return false; }
            juce::Thread::sleep (20);
        }
        const bool ok = proc.getExitCode() == 0;
        if (ok)
            if (auto xml = juce::XmlDocument::parse (out))
                for (auto* e : xml->getChildIterator())
                {
                    auto d = std::make_unique<juce::PluginDescription>();
                    if (d->loadFromXml (*e)) result.add (d.release());
                }
        out.deleteFile();
        return ok;   // false = plugin crashed or hung while scanning: it lands on the failed list
    }
};
#endif
}

bool isPluginScanChild (const juce::String& cmd) { return cmd.contains ("--hhs-scan"); }

int runPluginScanChild (const juce::String& cmd)
{
   #if JUCE_MAC
    juce::Process::setDockIconVisible (false);
   #endif
    const auto args = juce::StringArray::fromTokens (cmd, true);
    const int i = args.indexOf ("--hhs-scan");
    if (i < 0 || i + 3 >= args.size()) return 2;
    const auto formatName = dec (args[i + 1]), id = dec (args[i + 2]);
    const juce::File out (dec (args[i + 3]));
    juce::AudioPluginFormatManager fm;
    fm.addDefaultFormats();
    juce::XmlElement xml ("PLUGINS");
    for (auto* f : fm.getFormats())
        if (f->getName() == formatName)
        {
            juce::OwnedArray<juce::PluginDescription> found;
            f->findAllTypesForFile (found, id);
            for (auto* d : found) xml.addChildElement (d->createXml().release());
        }
    return xml.writeTo (out) ? 0 : 3;
}

static juce::File pluginLoadMarker()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("HumHouse Studio").getChildFile ("loading-plugin.txt");
}

void Engine::beginRiskyPluginCall (const juce::PluginDescription& d)
{
    auto f = pluginLoadMarker();
    f.getParentDirectory().createDirectory();
    f.replaceWithText (d.fileOrIdentifier + "\n" + d.name);
}

void Engine::endRiskyPluginCall() { pluginLoadMarker().deleteFile(); }

juce::String Engine::recoverFromPluginCrash()
{
    auto f = pluginLoadMarker();
    if (! f.existsAsFile()) return {};
    const auto lines = juce::StringArray::fromLines (f.loadFileAsString());
    f.deleteFile();
    if (lines.isEmpty() || lines[0].isEmpty()) return {};
    knownPlugins.addToBlacklist (lines[0]);
    return lines.size() > 1 && lines[1].isNotEmpty() ? lines[1] : lines[0];
}

struct Engine::TrackRuntime
{
    juce::AudioBuffer<float> in, out, dry;
    PdcDelay inDelay; std::vector<PdcDelay> edge;   // per sources[] entry
    int ownLatency = 0, inputAlign = 0, arrival = 0;   // samples: this insert's FX, latest input, output
    std::atomic<float> level { 0.0f };   // post-FX pre-fader block peak (sidechain source)
    float duckGain = 1.0f;
    float gainL = -1.0f, gainR = -1.0f;   // last fader gain: ramps, so automation / fader moves never zipper
    std::vector<std::shared_ptr<FxRuntime>> fx;
    std::atomic<float> peak { 0.0f };
    std::array<float, 256> peakHist {};   // per-block peaks: meters show the block being heard, not the one rendered ahead
    std::atomic<unsigned> peakW { 0 };
    int silentSamples = 0;
    double tailSeconds = 0.0;
    std::unique_ptr<juce::AudioFormatReader> frozen;
    juce::File frozenFile;
    std::array<Biquad, 5> meq;
    std::array<float, 15> meqKey {};
};

// short takes/clips live in RAM: punch-in and jumps play them instantly (no disk buffering wait)
#if JUCE_IOS
static constexpr int maxRamTakeSeconds = 300, maxRamTakes = 3;   // phones: a full session of takes in RAM got the app killed
#else
static constexpr int maxRamTakeSeconds = 1200, maxRamTakes = 64;
#endif

struct MemoryClipReader : juce::AudioFormatReader
{
    explicit MemoryClipReader (juce::AudioFormatReader& src) : juce::AudioFormatReader (nullptr, "HHS memory")
    {
        sampleRate = src.sampleRate; numChannels = juce::jmax (1u, src.numChannels); lengthInSamples = src.lengthInSamples;
        bitsPerSample = 32; usesFloatingPointData = true;
        auto d = std::make_shared<juce::AudioBuffer<float>> ((int) numChannels, (int) lengthInSamples);
        src.read (d.get(), 0, (int) lengthInSamples, 0, true, true);
        data = std::move (d);
    }
    // clips cut from the same file (rolls, splits, chops) share one decoded copy
    explicit MemoryClipReader (const MemoryClipReader& o) : juce::AudioFormatReader (nullptr, "HHS memory"), data (o.data)
    {
        sampleRate = o.sampleRate; numChannels = o.numChannels; lengthInSamples = o.lengthInSamples;
        bitsPerSample = 32; usesFloatingPointData = true;
    }
    bool readSamples (int* const* dest, int numDest, int startOffsetInDest, juce::int64 start, int num) override
    {
        for (int ch = 0; ch < numDest; ++ch)
        {
            if (dest[ch] == nullptr) continue;
            auto* d = reinterpret_cast<float*> (dest[ch]) + startOffsetInDest;
            const auto* src = data->getReadPointer (juce::jmin (ch, data->getNumChannels() - 1));
            for (int i = 0; i < num; ++i) { const auto p = start + i; d[i] = p >= 0 && p < lengthInSamples ? src[p] : 0.0f; }
        }
        return true;
    }
    std::shared_ptr<const juce::AudioBuffer<float>> data;
};

struct Engine::ClipRuntime
{
    int uid = 0;
    juce::File file;
    std::unique_ptr<juce::AudioFormatReader> reader;
    double fileRate = 44100.0;
    juce::int64 length = 0;
    juce::AudioBuffer<float> tmp;
};

static juce::String channelSignature (const Channel& c, double bpm)
{
    return juce::String ((int) c.type) + "|" + c.sampleFile.getFullPathName() + "|" + juce::String (c.builtinDrum) + "|" + c.pluginId
         + "|" + juce::String ((int) c.smpNormalize) + juce::String ((int) c.smpReverse) + juce::String ((int) c.smpInvert) + juce::String ((int) c.smpDeclick) + juce::String ((int) c.smpTrim)
         + "|" + juce::String (c.smpBoostDb) + "|" + juce::String (c.smpWarp) + "|" + juce::String (c.smpWarpAmt)
         + "|" + c.smpLayerFile.getFullPathName() + "|" + juce::String (c.smpLayerMix) + "|" + juce::String (c.smpLayerWarp) + "|" + juce::String (c.smpLayerWarpAmt) + "|" + juce::String (c.smpFadeIn) + "|" + juce::String (c.smpFadeOut) + "|" + juce::String (c.smpStart)
         + "|" + juce::String (c.stretchMode) + "|" + juce::String (c.stretchTime) + "|" + juce::String (c.stretchMode != 0 ? c.pitch : 0.0f)
         + "|" + juce::String (c.type == ChannelType::Sampler && c.tempoSync && sampleTempo (c) > 0.0 ? sampleTempo (c) / bpm : 1.0);
}

// =============================================================== engine
// Pro Tools-style -2.5 dB pan law (sin/cos curve), scaled so a centred pan stays at unity
static inline float panGain (float pan, bool right)
{
    pan = juce::jlimit (-1.0f, 1.0f, pan);
    if (pan == 0.0f) return 1.0f;
    const float th = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    return std::pow (std::max (0.0f, right ? std::sin (th) : std::cos (th)), 2.5f / 3.0103f) * 1.33352f;
}

// TPDF dither (1 LSB) for 16/24-bit exports; digital silence stays silent
static void tpdfDither (juce::AudioBuffer<float>& b, int n, int bits, juce::Random& rnd)
{
    if ((bits != 16 && bits != 24) || b.getMagnitude (0, n) == 0.0f) return;
    const float lsb = 1.0f / (float) (1 << (bits - 1));
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (auto* x = b.getWritePointer (ch), * e = x + n; x < e; ++x) *x += (rnd.nextFloat() - rnd.nextFloat()) * lsb;
}

// what the UI hands the audio thread: built and freed on the UI thread, swapped in with pointer swaps
struct Engine::Staged
{
    bool hasProj = false, hasRt = false, hasHits = false, hasPool = false, hasSolo = false;
    int gen = 0;
    Project proj;
    std::map<int, std::shared_ptr<ChannelRuntime>> channelRt;
    std::vector<ChannelRuntime*> channelOrder;
    std::vector<std::shared_ptr<TrackRuntime>> trackRt;
    std::vector<std::vector<std::shared_ptr<FxRuntime>>> fx;
    std::vector<double> tail;
    std::vector<std::unique_ptr<juce::AudioFormatReader>> frozen;
    std::vector<juce::File> frozenFile;
    std::vector<char> setFrozen, setEdge;
    std::vector<std::vector<PdcDelay>> edges;
    std::map<int, std::shared_ptr<ClipRuntime>> clipRt;
    std::vector<std::vector<int>> levels;
    Sources sources;
    std::vector<char> soloMuted;
    std::vector<float> trackCost, chanCost;
    std::vector<int> chanJobOrder;
    std::map<int, std::shared_ptr<const std::vector<DrumHit>>> drumHits;
    std::unique_ptr<WorkerPool> pool;
};

Engine::Staged& Engine::beginStage()
{
    for (;;)
    {
        int s = handoff.load();
        if ((s == 0 || s == 1 || s == 3) && handoff.compare_exchange_weak (s, 4))
        {
            if (s == 3) *staged = Staged();   // what the audio swapped out (old runtimes, old song copy) dies here, never on the audio thread
            return *staged;                   // s == 1: not taken yet, this edit adds to it
        }
        if (s == 2 || s == 5) std::this_thread::yield();   // the audio thread is mid-swap: microseconds
    }
}

void Engine::endStage()
{
    handoff.store (1);
    // no audio callback running (device closed / switching): adopt here so the next block starts current
    if (juce::Time::getMillisecondCounterHiRes() - lastBlockMs.load() > 250.0) adoptNow();
}

void Engine::adoptNow()
{
    int s = 1;
    if (! handoff.compare_exchange_strong (s, 5)) return;
    if (inBlock.load()) { handoff.store (1); return; }   // a block just started: it adopts instead
    adoptStaged();
    handoff.store (3);
}

void Engine::adoptStaged()
{
    const auto t0 = juce::Time::getHighResolutionTicks();
    auto& s = *staged;
    if (s.hasProj) { std::swap (aproj, s.proj); audioGen = s.gen; }
    if (s.hasHits) drumHits.swap (s.drumHits);
    if (s.hasPool) std::swap (pool, s.pool);
    if (s.hasSolo) soloMuted.swap (s.soloMuted);
    if (s.hasRt)
    {
        channelRt.swap (s.channelRt); channelOrder.swap (s.channelOrder); trackRt.swap (s.trackRt); clipRt.swap (s.clipRt);
        levels.swap (s.levels); sources.swap (s.sources);
        trackCost.swap (s.trackCost); chanCost.swap (s.chanCost); chanJobOrder.swap (s.chanJobOrder);
        for (size_t t = 0; t < trackRt.size(); ++t)
        {
            auto& tr = *trackRt[t];
            if (t < s.fx.size()) { tr.fx.swap (s.fx[t]); tr.tailSeconds = s.tail[t]; }
            if (t < s.setFrozen.size() && s.setFrozen[t]) { std::swap (tr.frozen, s.frozen[t]); std::swap (tr.frozenFile, s.frozenFile[t]); }
            if (t < s.setEdge.size() && s.setEdge[t]) tr.edge.swap (s.edges[t]);
        }
        for (size_t i = 0; i < channelOrder.size() && i < aproj.channels.size(); ++i)
        {
            auto* rt = channelOrder[i];
            if (rt->inst) rt->inst->update (aproj.channels[i]);
            rt->cutGroup = aproj.channels[i].cutGroup;
            rt->cutTails = aproj.channels[i].cutTails;
        }
    }
    const float us = (float) (1.0e6 * juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0));
    swapUsLast = us; if (us > swapUsMax.load()) swapUsMax = us;
}

void Engine::stageProject (Staged& s)
{
    // plugin blobs can be megabytes and the audio never reads them: park them while copying
    std::vector<juce::MemoryBlock*> blobs;
    for (auto& ch : proj.channels) blobs.push_back (&ch.pluginState);
    for (auto& m : proj.mixer) for (auto& f : m.fx) blobs.push_back (&f.pluginState);
    std::vector<juce::MemoryBlock> parked (blobs.size());
    for (size_t i = 0; i < blobs.size(); ++i) parked[i].swapWith (*blobs[i]);
    s.proj = proj;
    s.gen = uiGen;
    for (size_t i = 0; i < blobs.size(); ++i) parked[i].swapWith (*blobs[i]);
    s.hasProj = true;
}

void Engine::publishProject()
{
    // L (listen) only passes input on an armed track, so only arming makes playback live
    if (antState.load() != 0 && std::any_of (proj.lanes.begin(), proj.lanes.end(), [] (const Lane& l) { return l.armed; }))
        antExit (false, false);   // a track was armed: back to real time
    // structure changed and no rebuild yet: the next sync() publishes everything together
    if (proj.mixer.size() != uiTrackRt.size() || proj.channels.size() != uiChannelIds.size()) return;
    for (size_t i = 0; i < proj.channels.size(); ++i) if (proj.channels[i].id != uiChannelIds[i]) return;
    for (size_t t = 0; t < proj.mixer.size(); ++t) if (t < uiFx.size() && proj.mixer[t].fx.size() != uiFx[t].size()) return;
    std::vector<char> solo;
    computeSolo (proj, uiSources, solo);
    auto& s = beginStage();
    stageProject (s);
    s.soloMuted = std::move (solo); s.hasSolo = true;
    endStage();
}

void Engine::pushAuto (int kind, int index, int sub, float v)
{
    int s1, n1, s2, n2;
    autoFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0) { autoWrites[(size_t) s1] = { kind, index, sub, v, audioGen }; autoFifo.finishedWrite (1); }
}

void Engine::tick()
{
    // automation moved knobs on the audio copy: show it in the UI's song
    int s1, n1, s2, n2;
    const int ready = autoFifo.getNumReady();
    autoFifo.prepareToRead (ready, s1, n1, s2, n2);
    auto apply = [this] (const AutoWrite& w) {
        if (w.gen != uiGen) return;   // written while the previous song was still playing
        if (w.kind == AutoInsertVol || w.kind == AutoInsertPan)
        { if (w.index >= 0 && w.index < (int) proj.mixer.size()) (w.kind == AutoInsertVol ? proj.mixer[(size_t) w.index].volume : proj.mixer[(size_t) w.index].pan) = w.value; }
        else if (w.kind == AutoSendLevel)
        { if (w.index >= 0 && w.index < (int) proj.mixer.size()) for (auto& sd : proj.mixer[(size_t) w.index].sends) if (sd.target == w.sub) sd.level = w.value; }
        else if (auto* ch = proj.findChannel (w.sub))
        { if (w.kind == AutoChanVol) ch->volume = w.value; else if (w.kind == AutoChanPan) ch->pan = w.value; else if (w.kind == AutoChanPitch) ch->pitch = w.value; }
    };
    for (int i = 0; i < n1; ++i) apply (autoWrites[(size_t) (s1 + i)]);
    for (int i = 0; i < n2; ++i) apply (autoWrites[(size_t) (s2 + i)]);
    autoFifo.finishedRead (n1 + n2);
    int h = 3;
    if (handoff.compare_exchange_strong (h, 4)) { *staged = Staged(); handoff.store (0); }   // free leftovers promptly
    // value edits made straight on the song (knob drags, MIDI learn...) reach the audio copy within a tick
    if (playing.load() || (++tickCount % 3) == 0) publishProject();
}

struct Engine::AntThread : juce::Thread
{
    Engine& e;
    explicit AntThread (Engine& en) : juce::Thread ("HHS render ahead"), e (en) {}
    void run() override { while (! threadShouldExit()) if (! e.antRenderOnce()) e.antWake.wait (2); }
};

static thread_local bool renderingAhead = false;   // master meters are taken when the block is heard instead

bool Engine::antRenderOnce()
{
    antBusy.store (true);
    struct Idle { std::atomic<bool>& b; ~Idle() { b.store (false); } } idle { antBusy };
    if (antState.load() != 1) return false;
    const int chunk = antTmp.getNumSamples();
    if (chunk <= 0 || antFifo.getNumReady() >= antTarget.load() || antFifo.getFreeSpace() < chunk) return false;
    float* o[2] { antTmp.getWritePointer (0), antTmp.getWritePointer (1) };
    renderingAhead = true;
    processBlock (nullptr, 0, o, 2, chunk);
    renderingAhead = false;
    antBpm = ctx.bpm;
    int s1, n1, s2, n2;
    antFifo.prepareToWrite (chunk, s1, n1, s2, n2);
    for (int ch = 0; ch < 2; ++ch)
    {
        if (n1 > 0) antRing.copyFrom (ch, s1, antTmp, ch, 0, n1);
        if (n2 > 0) antRing.copyFrom (ch, s2, antTmp, ch, n1, n2);
    }
    antFifo.finishedWrite (n1 + n2);
    return true;
}

void Engine::antExit (bool discard, bool wait)
{
    int s = 3; antState.compare_exchange_strong (s, 0);
    s = 1; antState.compare_exchange_strong (s, 2);
    if (discard) antDiscard = true;
    if (! wait) return;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    while (antState.load() != 0)
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        // no device callback is coming to finish the hand-back (device stopped): do it here
        if (now - callbackMs.load() > 100.0 || now - t0 > 1000.0) { antForceRealtime(); break; }
        std::this_thread::sleep_for (std::chrono::microseconds (200));
    }
}

void Engine::antForceRealtime()
{
    int s = 3; antState.compare_exchange_strong (s, 0);
    s = 1; antState.compare_exchange_strong (s, 2);
    while (antBusy.load()) std::this_thread::yield();
    if (antState.load() == 2)
    {
        int s1, n1, s2, n2;
        antFifo.prepareToRead (antFifo.getNumReady(), s1, n1, s2, n2); antFifo.finishedRead (n1 + n2);
        antAheadBeats = 0.0;
        scopeLag().store (0, std::memory_order_relaxed);
        antState.store (0);
    }
}

Engine::Engine()
{
    formats.addDefaultFormats();
   #if ! JUCE_IOS
    knownPlugins.setCustomScanner (std::make_unique<ChildProcessScanner>());
   #endif
    proj = Project::createDefault();
    const int hw = (int) std::thread::hardware_concurrency();
    pool = std::make_unique<WorkerPool> (juce::jlimit (0, 31, hw - 1));
    uiPoolWorkers = pool->size();
    humWaveFactory (0);   // builds the shared wavetables now, not on the audio thread at the first synth swap
    staged = std::make_unique<Staged>();
    publisher = std::make_unique<Publisher> (*this);
    publisher->startTimerHz (30);
    antThread = std::make_unique<AntThread> (*this);
    if (! antThread->startRealtimeThread (juce::Thread::RealtimeOptions{}.withPriority (9))) antThread->startThread (juce::Thread::Priority::highest);
    diskThread.startThread (juce::Thread::Priority::high);
    recordingFolder = proj.recordingsFolder();
    prepare (44100.0, 512);
}

Engine::~Engine()
{
    alive->store (false);
    deviceManager.removeAudioCallback (this);
    antForceRealtime();
    antThread->signalThreadShouldExit(); antWake.signal(); antThread->stopThread (2000);
    finishRecordingWriters();
    {
        // disk readers unregister from diskThread, which is declared after them and would be destroyed first
        const juce::ScopedLock sl (lock);
        publisher.reset();
        *staged = Staged();
        uiClipRt.clear(); clipRt.clear();
        for (auto& t : trackRt) if (t) t->frozen.reset();
        for (auto& t : uiTrackRt) if (t) t->frozen.reset();
    }
    diskThread.stopThread (2000);
}

void Engine::initialiseAudio (const juce::XmlElement* saved)
{
    deviceManager.initialise (2, 2, saved, true);
    if (saved == nullptr)   // first run: a 512-sample buffer keeps big sessions (and OBS capture) glitch-free; change it in Audio settings
    {
        auto setup = deviceManager.getAudioDeviceSetup();
       #if JUCE_IOS
        if (setup.bufferSize != 256) { setup.bufferSize = 256; deviceManager.setAudioDeviceSetup (setup, true); }   // iPhone: about 5 ms, sing through the FX
       #else
        if (setup.bufferSize < 512) { setup.bufferSize = 512; deviceManager.setAudioDeviceSetup (setup, true); }
       #endif
    }
    if (dualOut.load()) setDualOutput (true);
    deviceManager.addAudioCallback (this);
}

void Engine::setDualOutput (bool on)
{
    dualOut = on;
    if (! on || deviceManager.getCurrentAudioDevice() == nullptr) return;
    auto setup = deviceManager.getAudioDeviceSetup();
    if (deviceManager.getCurrentAudioDevice()->getOutputChannelNames().size() < 4) return;   // a 2-out interface: nothing to open
    juce::BigInteger want = setup.outputChannels; want.setRange (0, 4, true);
    if (want != setup.outputChannels) { setup.outputChannels = want; setup.useDefaultOutputChannels = false; deviceManager.setAudioDeviceSetup (setup, true); }
}

void Engine::newProject (Project p)
{
    if (recording.load()) setRecording (false);
    if (playing.load()) stop();
    playing = false; panicPending = true;
    {
        const juce::ScopedLock sl (lock);
        proj = std::move (p);
        retroTotal = 0; retroRunFrom = 0; retroEndBeat = -1.0;   // a new song never gets the last song's retro audio
        position = 0.0;
    }
    if (onPluginDiscard)
    {
        for (auto& [id, c] : uiChannelRt) if (c && c->plugin) onPluginDiscard (c->plugin.get());
        for (auto& chain : uiFx) for (auto& f : chain) if (f && f->plugin) onPluginDiscard (f->plugin.get());
    }
    {   // everything is rebuilt; the audio keeps the old runtimes until it swaps, then they are freed on this thread
        const juce::ScopedLock sl (lock);
        uiChannelRt.clear(); uiChannelIds.clear(); uiTrackRt.clear(); uiFx.clear(); uiFrozenFile.clear(); uiEdges.clear(); uiClipRt.clear(); uiSources.clear();
        freshLayout = true;
        ++uiGen;
    }
    sync();
}

std::unique_ptr<juce::AudioPluginInstance> Engine::createPlugin (const juce::String& identifier, juce::String& error)
{
    auto desc = knownPlugins.getTypeForIdentifierString (identifier);
    if (desc == nullptr) { error = "Plugin not found - rescan in Plugin House"; return {}; }
    if (knownPlugins.getBlacklistedFiles().contains (desc->fileOrIdentifier))
    {
        error = desc->name + " crashed before and is disabled (Options > Re-enable crashed plugins)";
        return {};
    }
   #if ! JUCE_IOS
    if (pluginSandbox.load())
    {
        juce::String sandboxError;   // helper failed to start: fall back to loading it here
        if (auto sb = createSandboxedPlugin (*desc, sampleRate, maxBlock, sandboxError)) { sb->setPlayHead (this); return sb; }
    }
   #endif
    beginRiskyPluginCall (*desc);
    auto inst = formats.createPluginInstance (*desc, sampleRate, maxBlock, error);
    if (inst)
    {
        configureBuses (*inst);
        inst->setPlayHead (this);
        inst->setRateAndBufferSizeDetails (sampleRate, maxBlock);
        inst->prepareToPlay (sampleRate, maxBlock);
    }
    endRiskyPluginCall();
    return inst;
}

void Engine::sync()
{
    if (playing.load()) { editChase = true; chasePending = true; }
    std::map<int, std::shared_ptr<const std::vector<DrumHit>>> hits;
    {   // Drummer regions: resizing one re-writes its part right away (the fill moves to the new end)
        for (auto& c : proj.clips)
            if (c.isDrummer())
                for (auto& ch : proj.channels)
                    if (ch.id == c.drummer && ch.type == ChannelType::Drummer) hits[c.uid] = drummerHits (ch.drum, proj.bpm, c.length);
    }
    // ---- build new objects outside the audio lock (sample loading / plugin creation can be slow)
    std::map<int, std::shared_ptr<ChannelRuntime>> newC;
    for (auto& ch : proj.channels)
    {
        const auto sig = channelSignature (ch, proj.bpm);
        auto it = uiChannelRt.find (ch.id);
        if (it != uiChannelRt.end() && it->second->signature == sig) continue;
        auto rt = std::make_shared<ChannelRuntime>();
        rt->id = ch.id; rt->signature = sig;
        if (ch.type == ChannelType::Plugin)
        {
            juce::String err;
            rt->plugin = createPlugin (ch.pluginId, err);
            if (rt->plugin && ch.pluginState.getSize() > 0)
            {
                beginRiskyPluginCall (rt->plugin->getPluginDescription());
                rt->plugin->setStateInformation (ch.pluginState.getData(), (int) ch.pluginState.getSize());
                endRiskyPluginCall();
            }
        }
        else
        {
            SharedSample s;
            if (ch.type == ChannelType::Sampler && ch.sampleFile.existsAsFile())
            {
                auto key = ch.sampleFile.getFullPathName();
                auto c = sampleCache.find (key);
                if (c == sampleCache.end()) c = sampleCache.emplace (key, loadSampleFile (ch.sampleFile, sampleRate)).first;
                SharedSample layer;
                if (ch.smpLayerFile.existsAsFile())
                {
                    auto lk = ch.smpLayerFile.getFullPathName();
                    auto lc = sampleCache.find (lk);
                    if (lc == sampleCache.end()) lc = sampleCache.emplace (lk, loadSampleFile (ch.smpLayerFile, sampleRate)).first;
                    layer = lc->second;
                }
                s = processSample (c->second, ch, sampleRate, proj.bpm, layer);
            }
            rt->inst = createInstrument (ch, s, sampleRate);
        }
        const int nch = rt->plugin ? pluginChannels (*rt->plugin) : 2;
        rt->buf.setSize (nch, maxBlock);
        rt->midi.ensureSize (4096);
        newC[ch.id] = std::move (rt);
    }

    std::map<int, std::shared_ptr<FxRuntime>> newFx;
    for (size_t t = 0; t < proj.mixer.size(); ++t)
        for (auto& slot : proj.mixer[t].fx)
        {
            const auto sig = slot.type + "|" + slot.pluginId;
            bool exists = false;
            if (t < uiFx.size())
                for (auto& f : uiFx[t]) if (f->uid == slot.uid && f->signature == sig) exists = true;
            if (exists) continue;
            auto f = std::make_shared<FxRuntime>();
            f->uid = slot.uid; f->signature = sig;
            if (slot.type == "plugin")
            {
                juce::String err;
                f->plugin = createPlugin (slot.pluginId, err);
                if (f->plugin && slot.pluginState.getSize() > 0)
                {
                    beginRiskyPluginCall (f->plugin->getPluginDescription());
                    f->plugin->setStateInformation (slot.pluginState.getData(), (int) slot.pluginState.getSize());
                    endRiskyPluginCall();
                }
                if (f->plugin)
                {
                    f->tail = std::min (10.0, f->plugin->getTailLengthSeconds()) + 1.0;
                    f->buf.setSize (pluginChannels (*f->plugin), maxBlock);
                    f->midi.ensureSize (2048);
                }
            }
            else if ((f->builtin = createBuiltinEffect (slot.type)))
            {
                for (auto& [id, v] : slot.params) f->builtin->setParam (id, v);   // saved knobs first, so prepare() sets up e.g. linear phase off the audio thread
                f->builtin->prepare (sampleRate, maxBlock);
                f->tail = f->builtin->tailSeconds() + 0.5;
            }
            if (! f->dryDelay.ready()) f->dryDelay.prepare (8192);
            newFx[slot.uid] = std::move (f);
        }

    std::map<int, std::shared_ptr<ClipRuntime>> newClips;
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    for (auto& c : proj.clips)
    {
        if (! c.isAudio()) continue;
        auto it = uiClipRt.find (c.uid);
        // a RAM take that is no longer "fresh" (phone memory cap / low-memory warning) goes back to streaming from disk
        if (it != uiClipRt.end() && it->second->file == c.audioFile
            && (freshTakes.count (c.audioFile.getFullPathName()) > 0 || dynamic_cast<MemoryClipReader*> (it->second->reader.get()) == nullptr)) continue;
        auto rt = std::make_shared<ClipRuntime>();
        rt->uid = c.uid; rt->file = c.audioFile;
        // a take you just recorded (punch / loop) is loaded into RAM, so it plays the moment the loop comes back round
        if (freshTakes.count (c.audioFile.getFullPathName()) > 0)
            if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor (c.audioFile) }; r != nullptr && r->lengthInSamples < (juce::int64) (r->sampleRate * maxRamTakeSeconds))
            {
                rt->fileRate = r->sampleRate; rt->length = r->lengthInSamples;
                rt->reader = std::make_unique<MemoryClipReader> (*r);
                rt->tmp.setSize (2, maxBlock * 4 + 8);
                newClips[c.uid] = std::move (rt);
                continue;
            }
        if (auto* r = fm.createReaderFor (c.audioFile))   // every clip streams from its file on disk (as before 1.2.7)
        {
            rt->fileRate = r->sampleRate; rt->length = r->lengthInSamples;
            rt->reader = std::make_unique<juce::BufferingAudioReader> (r, diskThread, (int) (r->sampleRate * 3));
            auto* br = static_cast<juce::BufferingAudioReader*> (rt->reader.get());
            // a clip under (or just ahead of) the playhead - e.g. the take you just stopped - is buffered before it
            // goes live, so it sounds at once instead of a second of silence while the disk thread catches up
            const double pb = positionBeats();
            if (playing.load() && ! c.hidden && ! c.reversed && pb < c.start + c.length && pb > c.start - 2.0)   // stopped (e.g. opening a project): play primes it, no 0.3 s wait per clip
            {
                const auto at = (juce::int64) ((c.sourceOffset + std::max (0.0, pb - c.start) * 60.0 / proj.bpm) * r->sampleRate);
                if (at >= 0 && at < r->lengthInSamples)
                {
                    juce::AudioBuffer<float> prime ((int) r->numChannels, 2048);
                    br->setReadTimeout (300);
                    br->read (&prime, 0, 2048, at, true, r->numChannels > 1);
                }
            }
            br->setReadTimeout (0);
        }
        rt->tmp.setSize (2, maxBlock * 4 + 8);
        newClips[c.uid] = std::move (rt);
    }

    {
        // the one-knob duck keeps following the vocal after you stop recording / disarm, so the mix you heard
        // while tracking is the mix you hear back (it used to only duck while a lane was armed)
        int dsrc = -1;
        for (auto& ln : proj.lanes) if (ln.armed && ln.mixerTrack > 0) { dsrc = ln.mixerTrack; break; }
        if (dsrc < 0)
            for (auto& c : proj.clips)
                if (c.isAudio() && ! c.muted && c.lane >= 0 && c.lane < (int) proj.lanes.size() && proj.lanes[(size_t) c.lane].mixerTrack > 0)
                { dsrc = proj.lanes[(size_t) c.lane].mixerTrack; break; }
        duckAutoSrc = dsrc;
    }
    std::vector<std::unique_ptr<juce::AudioFormatReader>> newFrozen (proj.mixer.size());
    for (size_t t = 0; t < proj.mixer.size(); ++t)
    {
        auto& mt = proj.mixer[t];
        const bool have = t < uiFrozenFile.size() && uiFrozenFile[t] != juce::File() && uiFrozenFile[t] == mt.frozenFile;
        if (mt.frozen && mt.frozenFile.existsAsFile() && ! have)
            if (auto* r = fm.createReaderFor (mt.frozenFile))
            {
                auto* br = new juce::BufferingAudioReader (r, diskThread, (int) (r->sampleRate * 3));
                br->setReadTimeout (0);
                newFrozen[t].reset (br);
            }
    }

    // ---- build everything the swap needs out here: the audio thread then waits only for pointer moves, never for allocations
    const size_t nT = proj.mixer.size();
    std::vector<std::shared_ptr<TrackRuntime>> freshT (nT);
    for (size_t t = uiTrackRt.size(); t < nT; ++t)
    {
        auto rt = std::make_shared<TrackRuntime>();
        rt->in.setSize (2, maxBlock); rt->out.setSize (2, maxBlock); rt->dry.setSize (2, maxBlock);
        rt->inDelay.prepare (kPdcSize);
        freshT[t] = std::move (rt);
    }
    auto newLevels = proj.processingLevels();
    std::vector<std::vector<std::pair<int, float>>> newSources (nT);
    for (int t = 1; t < (int) nT; ++t)
    {
        auto& mt = proj.mixer[(size_t) t];
        for (auto& s : mt.sends) if (s.target >= 0 && s.target < (int) nT) newSources[(size_t) s.target].push_back ({ t, s.level });
        if (mt.routesToMaster) newSources[0].push_back ({ t, 1.0f });
    }
    using EdgeVec = decltype (TrackRuntime::edge);
    std::vector<std::unique_ptr<EdgeVec>> freshEdges (nT);
    for (size_t t = 0; t < nT; ++t)
    {
        const size_t have = t < uiEdges.size() ? uiEdges[t] : 0;
        if (have != newSources[t].size())
        {
            auto e = std::make_unique<EdgeVec> (newSources[t].size());
            for (auto& d : *e) d.prepare (kPdcSize);
            freshEdges[t] = std::move (e);
        }
    }
    std::vector<float> newTrackCost (nT + 1, 0.0f), newChanCost (proj.channels.size(), 0.0f);
    std::vector<int> newJobOrder (proj.channels.size());
    for (size_t i = 0; i < newJobOrder.size(); ++i) newJobOrder[i] = (int) i;

    // ---- the new layout, built from the UI's own view of the runtimes; the audio thread only swaps it in
    std::vector<std::shared_ptr<ChannelRuntime>> discardC;
    std::vector<std::shared_ptr<FxRuntime>> discardFx;
    {
        const juce::ScopedLock sl (lock);   // UI vs device setup (prepareRuntimes); the audio thread never takes it
        std::map<int, std::shared_ptr<ChannelRuntime>> merged;
        for (auto& ch : proj.channels)
        {
            if (auto n = newC.find (ch.id); n != newC.end()) merged[ch.id] = std::move (n->second);
            else if (auto o = uiChannelRt.find (ch.id); o != uiChannelRt.end()) merged[ch.id] = o->second;
        }
        for (auto& [id, rt] : uiChannelRt)
            if (auto m = merged.find (id); rt && (m == merged.end() || m->second != rt)) discardC.push_back (rt);
        std::vector<ChannelRuntime*> order;
        std::vector<int> ids;
        order.reserve (proj.channels.size()); ids.reserve (proj.channels.size());
        for (auto& ch : proj.channels) { order.push_back (merged[ch.id].get()); ids.push_back (ch.id); }
        if (newJobOrder.size() != order.size())
        {
            newJobOrder.resize (order.size());
            for (size_t i = 0; i < newJobOrder.size(); ++i) newJobOrder[i] = (int) i;
            newChanCost.assign (order.size(), 0.0f);
        }

        // tracks: runtimes stay with their mixer slot; FX chains, frozen readers and PDC edges are swapped in by the audio
        std::vector<std::shared_ptr<TrackRuntime>> tracks;
        std::vector<std::vector<std::shared_ptr<FxRuntime>>> chains (nT);
        std::vector<double> tails (nT, 0.5);
        std::vector<std::unique_ptr<juce::AudioFormatReader>> frozenReaders (nT);
        std::vector<juce::File> frozenFiles (nT), nextFrozen = uiFrozenFile;
        nextFrozen.resize (nT);
        std::vector<char> setFrozen (nT, 0), setEdge (nT, 0);
        static const std::vector<std::shared_ptr<FxRuntime>> none;
        for (size_t t = 0; t < nT; ++t)
        {
            auto rt = t < uiTrackRt.size() && uiTrackRt[t] ? uiTrackRt[t] : freshT[t];
            if (! rt)
            {
                rt = std::make_shared<TrackRuntime>();
                rt->in.setSize (2, maxBlock); rt->out.setSize (2, maxBlock); rt->dry.setSize (2, maxBlock);
                rt->inDelay.prepare (kPdcSize);
            }
            const auto& had = t < uiFx.size() ? uiFx[t] : none;
            auto& chain = chains[t];
            chain.reserve (proj.mixer[t].fx.size());
            for (auto& slot : proj.mixer[t].fx)
            {
                std::shared_ptr<FxRuntime> f;
                if (auto n = newFx.find (slot.uid); n != newFx.end()) f = std::move (n->second);
                else for (auto& o : had) if (o && o->uid == slot.uid) { f = o; break; }
                if (f) chain.push_back (std::move (f));
            }
            for (auto& o : had) if (o && std::find (chain.begin(), chain.end(), o) == chain.end()) discardFx.push_back (o);
            for (auto& f : chain) { if (! f->dryDelay.ready()) f->dryDelay.prepare (8192); tails[t] = std::max (tails[t], f->tail); }
            auto& mt = proj.mixer[t];
            if (newFrozen[t]) { frozenReaders[t] = std::move (newFrozen[t]); frozenFiles[t] = mt.frozenFile; nextFrozen[t] = mt.frozenFile; setFrozen[t] = 1; }
            else if (! mt.frozen && nextFrozen[t] != juce::File()) { nextFrozen[t] = juce::File(); setFrozen[t] = 1; }
            tracks.push_back (std::move (rt));
        }
        for (size_t t = nT; t < uiFx.size(); ++t) for (auto& f : uiFx[t]) if (f) discardFx.push_back (f);

        // clips
        std::map<int, std::shared_ptr<ClipRuntime>> mergedClips;
        for (auto& c : proj.clips)
        {
            if (! c.isAudio()) continue;
            if (auto n = newClips.find (c.uid); n != newClips.end()) mergedClips[c.uid] = std::move (n->second);
            else if (auto o = uiClipRt.find (c.uid); o != uiClipRt.end() && o->second) mergedClips[c.uid] = o->second;
        }

        // routing
        std::vector<size_t> edgeCount (nT);
        for (size_t t = 0; t < nT; ++t) { edgeCount[t] = newSources[t].size(); if (freshEdges[t]) setEdge[t] = 1; }
        std::vector<char> solo;
        computeSolo (proj, newSources, solo);

        uiChannelRt = merged; uiChannelIds = ids; uiTrackRt = tracks; uiFx = chains; uiFrozenFile = nextFrozen;
        uiEdges = edgeCount; uiClipRt = mergedClips; uiSources = newSources;

        auto& s = beginStage();
        if (freshLayout)
        {
            s.frozen.clear(); s.frozenFile.clear(); s.setFrozen.clear(); s.edges.clear(); s.setEdge.clear();
            freshLayout = false;
        }
        s.hasRt = s.hasHits = s.hasSolo = true;
        s.channelRt = std::move (merged); s.channelOrder = std::move (order); s.trackRt = std::move (tracks);
        s.fx = std::move (chains); s.tail = std::move (tails);
        // a frozen reader / PDC edge set the audio hasn't picked up yet stays unless this build replaces it
        s.frozen.resize (nT); s.frozenFile.resize (nT); s.setFrozen.resize (nT, 0);
        s.edges.resize (nT); s.setEdge.resize (nT, 0);
        for (size_t t = 0; t < nT; ++t)
        {
            if (setFrozen[t]) { s.frozen[t] = std::move (frozenReaders[t]); s.frozenFile[t] = frozenFiles[t]; s.setFrozen[t] = 1; }
            if (setEdge[t]) { s.edges[t] = std::move (*freshEdges[t]); s.setEdge[t] = 1; }
        }
        s.clipRt = std::move (mergedClips);
        s.levels = std::move (newLevels); s.sources = std::move (newSources); s.soloMuted = std::move (solo);
        s.trackCost = std::move (newTrackCost); s.chanCost = std::move (newChanCost); s.chanJobOrder = std::move (newJobOrder);
        s.drumHits = std::move (hits);
        stageProject (s);
        endStage();
    }
    // close editors before their plugin instance is deleted (it dies with the leftovers once the audio has let go)
    if (onPluginDiscard)
    {
        for (auto& ch : discardC) if (ch && ch->plugin) onPluginDiscard (ch->plugin.get());
        for (auto& f : discardFx) if (f && f->plugin) onPluginDiscard (f->plugin.get());
    }
    sendChangeMessage();
}

void Engine::computeSolo (const Project& p, const Sources& src, std::vector<char>& out) const
{
    const size_t n = p.mixer.size();
    out.assign (n, 0);
    bool any = false;
    for (size_t t = 1; t < n; ++t) any |= p.mixer[t].solo;
    if (! any) return;
    // a track stays audible if it is soloed, feeds a soloed track, or is fed by one
    std::vector<char> keep (n, 0);
    std::function<void (int)> down = [&] (int t) { if (keep[(size_t) t] & 1) return; keep[(size_t) t] |= 1; for (int o : p.outputsOf (t)) down (o); };
    std::function<void (int)> up = [&] (int t) { if (keep[(size_t) t] & 2) return; keep[(size_t) t] |= 2; if ((size_t) t < src.size()) for (auto& s : src[(size_t) t]) up (s.first); };
    for (size_t t = 1; t < n; ++t) if (p.mixer[t].solo) { down ((int) t); keep[t] &= ~1; down ((int) t); up ((int) t); }
    for (size_t t = 1; t < n; ++t) out[t] = keep[t] ? 0 : 1;
}

void Engine::prepare (double sr, int block)
{
    const juce::ScopedLock sl (lock);
    sampleRate = sr; maxBlock = std::max (block, 64);
    scratch.setSize (2, maxBlock); inputScratch.setSize (8, maxBlock);
    antTmp.setSize (2, maxBlock);
    antTarget = juce::jlimit (2 * maxBlock, antFifo.getTotalSize() - 2 * maxBlock, std::max (3 * maxBlock, (int) (0.08 * sr)));   // ~80 ms ahead (grows on dropouts)
    patLen.reserve (std::max<size_t> (1024, proj.patterns.size() * 2));   // the audio thread never grows it
    if (retroBuf.getNumSamples() != (int) (sr * 60.0)) { retroBuf.setSize (2, (int) (sr * 60.0)); retroBuf.clear(); retroWrite = 0; retroTotal = 0; retroRunFrom = 0; retroEndBeat = -1.0; }
    prepareRuntimes();
}

void Engine::prepareRuntimes()
{
    for (auto& [id, rt] : uiChannelRt)
    {
        rt->buf.setSize (rt->buf.getNumChannels(), maxBlock);
        if (rt->inst) rt->inst->prepare (sampleRate, maxBlock);
        if (rt->plugin)
        {
            rt->plugin->releaseResources(); rt->plugin->setRateAndBufferSizeDetails (sampleRate, maxBlock); rt->plugin->prepareToPlay (sampleRate, maxBlock);
            rt->buf.setSize (pluginChannels (*rt->plugin), maxBlock);
        }
    }
    for (auto& t : uiTrackRt) if (t) { t->in.setSize (2, maxBlock); t->out.setSize (2, maxBlock); t->dry.setSize (2, maxBlock); }
    for (auto& chain : uiFx)
    {
        for (auto& f : chain)
        {
            if (f->builtin) f->builtin->prepare (sampleRate, maxBlock);
            if (f->plugin)
            {
                f->plugin->releaseResources(); f->plugin->setRateAndBufferSizeDetails (sampleRate, maxBlock); f->plugin->prepareToPlay (sampleRate, maxBlock);
                f->buf.setSize (pluginChannels (*f->plugin), maxBlock);
            }
        }
    }
    for (auto& [id, c] : uiClipRt) c->tmp.setSize (2, maxBlock * 4 + 8);
    // sample-based instruments must be rebuilt at the new rate
    for (auto& [id, rt] : uiChannelRt) if (rt->inst) rt->signature = {};
}

void Engine::audioDeviceAboutToStart (juce::AudioIODevice* d)
{
    antForceRealtime();
    prepare (d->getCurrentSampleRate(), d->getCurrentBufferSizeSamples());
    recordLatency = juce::jmax (0, d->getInputLatencyInSamples() + d->getOutputLatencyInSamples());
    {
        const juce::ScopedLock sl (lock);
        deviceWorkgroup = d->getWorkgroup();
        if (pool) pool->setWorkgroup (deviceWorkgroup);
    }
    juce::MessageManager::callAsync ([this, a = alive] { if (a->load()) sync(); });
}
void Engine::audioDeviceStopped() { antForceRealtime(); }

double Engine::cpuLoad() const { return std::max ((double) deviceManager.getCpuUsage(), (double) renderLoad.load()); }   // render-ahead does the work off the device callback

void Engine::setPerformance (int threads, bool multithreaded, bool smartDisable)
{
    const int hw = juce::jmax (1, (int) std::thread::hardware_concurrency());
    const int workers = threads <= 0 ? juce::jlimit (0, 31, hw - 1) : juce::jlimit (0, 63, threads - 1);
    std::unique_ptr<WorkerPool> fresh;
    mtOn = multithreaded; smartOn = smartDisable;
    if (uiPoolWorkers == workers) return;
    uiPoolWorkers = workers;
    fresh = std::make_unique<WorkerPool> (workers);
    fresh->setWorkgroup (deviceWorkgroup);
    auto& s = beginStage();
    s.pool = std::move (fresh); s.hasPool = true;   // the old pool comes back with the leftovers and is joined on this thread
    endStage();
}

void Engine::forEachLiveTake (const std::function<void (int, double, const float*, int, double)>& fn)
{
    if (! recording.load()) return;
    struct Info { int lane; double start; const float* pk; int num; };
    std::vector<Info> live;
    {
        const juce::SpinLock::ScopedTryLockType tl (takeLock);   // brief: copy, then draw outside the lock
        if (! tl.isLocked()) return;
        for (auto& tk : takes) if (tk->started && ! tk->surprise) live.push_back ({ tk->lane, tk->startBeat, tk->peaks.data(), tk->numPeaks.load() });
    }
    const double beatsPerPeak = 256.0 / sampleRate * proj.bpm / 60.0;
    for (auto& i : live) fn (i.lane, i.start, i.pk, i.num, beatsPerPeak);
}

float Engine::fxLoad (int track, int uid) const
{
    if (track < 0 || track >= (int) uiFx.size()) return 0.0f;
    for (auto& f : uiFx[(size_t) track]) if (f->uid == uid) return f->load.load();
    return 0.0f;
}

float Engine::trackPeak (int t) const
{
    if (t < 0 || t >= (int) uiTrackRt.size() || ! uiTrackRt[(size_t) t]) return 0.0f;
    auto& tr = *uiTrackRt[(size_t) t];
    const int lag = scopeLag().load (std::memory_order_relaxed);
    const unsigned w = tr.peakW.load (std::memory_order_acquire);
    if (lag <= 0 || w == 0) return tr.peak.load();
    const unsigned back = (unsigned) juce::jmin (255, lag / juce::jmax (1, antTmp.getNumSamples()));
    return tr.peakHist[(w - 1u - juce::jmin (back, w - 1u)) & 255u];
}

juce::AudioPluginInstance* Engine::channelPlugin (int id)
{
    auto it = uiChannelRt.find (id);
    return it != uiChannelRt.end() ? it->second->plugin.get() : nullptr;
}
juce::AudioPluginInstance* Engine::fxPlugin (int track, int uid)
{
    if (track < 0 || track >= (int) uiFx.size()) return nullptr;
    for (auto& f : uiFx[(size_t) track]) if (f->uid == uid) return f->plugin.get();
    return nullptr;
}
BuiltinEffect* Engine::builtinFx (int track, int uid)
{
    if (track < 0 || track >= (int) uiFx.size()) return nullptr;
    for (auto& f : uiFx[(size_t) track]) if (f->uid == uid) return f->builtin.get();
    return nullptr;
}

void Engine::storePluginStates()
{
    for (auto& ch : proj.channels)
        if (auto* p = channelPlugin (ch.id)) { ch.pluginState.reset(); p->getStateInformation (ch.pluginState); }
    for (size_t t = 0; t < proj.mixer.size(); ++t)
        for (auto& s : proj.mixer[t].fx)
        {
            if (auto* p = fxPlugin ((int) t, s.uid)) { s.pluginState.reset(); p->getStateInformation (s.pluginState); }
            if (auto* b = builtinFx ((int) t, s.uid))
                for (auto& d : b->params()) s.params[d.id] = b->getParam (d.id);
        }
}

// =============================================================== transport
double Engine::loopLength() const { return loopLengthOf (proj); }

double Engine::loopLengthOf (const Project& proj)
{
    if (! proj.songMode)
        return proj.currentPattern >= 0 && proj.currentPattern < (int) proj.patterns.size()
                   ? proj.patterns[(size_t) proj.currentPattern].lengthBeats() : 16.0;
    const double bar = proj.beatsPerBar;
    return std::max (bar, std::ceil (proj.songLengthBeats() / bar - 1.0e-9) * bar);
}

static double startBeat = 0.0;

// Audio clips stream from disk with no wait on the audio thread; load the audio at the play position
// (and each later clip's start) before playback, so the first beat isn't silent.
void Engine::primeClipReaders (double beat)
{
    const auto deadline = juce::Time::getMillisecondCounter() + 800;
    for (auto& clip : proj.clips)
    {
        if (! clip.isAudio() || beat >= clip.start + clip.length) continue;
        auto it = uiClipRt.find (clip.uid);
        if (it == uiClipRt.end() || ! it->second->reader) continue;
        auto* br = dynamic_cast<juce::BufferingAudioReader*> (it->second->reader.get());
        if (br == nullptr || juce::Time::getMillisecondCounter() > deadline) continue;
        const double srcSec = clip.sourceOffset + beatsToSeconds (std::max (0.0, beat - clip.start), proj.bpm);
        juce::AudioBuffer<float> tmp ((int) juce::jmax (1u, br->numChannels), 2048);
        br->setReadTimeout (250);
        br->read (&tmp, 0, 2048, (juce::int64) (srcSec * it->second->fileRate), true, true);
        br->setReadTimeout (0);
    }
}

void Engine::play()
{
    if (playing.load()) return;
    publishProject();   // start from the song as it is right now (armed lanes, punch, loop)
    const bool rec = recording.load(), punch = rec && proj.songMode && proj.hasPunch();
    const double here = position.load();
    const bool loopSel = ! rec && proj.songMode && proj.hasPunch();   // FL: a highlighted range always plays from its start
    const double from = punch ? std::max (0.0, proj.punchIn - proj.preRollBars * proj.beatsPerBar) : loopSel ? proj.punchIn : here;
    primeClipReaders (from);              // load earlier takes/beats where playback really starts (incl. punch pre-roll)
    const bool capture = ! rec && std::any_of (proj.lanes.begin(), proj.lanes.end(), [] (const Lane& l) { return l.armed; });
    if (rec || capture) startRecordingWriters (capture);     // create take files outside the audio lock, so nothing drops out as recording starts
    const bool live = rec || capture;
    if (anticipativeOn.load() && ! live && ! offline.load() && juce::Time::getMillisecondCounterHiRes() - callbackMs.load() < 200.0)
    {
        // nothing live to hear: render playback ~25 ms ahead, so a CPU spike or a slow plugin can't drop out
        antDiscard = true;
        int z = 0;
        if (antState.compare_exchange_strong (z, 3))
        {
            const auto until = juce::Time::getMillisecondCounterHiRes() + 200.0;
            while (antState.load() == 3 && juce::Time::getMillisecondCounterHiRes() < until) std::this_thread::sleep_for (std::chrono::microseconds (200));
            int three = 3; antState.compare_exchange_strong (three, 0);   // no callback answered: stay real time
        }
    }
    const juce::ScopedLock sl (lock);
    if (playing) return;
    if (loopSel) position = from;
    startBeat = position.load();
    if (rec)
    {
        if (punch) position = from;
        countInRemaining = countInOn.load() && ! (proj.songMode && proj.hasPunch()) ? proj.beatsPerBar : 0.0;
        countingIn = countInRemaining > 0.0;
    }
    editChase = false; chasePending = true;
    if (std::abs (std::fmod (position.load() * 4.0, 1.0)) > 1.0e-6) declickGain = 0.0f;   // off the grid: fade in
    playing = true;
}

void Engine::stop()
{
    antExit (true, true);   // back to real time first: what was rendered ahead is dropped
    stopPreview();
    {
        const juce::ScopedLock sl (lock);
        if (! playing && takes.empty()) { position = 0.0; return; }
        playing = false; countingIn = false;
        panicPending = true;   // the audio thread silences every voice at its next block
    }
    lastTakeStart = -1.0;
    finishRecordingWriters();
    if (proj.quickPunch && lastTakeStart >= 0.0) position = startBeat = lastTakeStart;
    else if (proj.returnToStart) position = startBeat;
}

void Engine::setPositionBeats (double b)
{
    position = std::max (0.0, b); startBeat = position; editChase = false; chasePending = true;
    if (antState.load() != 0) antDiscard = true;   // what was rendered ahead belongs to the old spot
    if (playing.load()) declickGain = 0.0f;
}

void Engine::setRecording (bool r)
{
    if (r == recording.load()) return;
    if (r) antExit (false, true);   // recording is live: real time
    publishProject();
    recording = r;
    if (! playing) return;
    finishRecordingWriters();            // punch out (or end the surprise capture that ran before the punch in)
    startRecordingWriters (! r);         // punch in on the fly / after punch out keep capturing surprise takes
}

// =============================================================== recording
void Engine::startRecordingWriters (bool surprise)
{
    std::vector<std::unique_ptr<Take>> newTakes;
    recordingFolder = proj.recordingsFolder();
    recordingFolder.createDirectory();
    for (int l = 0; l < (int) proj.lanes.size(); ++l)
    {
        if (! proj.lanes[(size_t) l].armed) continue;
        auto t = std::make_unique<Take>();
        t->lane = l;
        recordingFolder.createDirectory();
        const auto song = juce::File::createLegalFileName (proj.projectFile.existsAsFile() ? proj.projectFile.getFileNameWithoutExtension() : proj.title).replaceCharacter (' ', '_');
        t->file = recordingFolder.getNonexistentChildFile (proj.lanes[(size_t) l].name.replaceCharacters (" /\\", "___") + "_" + song + "_"
                                                           + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + (surprise ? "_surprise" : "_take"), ".wav");
        t->surprise = surprise;
        juce::WavAudioFormat wav;
        if (auto* os = t->file.createOutputStream().release())
        {
            if (auto* w = wav.createWriterFor (os, sampleRate, 2, 24, {}, 0))
                t->writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (w, diskThread, 65536);
            else delete os;
        }
        if (t->writer) newTakes.push_back (std::move (t));
    }
    const juce::SpinLock::ScopedLockType sl (takeLock);
    takes = std::move (newTakes);
}

bool Engine::retroRecord()
{
    if (proj.lanes.empty()) return false;
    const int cap = retroBuf.getNumSamples();
    const int w = retroWrite.load();
    const double endBeat = retroEndBeat.load();
    const juce::int64 total = retroTotal.load(), runFrom = retroRunFrom.load();
    const int lane = juce::jlimit (0, (int) proj.lanes.size() - 1, retroLane.load());
    // the oldest half second may be overwritten while copying: leave it out
    const int len = (int) std::min<juce::int64> (total, (juce::int64) cap - (juce::int64) (sampleRate * 0.5));
    if (len <= 0) return false;
    juce::AudioBuffer<float> grab (2, len);
    const int start = ((w - len) % cap + cap) % cap;
    const int first = std::min (len, cap - start);
    for (int ch = 0; ch < 2; ++ch)
    {
        grab.copyFrom (ch, 0, retroBuf, ch, start, first);
        if (first < len) grab.copyFrom (ch, first, retroBuf, ch, 0, len - first);
    }
    // keep only what was heard: trim the silence at both ends (-60 dB), with 50 ms of air
    int a = 0, b = len - 1;
    auto loud = [&grab] (int i) { return std::abs (grab.getSample (0, i)) > 0.001f || std::abs (grab.getSample (1, i)) > 0.001f; };
    while (a < len && ! loud (a)) ++a;
    if (a >= len) return false;
    while (b > a && ! loud (b)) --b;
    const int pad = (int) (sampleRate * 0.05);
    a = std::max (0, a - pad); b = std::min (len - 1, b + pad);
    const int keepLen = b - a + 1;

    auto folder = proj.recordingsFolder(); folder.createDirectory();
    const auto song = juce::File::createLegalFileName (proj.projectFile.existsAsFile() ? proj.projectFile.getFileNameWithoutExtension() : proj.title).replaceCharacter (' ', '_');
    const auto file = folder.getNonexistentChildFile (proj.lanes[(size_t) lane].name.replaceCharacters (" /\\", "___") + "_" + song + "_"
                                                      + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + "_retro", ".wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> wr;
        if (auto* os = file.createOutputStream().release())
        {
            wr.reset (wav.createWriterFor (os, sampleRate, 2, 24, {}, 0));
            if (wr == nullptr) delete os;
        }
        if (wr == nullptr || ! wr->writeFromAudioSampleBuffer (grab, a, keepLen)) { wr.reset(); file.deleteFile(); return false; }
    }
    const double spb = sampleRate * 60.0 / proj.bpm;
    Clip c; c.uid = proj.newUid(); c.lane = lane; c.audioFile = file; c.mixerTrack = proj.lanes[(size_t) lane].mixerTrack;
    c.length = keepLen / spb; c.fadeIn = c.fadeOut = 0.02;
    // played along: line it up where it was heard (minus the device latency); stopped: put it at the playhead
    // (only when it was heard in one straight run: a take that crosses a loop jump or a stop goes at the playhead)
    const bool oneRun = endBeat >= 0.0 && total - len + a >= runFrom;
    c.start = oneRun ? endBeat - (double) (len - a + recordLatency.load()) / spb : position.load();
    if (c.start < 0.0) { c.sourceOffset = -c.start * 60.0 / proj.bpm; c.length += c.start; c.start = 0.0; }
    if (c.length < 0.05) { file.deleteFile(); return false; }
    {
        const juce::ScopedLock sl (lock);
        proj.clips.push_back (c);
    }
    rememberTake (file.getFullPathName());
    juce::MessageManager::callAsync ([this, al = alive] { if (! al->load()) return; sync(); primeClipReaders (position.load()); if (onRecordingAdded) onRecordingAdded(); });
    return true;
}

void Engine::finishRecordingWriters()
{
    std::vector<std::unique_ptr<Take>> done;
    {
        const juce::ScopedLock sl (lock);
        const juce::SpinLock::ScopedLockType tl (takeLock);
        std::swap (done, takes);
    }
    if (done.empty()) return;
    bool added = false;
    for (auto& t : done)
    {
        const juce::int64 written = t->samplesWritten;
        t->writer.reset();       // flushes
        rememberTake (t->file.getFullPathName());
        if (written <= 0) { t->file.deleteFile(); continue; }   // nothing was recorded at all
        if (! t->started) continue;                             // never delete a take that holds audio
        if (t->lane < 0 || t->lane >= (int) proj.lanes.size()) continue;   // lane deleted while recording: keep the WAV
        auto& lane = proj.lanes[(size_t) t->lane];
        const int insert = lane.mixerTrack;
        const int group = proj.newUid();
        const double spb = sampleRate * 60.0 / proj.bpm;

        // one clip per loop pass, trimmed to the punch range
        std::vector<std::pair<juce::int64, double>> passes (t->passes.begin(), t->passes.begin() + t->numPasses);
        passes.push_back ({ written, -1.0 });
        std::vector<Clip> newClips;
        for (size_t i = 0; i + 1 < passes.size(); ++i)
        {
            // latency compensation: what you sang at beat b reaches the file `lat` samples later
            const juce::int64 lat = juce::jlimit<juce::int64> (0, written / 2, (juce::int64) recordLatency.load());
            const juce::int64 s0 = passes[i].first + lat, s1 = i + 2 == passes.size() ? written : passes[i + 1].first + lat;
            double b0 = passes[i].second, b1 = b0 + (double) (s1 - s0) / spb;
            double srcOff = (double) s0 / sampleRate;
            if (proj.songMode && proj.hasPunch())
            {
                const double pb0 = std::max (b0, proj.punchIn), pb1 = std::min (b1, proj.punchOut);
                if (pb1 <= pb0 + 1.0e-3) continue;
                srcOff += (pb0 - b0) * 60.0 / proj.bpm;
                b0 = pb0; b1 = pb1;
            }
            if (b1 - b0 < 0.05) continue;
            Clip c; c.uid = proj.newUid(); c.lane = t->lane; c.start = b0; c.length = b1 - b0; c.audioFile = t->file;
            c.sourceOffset = srcOff; c.mixerTrack = insert; c.takeGroup = group;
            c.fadeIn = c.fadeOut = 0.02;
            newClips.push_back (c);
        }
        if (newClips.empty()) continue;
        if (t->surprise)
        {
            float pk = 0.0f;
            for (int k = 0, nk = juce::jmin ((int) t->peaks.size(), t->numPeaks.load()); k < nk; ++k) pk = std::max (pk, t->peaks[(size_t) k]);
            if (pk < 0.02f) { t->file.deleteFile(); continue; }   // nobody sang: no take
            const juce::ScopedLock sl (lock);
            for (auto& c : newClips) { c.hidden = true; proj.clips.push_back (c); }
            added = true;
            continue;
        }
        if (! newClips.empty()) lastTakeStart = lastTakeStart < 0.0 ? newClips.front().start : std::min (lastTakeStart, newClips.front().start);
        if (! proj.hasPunch())
        {
            // a new take never records over an earlier one: if the track is busy there it drops to the next free track
            const double a0 = newClips.front().start, a1 = a0 + newClips.front().length;
            auto overlaps = [&] (int ln) {
                for (auto& c : proj.clips) if (c.lane == ln && c.isAudio() && ! c.hidden && c.start < a1 && c.start + c.length > a0) return true;
                return false; };
            int ln = t->lane;
            while (overlaps (ln)) ++ln;
            if (ln != t->lane)
            {
                const juce::ScopedLock sl (lock);
                while (ln >= (int) proj.lanes.size()) { Lane nl; nl.name = "Track " + juce::String ((int) proj.lanes.size() + 1); nl.mixerTrack = insert; proj.lanes.push_back (nl); }
                for (size_t i = 0; i < newClips.size(); ++i)
                {
                    newClips[i].lane = ln;
                    newClips[i].hidden = i + 1 < newClips.size();
                    proj.clips.push_back (newClips[i]);
                }
                added = true;
                continue;
            }
        }
        // Pro Tools style: a new take lands on its own track over the old one; the old take is cut at the new take's
        // edges (the rest keeps playing) and kept underneath as an alternate
        // punch: cut existing clips on this lane at the new take's range (5 ms crossfade via fades)
        const double r0 = newClips.front().start, r1 = newClips.front().start + newClips.front().length;
        std::vector<Clip> keep;
        for (auto& c : proj.clips)
        {
            if (c.lane != t->lane || ! c.isAudio() || c.hidden || c.start >= r1 || c.start + c.length <= r0) { keep.push_back (c); continue; }
            // the part after punch-out keeps the old uid, so its already-streaming reader keeps playing with no dropout
            if (c.start < r0) { Clip a = c; a.uid = proj.newUid(); a.length = r0 - c.start; a.fadeOut = 0.02; keep.push_back (a); }
            if (c.start + c.length > r1)
            {
                Clip b = c;
                b.sourceOffset += (r1 - c.start) * 60.0 / proj.bpm; b.start = r1; b.length = c.start + c.length - r1; b.fadeIn = 0.02;
                keep.push_back (b);
            }
            Clip hiddenOld = c; hiddenOld.hidden = true; hiddenOld.takeGroup = group; hiddenOld.uid = proj.newUid();
            keep.push_back (hiddenOld);   // old take kept as an alternate
        }
        for (size_t i = 0; i < newClips.size(); ++i)
        {
            newClips[i].hidden = i + 1 < newClips.size(); // last loop pass is the visible keeper
            keep.push_back (newClips[i]);
        }
        {
            const juce::ScopedLock sl (lock);
            proj.clips = std::move (keep);
        }
        added = true;
    }
    if (added)
    {
        juce::MessageManager::callAsync ([this, a = alive] { if (! a->load()) return; sync(); primeClipReaders (position.load()); if (onRecordingAdded) onRecordingAdded(); });
    }
}

// =============================================================== preview
void Engine::previewNote (int channelId, int key, float vel, bool on)
{
    if (on && antState.load() != 0) antExit (false, false);   // you're playing live: no render-ahead delay
    int s1, n1, s2, n2;
    previewFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0) previewEvents[(size_t) s1] = { channelId, key, vel, on };
    else if (n2 > 0) previewEvents[(size_t) s2] = { channelId, key, vel, on };
    previewFifo.finishedWrite (n1 + n2);
}

void Engine::previewFile (const juce::File& f)
{
    if (antState.load() != 0) antExit (false, false);
    auto s = loadSampleFile (f, sampleRate);
    const juce::SpinLock::ScopedLockType sl (previewLock);
    previewSample = s; previewPos = s ? 0 : -1;
    previewStarted = juce::Time::getMillisecondCounter();
}

// =============================================================== audio
juce::Optional<juce::AudioPlayHead::PositionInfo> Engine::getPosition() const
{
    PositionInfo info;
    info.setBpm (aproj.bpm);
    info.setTimeSignature (juce::AudioPlayHead::TimeSignature { aproj.beatsPerBar, 4 });
    info.setPpqPosition (position.load());
    info.setTimeInSeconds (beatsToSeconds (position.load(), aproj.bpm));
    info.setIsPlaying (playing.load());
    info.setIsRecording (recording.load() && playing.load());
    info.setIsLooping (true);
    return info;
}

void Engine::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                               const juce::AudioIODeviceCallbackContext&)
{
    callbackMs = juce::Time::getMillisecondCounterHiRes();
    int st = antState.load();
    if (st == 3) { antPrimed = false; antState.store (1); st = 1; antWake.signal(); }
    if (st == 1 || st == 2)
    {
        juce::ScopedNoDenormals noDenormals;
        int s1, n1, s2, n2;
        if (antDiscard.exchange (false)) { antFifo.prepareToRead (antFifo.getNumReady(), s1, n1, s2, n2); antFifo.finishedRead (n1 + n2); }
        const double spb = sampleRate * 60.0 / juce::jmax (1.0, antBpm.load());
        {   // Retro Record keeps hearing the mic while playback is rendered ahead
            const int c0 = retroC0.load(), c1 = retroC1.load();
            const float* il = c0 >= 0 && c0 < numIn ? in[c0] : nullptr;
            const float* ir = c1 >= 0 && c1 < numIn ? in[c1] : il;
            const bool rolling = playing.load() && ! countingIn.load();
            if (il != nullptr) writeRetro (il, ir, n, rolling ? positionBeats() : -1.0, n / spb, retroRl.load());
        }
        const int ready = antFifo.getNumReady();
        if (st == 1 && ! antPrimed.load() && ready >= antTarget.load()) antPrimed = true;
        const int take = st == 2 || antPrimed.load() ? std::min (ready, n) : 0;
        antFifo.prepareToRead (take, s1, n1, s2, n2);
        for (int ch = 0; ch < numOut; ++ch)
        {
            if (out[ch] == nullptr) continue;
            const bool dual = dualOut.load (std::memory_order_relaxed) && (ch == 2 || ch == 3);
            const int src = ch == 0 || (dual && ch == 2) ? 0 : ch == 1 || (dual && ch == 3) ? 1 : -1;
            if (src < 0) { juce::FloatVectorOperations::clear (out[ch], n); continue; }
            if (n1 > 0) juce::FloatVectorOperations::copy (out[ch], antRing.getReadPointer (src, s1), n1);
            if (n2 > 0) juce::FloatVectorOperations::copy (out[ch] + n1, antRing.getReadPointer (src, s2), n2);
            if (take < n) juce::FloatVectorOperations::clear (out[ch] + take, n - take);
        }
        antFifo.finishedRead (n1 + n2);
        if (st == 1 && antPrimed.load() && take < n)
        {
            ++antUnderruns;
            // adaptive render-ahead: a dropout means this machine needs more cushion for non-live tracks; grow it (up to ~250 ms)
            const int cap = juce::jmin (antFifo.getTotalSize() - 2 * maxBlock, (int) (0.25 * sampleRate));
            if (antTarget.load() < cap) antTarget = juce::jmin (cap, antTarget.load() + 2 * n);
        }
        if (numOut > 0 && out[0] != nullptr)
        {
            const float* hr = numOut > 1 && out[1] != nullptr ? out[1] : out[0];
            masterPeakL = std::max (juce::FloatVectorOperations::findMaximum (out[0], n), -juce::FloatVectorOperations::findMinimum (out[0], n));
            masterPeakR = std::max (juce::FloatVectorOperations::findMaximum (hr, n), -juce::FloatVectorOperations::findMinimum (hr, n));
        }
        scopeLag().store (antFifo.getNumReady(), std::memory_order_relaxed);
        antAheadBeats = (double) antFifo.getNumReady() / spb;
        antWake.signal();
        if (st == 2 && antFifo.getNumReady() == 0 && ! antBusy.load())
        {
            // everything rendered ahead has been heard: carry on in real time from exactly there
            antAheadBeats = 0.0;
            scopeLag().store (0, std::memory_order_relaxed);
            antState.store (0);
            processRealtime (in, numIn, out, numOut, n, take);
        }
        return;
    }
    scopeLag().store (0, std::memory_order_relaxed);
    processRealtime (in, numIn, out, numOut, n, 0);
}

void Engine::processRealtime (const float* const* in, int numIn, float* const* out, int numOut, int n, int from)
{
    int done = from;
    while (done < n)
    {
        const int m = std::min (maxBlock, n - done);
        const float* ins[8] {}; float* outs[8] {};
        for (int i = 0; i < std::min (numIn, 8); ++i) ins[i] = in[i] ? in[i] + done : nullptr;
        for (int i = 0; i < std::min (numOut, 8); ++i) outs[i] = out[i] + done;
        processBlock (ins, std::min (numIn, 8), outs, std::min (numOut, 8), m);
        done += m;
    }
}

void Engine::click (float* l, float* r, int n, double b0, double b1)
{
    const double spb = sampleRate * 60.0 / aproj.bpm;
    const double firstBeat = std::ceil (b0 - 1.0e-9);
    for (double b = firstBeat; b < b1; b += 1.0)
    {
        const int off = (int) ((b - b0) * spb);
        if (off < 0 || off >= n) continue;
        clickLeft = (int) (sampleRate * 0.03);
        clickFreq = ((long) std::llround (b) % aproj.beatsPerBar) == 0 ? 1600.0 : 1000.0;
        clickPhase = 0;
        for (int i = off; i < n && clickLeft > 0; ++i, --clickLeft)
        {
            const float env = (float) clickLeft / (float) (sampleRate * 0.03);
            const float s = 0.35f * env * env * (float) std::sin (clickPhase);
            clickPhase += juce::MathConstants<double>::twoPi * clickFreq / sampleRate;
            l[i] += s; r[i] += s;
        }
        return;
    }
    for (int i = 0; i < n && clickLeft > 0; ++i, --clickLeft)
    {
        const float env = (float) clickLeft / (float) (sampleRate * 0.03);
        const float s = 0.35f * env * env * (float) std::sin (clickPhase);
        clickPhase += juce::MathConstants<double>::twoPi * clickFreq / sampleRate;
        l[i] += s; r[i] += s;
    }
}

// pattern clip level (the dB dot): scales the note velocities of that clip only
static Note clipVel (const Note& n, float g) { if (g == 1.0f) return n; Note c = n; c.velocity = juce::jlimit (0.0f, 1.0f, c.velocity * g); return c; }

void Engine::renderSegment (double b0, double b1, int off, int n)
{
    const double spb = sampleRate * 60.0 / aproj.bpm;
    auto toOffset = [&] (double beat) { return juce::jlimit (off, off + n - 1, off + (int) ((beat - b0) * spb)); };
    auto swingBeat = [&] (double s) {
        if (aproj.swing <= 0.0) return s;
        const double six = s / 0.25;
        return (std::abs (six - std::round (six)) < 1.0e-6 && ((long) std::llround (six) % 2) == 1) ? s + aproj.swing * 0.0833 : s;
    };
    bool anySolo = false;
    for (auto& c : aproj.channels) anySolo |= c.solo;
    const bool laneSolo = std::any_of (aproj.lanes.begin(), aproj.lanes.end(), [] (const Lane& l) { return l.solo; });
    auto laneOff = [&] (int lane) {   // Playlist track M / S
        if (lane < 0 || lane >= (int) aproj.lanes.size()) return false;
        const auto& L = aproj.lanes[(size_t) lane];
        return L.muted || (laneSolo && ! L.solo); };
    // each used pattern's length once per segment, not once per channel (it walks every note)
    patLen.assign (aproj.patterns.size(), -1.0);   // measured on first use, so unused patterns cost nothing
    auto lenOf = [this] (int i) { auto& L = patLen[(size_t) i]; if (L < 0.0) L = aproj.patterns[(size_t) i].lengthBeats(); return L; };

    auto emit = [&] (size_t ci, const Note& note, double absStart, double startSec = 0.0)
    {
        auto* rt = channelOrder[ci];
        const int o = toOffset (absStart);
        // a held note on the same key (e.g. a long 808 tail across the loop) must not cut this new one:
        // end it right here and drop its later note-off
        for (int k = 0; k < rt->numEvents;)
        {
            const auto& ev = rt->events[(size_t) k];
            if (! ev.on && ev.key == note.key && ev.offset >= o) rt->events[(size_t) k] = rt->events[(size_t) --rt->numEvents];
            else ++k;
        }
        for (int k = 0; k < rt->numActive;)
            if (rt->active[(size_t) k].key == note.key) { rt->addEvent ({ o, note.key, 0, false, false }); rt->active[(size_t) k] = rt->active[(size_t) --rt->numActive]; }
            else ++k;
        Event on { o, note.key, note.velocity, true, note.slide }; on.startSec = startSec;
        rt->addEvent (on);
        if (rt->numActive < (int) rt->active.size())
            rt->active[(size_t) rt->numActive++] = { note.key, note.length * spb + (o - off) };
    };
    // FL-style: when playback starts, jumps, loops or the pattern is edited mid-note, the note still sounds
    const bool chase = chasePending.exchange (false);
    const bool quietChase = chase && editChase.exchange (false);
    auto chaseNote = [&] (size_t ci, const Note& note, double absStart, double limit)
    {
        if (absStart >= b0) return;
        auto& c = aproj.channels[ci];
        const bool shot = c.type == ChannelType::Sampler && c.oneShot;   // one-shots play the whole sample, whatever the note length
        const double end = std::min (absStart + note.length, limit);
        if (shot ? (quietChase || b0 - absStart > 64.0) : end <= b0 + 1.0 / 32.0) return;   // after an edit a one-shot is still ringing: never re-hit it
        auto* rt = channelOrder[ci];
        for (int k = 0; k < rt->numActive; ++k) if (rt->active[(size_t) k].key == note.key) return;
        Note tail = note; tail.length = std::max (end - b0, 1.0 / 32.0); tail.slide = false;
        emit (ci, tail, b0, (b0 - absStart) * 60.0 / aproj.bpm);   // samplers start from the matching point in the sample
        if (! quietChase) declickGain = 0.0f;   // starting mid-note: fade in instead of a hard, boxy cut-in (edits: the voice fades itself, the mix keeps going)
    };

    for (size_t ci = 0; ci < aproj.channels.size() && ci < channelOrder.size(); ++ci)
    {
        auto& ch = aproj.channels[ci];
        if (ch.muted || (anySolo && ! ch.solo)) continue;
        const double shiftBeats = (double) ch.shift * 0.001 * aproj.bpm / 60.0;   // channel Shift (ms) in beats
        if (! aproj.songMode)
        {
            if (aproj.currentPattern < 0 || aproj.currentPattern >= (int) aproj.patterns.size()) continue;
            auto& pat = aproj.patterns[(size_t) aproj.currentPattern];
            auto it = pat.notes.find (ch.id);
            if (it == pat.notes.end()) continue;
            const double plen = lenOf (aproj.currentPattern);
            for (auto& note : it->second)
            {
                if (note.muted) continue;
                double s = swingBeat (note.start) + shiftBeats;
                if (s < 0.0) s += plen; else if (s >= plen) s -= plen;   // shifted past the loop edge: wraps like FL
                if (s >= b0 && s < b1) emit (ci, note, s);
                else if (chase) chaseNote (ci, note, s, plen);
            }
        }
        else if (ch.type == ChannelType::Drummer)
        {
            const int srcId = ch.drumLink != 0 ? ch.drumLink : ch.id, mask = ch.drum.groupMask;
            const DrummerParams* src = &ch.drum;
            for (auto& o : aproj.channels) if (o.id == srcId && o.type == ChannelType::Drummer) src = &o.drum;
            const auto silent = [src] (int sec, int g) { return sec >= 0 && sec < (int) src->sectionMute.size() && (src->sectionMute[(size_t) sec] >> g & 1) != 0; };
            for (auto& clip : aproj.clips)
            {
                if (clip.drummer != srcId || clip.muted || clip.hidden) continue;
                if (laneOff (clip.lane)) continue;
                if (clip.start >= b1 || clip.start + clip.length <= b0) continue;
                auto it = drumHits.find (clip.uid);
                if (it == drumHits.end() || ! it->second) continue;
                // hits are sorted: jump to this block instead of walking the whole song's hits every block (x5 when split)
                const auto& hv = *it->second;
                for (auto hi = std::lower_bound (hv.begin(), hv.end(), b0 - clip.start - shiftBeats - 1.0e-9,
                                                 [] (const DrumHit& x, double b) { return x.beat < b; });
                     hi != hv.end(); ++hi)
                {
                    const auto& h = *hi;
                    if (h.beat >= clip.length) break;
                    const double s = clip.start + h.beat + shiftBeats;
                    if (s >= b1) break;
                    if (s < b0) continue;
                    double ss = 0, sl = 32.0;
                    const int g = drumGroupOf (h.lane), sec = drumSectionAt (*src, clip.start + h.beat, &ss, &sl);
                    if ((mask >> g & 1) == 0 || silent (sec, g)) continue;
                    if (auto ms = src->steps.find (sec); ms != src->steps.end() && ms->second[(size_t) g] != 0
                        && ! (src->fills > 0.02f && clip.start + h.beat - ss >= sl - 4.0)) continue;   // your STEPS own that drum; the groove plays around it and fills bar 8
                    Note nt; nt.key = kDrummerLaneKey + h.lane + 32 * (h.variant & 3); nt.start = h.beat; nt.length = 0.125; nt.velocity = juce::jlimit (0.02f, 1.0f, h.vel * drummerDynamics (*src, clip.start + h.beat));
                    emit (ci, nt, s);
                }
                if (src->steps.empty()) continue;
                // manual STEPS sections: their 16-step bar repeats on the exact grid
                for (int n = juce::jmax (0, (int) std::ceil ((b0 - shiftBeats - clip.start) * 4.0 - 1.0e-9)); ; ++n)
                {
                    const double rel = n * 0.25, at = clip.start + rel, s = at + shiftBeats;
                    if (rel >= clip.length || s >= b1) break;
                    if (s < b0) continue;
                    double ss = 0, sl = 32.0;
                    const int sec = drumSectionAt (*src, at, &ss, &sl), step = (int) (std::llround (at * 4.0) % 16);
                    const auto st = src->steps.find (sec);
                    if (st == src->steps.end()) continue;
                    if (src->fills > 0.02f && at - ss >= sl - 4.0) continue;   // bar 8 of the section: the automatic fill
                    for (int g = 0; g < NumDrumGroups; ++g)
                        if ((mask >> g & 1) != 0 && (st->second[(size_t) g] >> step & 1) != 0 && ! silent (sec, g))
                        {
                            Note nt; nt.key = kDrummerLaneKey + drumGroupLane (g); nt.start = rel; nt.length = 0.125;
                            const auto hsh = ((juce::uint32) std::llround (at * 4.0) * 2654435761u + (juce::uint32) g * 40503u) % 1000u;
                            const float acc = step % 8 == 0 ? 0.95f : step % 4 == 0 ? 0.86f : step % 2 == 0 ? 0.74f : 0.6f;   // played, not stamped
                            nt.velocity = juce::jlimit (0.02f, 1.0f, acc * (0.93f + 0.14f * (float) hsh / 1000.0f) * drummerDynamics (*src, at));
                            emit (ci, nt, s);
                        }
                }
            }
        }
        else
        {
            for (auto& clip : aproj.clips)
            {
                if (clip.isAudio() || clip.isDrummer() || clip.patternIndex < 0 || clip.muted || clip.hidden) continue;
                if (laneOff (clip.lane)) continue;
                if (clip.start >= b1 || clip.start + clip.length <= b0) continue;
                if (clip.patternIndex >= (int) aproj.patterns.size()) continue;
                auto& pat = aproj.patterns[(size_t) clip.patternIndex];
                auto it = pat.notes.find (ch.id);
                if (it == pat.notes.end()) continue;
                const double plen = lenOf (clip.patternIndex);
                const double base = clip.start - std::fmod (juce::jmax (0.0, clip.patOffset), plen);   // a sliced clip starts mid-pattern
                const int r0 = std::max (0, (int) std::floor ((b0 - base) / plen) - 1);
                const int r1 = (int) std::floor ((b1 - base) / plen) + 1;
                for (int rep = r0; rep <= r1; ++rep)
                    for (auto& note : it->second)
                    {
                        if (note.muted) continue;
                        const double s = base + rep * plen + swingBeat (note.start) + shiftBeats;
                        if (s >= b0 && s < b1 && s < clip.start + clip.length + juce::jmax (0.0, shiftBeats) && s >= clip.start + juce::jmin (0.0, shiftBeats)) emit (ci, clipVel (note, clip.gain), s);
                        else if (chase && s < b0 && s >= clip.start) chaseNote (ci, clipVel (note, clip.gain), s, clip.start + clip.length);
                    }
            }
        }
    }

    // audio clips -> their insert (FL-style: routing follows the recording)
    if (aproj.songMode)
    {
        for (auto& clip : aproj.clips)
        {
            if (! clip.isAudio() || clip.muted || clip.hidden) continue;
            if (laneOff (clip.lane)) continue;
            const double shB = (clip.lane >= 0 && clip.lane < (int) aproj.lanes.size() ? aproj.lanes[(size_t) clip.lane].shiftMs : 0.0f) * 0.001 * aproj.bpm / 60.0;   // Soft
            const double c0 = std::max (b0, clip.start + shB), c1 = std::min (b1, clip.start + clip.length + shB);
            if (c1 <= c0) continue;
            const float laneGain = clip.lane >= 0 && clip.lane < (int) aproj.lanes.size() ? juce::Decibels::decibelsToGain (aproj.lanes[(size_t) clip.lane].volumeDb, -60.0f) : 1.0f;
            auto it = clipRt.find (clip.uid);
            if (it == clipRt.end() || ! it->second->reader) continue;
            auto& cr = *it->second;
            const int insert = aproj.clipInsert (clip);
            if (insert < 0 || insert >= (int) trackRt.size()) continue;
            auto& dest = trackRt[(size_t) insert]->in;
            const int o0 = off + (int) std::llround ((c0 - b0) * spb);
            const int o1 = std::min (off + n, off + (int) std::llround ((c1 - b0) * spb));
            const int len = o1 - o0;
            if (len <= 0) continue;
            const double ratio = cr.fileRate / sampleRate;
            const double srcSec = clip.sourceOffset + beatsToSeconds (c0 - clip.start - shB, aproj.bpm);
            // exact source position: the fractional part carries into the interpolation, so block edges never drop or repeat a sample
            const double srcPos = srcSec * cr.fileRate;
            const juce::int64 srcStart = (juce::int64) std::floor (srcPos + 1.0e-6);
            const double frac0 = juce::jlimit (0.0, 0.999999, srcPos - (double) srcStart);
            const int need = std::min (cr.tmp.getNumSamples() - 2, (int) std::ceil (len * ratio + frac0) + 2);
            cr.tmp.clear();
            cr.reader->read (&cr.tmp, 0, need + 2, srcStart - 1, true, true);   // one sample of history for 4-point Hermite
            const double spbC = spb;
            for (int i = 0; i < len; ++i)
            {
                const double sp = frac0 + i * ratio;
                const int i0 = (int) sp; const float fr = (float) (sp - i0);
                if (i0 + 1 >= need) break;
                const double beatInClip = (c0 - clip.start) + i / spbC;
                float g = clip.gain * laneGain;
                if (clip.fadeIn > 0 && beatInClip < clip.fadeIn) g *= (float) std::pow (beatInClip / clip.fadeIn, std::exp (clip.fadeCurve));
                const double toEnd = clip.length - beatInClip;
                if (clip.fadeOut > 0 && toEnd < clip.fadeOut) g *= (float) std::pow (std::max (0.0, toEnd / clip.fadeOut), std::exp (clip.fadeCurve));
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* s = cr.tmp.getReadPointer (ch);
                    const float xm = s[i0], x0 = s[i0 + 1], x1 = s[i0 + 2], x2 = s[i0 + 3];   // Hermite instead of linear: no dulled highs / grit when the clip rate differs
                    const float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
                    dest.addSample (ch, o0 + i, (((c3 * fr + c2) * fr + c1) * fr + x0) * g);
                }
            }
        }
        // frozen tracks stream their render
        for (size_t t = 1; t < trackRt.size(); ++t)
        {
            auto& tr = *trackRt[t];
            if (! tr.frozen) continue;
            const juce::int64 src = (juce::int64) (beatsToSeconds (b0, aproj.bpm) * tr.frozen->sampleRate);
            juce::AudioBuffer<float> view (tr.in.getArrayOfWritePointers(), 2, off, n);
            tr.frozen->read (&view, 0, n, src, true, true);
        }
    }
}

struct EngineJobs
{
    static void channel (void* c, int i)
    {
        auto* e = static_cast<Engine*> (c);
        const int ci = i < (int) e->chanJobOrder.size() ? e->chanJobOrder[(size_t) i] : i;
        const auto t0 = juce::Time::getHighResolutionTicks();
        channelJob (e, ci);
        if (ci < (int) e->chanCost.size()) e->chanCost[(size_t) ci] = std::max ((float) (juce::Time::getHighResolutionTicks() - t0), e->chanCost[(size_t) ci] * 0.9f);
    }
    static void applyCutFade (Engine::ChannelRuntime& rt, juce::AudioBuffer<float>& b, int n, double sr)
    {
        const float down = 1.0f / (float) (0.010 * sr), up = 1.0f / (float) (0.002 * sr);
        auto* L = b.getWritePointer (0); auto* R = b.getWritePointer (1);
        int ge = 0;
        for (int i = 0; i < n; ++i)
        {
            while (ge < rt.numGainEv && rt.gainEv[(size_t) ge].first <= i) rt.cutTarget = rt.gainEv[(size_t) ge++].second;
            float& g = rt.cutGain;
            if (g > rt.cutTarget) { g = std::max (rt.cutTarget, g - down); if (g <= 0.0f) rt.soundOffNext = true; }
            else if (g < rt.cutTarget) g = std::min (rt.cutTarget, g + up);
            L[i] *= g; R[i] *= g;
        }
        while (ge < rt.numGainEv) rt.cutTarget = rt.gainEv[(size_t) ge++].second;
        rt.numGainEv = 0;
    }
    static void channelJob (Engine* e, int i)
    {
        auto* rt = e->channelOrder[(size_t) i];
        const int n = e->currentBlock;
        rt->buf.clear (0, n);
        if (rt->plugin)
        {
            rt->midi.clear();
            auto endAll = [rt] (int at) {
                for (int k = 0; k < 128; ++k) if (rt->held[(size_t) k]) rt->midi.addEvent (juce::MidiMessage::noteOff (1, k), at);
                rt->held.reset();
            };
            bool sentOff = rt->panic;
            if (rt->panic)
            {   // many VST3s ignore CC123 alone: explicit note-offs first, then all-notes-off + all-sound-off
                endAll (0);
                rt->midi.addEvent (juce::MidiMessage::allNotesOff (1), 0);
                rt->midi.addEvent (juce::MidiMessage::allSoundOff (1), 0);
                rt->panic = false;
            }
            if (! rt->cutTails) { rt->cutGain = rt->cutTarget = 1.0f; rt->soundOffNext = false; }
            if (rt->soundOffNext) { rt->midi.addEvent (juce::MidiMessage::allSoundOff (1), 0); rt->soundOffNext = false; }
            rt->numGainEv = 0;
            std::sort (rt->events.begin(), rt->events.begin() + rt->numEvents, [] (auto& a, auto& b) { return a.offset != b.offset ? a.offset < b.offset : ! a.on && b.on; });
            for (int k = 0; k < rt->numEvents; ++k)
            {
                auto& ev = rt->events[(size_t) k];
                if (ev.key < 0) { endAll (ev.offset); if (rt->cutTails) rt->gainAt (ev.offset, 0.0f); sentOff = true; continue; }
                if (ev.key > 127) continue;
                if (ev.on) { rt->midi.addEvent (juce::MidiMessage::noteOn (1, ev.key, juce::jlimit (0.0f, 1.0f, ev.vel)), ev.offset); rt->held.set ((size_t) ev.key); if (rt->cutTails) rt->gainAt (ev.offset, 1.0f); continue; }
                rt->midi.addEvent (juce::MidiMessage::noteOff (1, ev.key), ev.offset);
                rt->held.reset ((size_t) ev.key); sentOff = true;
                if (rt->cutTails && rt->held.none()) rt->gainAt (ev.offset, 0.0f);   // CUT: fade out the release tail
            }
            if (rt->numEvents == 0 && rt->numActive == 0 && rt->held.none() && ++rt->silentBlocks > e->idleThresholdBlocks * 4 && e->smartOn.load()) { rt->numEvents = 0; return; }
            if (rt->numEvents > 0) rt->silentBlocks = 0;
            auto& pl = *rt->plugin;
            const juce::ScopedTryLock cl (pl.getCallbackLock());
            if (! cl.isLocked() || pl.isSuspended() || pluginChannels (pl) > rt->buf.getNumChannels() || n > rt->buf.getNumSamples())
            { if (sentOff || rt->held.any()) rt->panic = true; rt->numEvents = 0; return; }   // a dropped note-off would ring forever: end all next block
            juce::AudioBuffer<float> view (rt->buf.getArrayOfWritePointers(), rt->buf.getNumChannels(), n);
            pl.processBlock (view, rt->midi);
            if (pl.getTotalNumOutputChannels() == 1) view.copyFrom (1, 0, view, 0, 0, n);
            if (view.getMagnitude (0, n) > 1.0e-5f) rt->silentBlocks = 0;
            if (rt->cutTails && (rt->numGainEv > 0 || rt->cutGain < 1.0f || rt->cutTarget < 1.0f)) applyCutFade (*rt, view, n, e->sampleRate);
        }
        else if (rt->inst)
        {
            if (rt->numEvents == 0 && ! rt->inst->isActive()) return;
            std::sort (rt->events.begin(), rt->events.begin() + rt->numEvents, [] (auto& a, auto& b) { return a.offset != b.offset ? a.offset < b.offset : ! a.on && b.on; });
            int pos = 0;
            for (int k = 0; k < rt->numEvents; ++k)
            {
                auto& ev = rt->events[(size_t) k];
                if (ev.offset > pos) { rt->inst->render (rt->buf, pos, ev.offset - pos); pos = ev.offset; }
                if (ev.key < 0) rt->inst->choke();
                else if (ev.on) rt->inst->noteOnAt (ev.key, ev.vel, ev.slide, ev.startSec); else rt->inst->noteOff (ev.key);
            }
            if (pos < n) rt->inst->render (rt->buf, pos, n - pos);
        }
        rt->numEvents = 0;
    }

    static void track (void* c, int i)
    {
        auto* e = static_cast<Engine*> (c);
        const int t = e->levels[(size_t) e->currentLevel][(size_t) i];
        const auto t0 = juce::Time::getHighResolutionTicks();
        e->processTrack (t);
        if (t < (int) e->trackCost.size()) e->trackCost[(size_t) t] = std::max ((float) (juce::Time::getHighResolutionTicks() - t0), e->trackCost[(size_t) t] * 0.9f);
    }
};

void Engine::applyMiniEq (TrackRuntime& tr, const MixerTrack& mt, int n)
{
    bool on[5]; bool any = false;
    for (size_t b = 0; b < 5; ++b)
    {
        on[b] = std::abs (mt.meqGain[b]) > 0.01f;
        any |= on[b];
        const float f = juce::jlimit (20.0f, (float) (sampleRate * 0.45), mt.meqFreq[b]), g = mt.meqGain[b], q = juce::jlimit (0.1f, 18.0f, mt.meqQ[b]);
        if (on[b] && (tr.meqKey[b * 3] != f || tr.meqKey[b * 3 + 1] != g || tr.meqKey[b * 3 + 2] != q))
        {
            tr.meq[b].set (b == 0 ? Biquad::LowShelf : b == 4 ? Biquad::HighShelf : Biquad::Peak, sampleRate, f, q, g);
            tr.meqKey[b * 3] = f; tr.meqKey[b * 3 + 1] = g; tr.meqKey[b * 3 + 2] = q;
        }
        else if (! on[b] && tr.meqKey[b * 3 + 1] != 0.0f) { tr.meq[b].clear(); tr.meqKey[b * 3 + 1] = 0.0f; }
    }
    if (! any) return;
    for (int ch = 0; ch < 2; ++ch)
        for (auto* x = tr.out.getWritePointer (ch), * e = x + n; x < e; ++x)
        {
            float v = *x;
            for (size_t b = 0; b < 5; ++b) if (on[b]) v = tr.meq[b].tick (v, ch);
            *x = v;
        }
}

// Link to controller: every block each link reads its controller (Peak Controller value 0..1) and sets the knob
void Engine::applyCtlLinks()
{
    auto fxAt = [this] (int track, int uid) -> BuiltinEffect* {
        if (track < 0 || track >= (int) trackRt.size()) return nullptr;
        for (auto& f : trackRt[(size_t) track]->fx) if (f->uid == uid) return f->builtin.get();
        return nullptr;
    };
    for (auto& L : aproj.ctlLinks)
    {
        auto* src = fxAt (L.srcTrack, L.srcUid);
        auto* dst = fxAt (L.dstTrack, L.dstUid);
        if (src == nullptr || dst == nullptr || src == dst) continue;
        const int i = dst->paramIndex (L.param);
        if (i < 0) continue;
        const auto& d = dst->params()[(size_t) i];
        const float v = juce::jlimit (0.0f, 1.0f, src->meter (0));
        float x = d.min + (L.lo + v * (L.hi - L.lo)) * (d.max - d.min);
        if (! d.choices.empty()) x = std::round (x);
        dst->setParamQuiet (i, juce::jlimit (d.min, d.max, x));
    }
}

void Engine::processTrack (int t)
{
    const int n = currentBlock;
    auto& tr = *trackRt[(size_t) t];
    auto& mt = aproj.mixer[(size_t) t];
    tr.out.makeCopyOf (tr.in, true);
    // delay compensation: everything arriving here is lined up with the latest (most delayed) input
    if (tr.inputAlign > 0 && tr.inDelay.ready()) tr.inDelay.delayInPlace (tr.out, n, tr.inputAlign);
    if (! tr.frozen)
    {
        auto& S = sources[(size_t) t];
        // 64-bit bus summing: many inputs add up in double, then round once (cleaner at high track counts)
        constexpr int kSumMax = 8192;
        static thread_local double acc[2][kSumMax];
        const bool wide = S.size() >= 2 && n <= kSumMax;
        if (wide) for (int ch = 0; ch < 2; ++ch) std::fill (acc[ch], acc[ch] + n, 0.0);
        for (size_t k = 0; k < S.size(); ++k)
        {
            auto& srt = *trackRt[(size_t) S[k].first];
            const int dl = tr.inputAlign - srt.arrival;
            if (dl > 0 && k < tr.edge.size() && tr.edge[k].ready()) tr.edge[k].addDelayed (srt.out, tr.out, n, dl, S[k].second);
            else if (wide)
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float* x = srt.out.getReadPointer (ch); const double g = S[k].second;
                    for (int i = 0; i < n; ++i) acc[ch][i] += g * (double) x[i];
                }
            else for (int ch = 0; ch < 2; ++ch) tr.out.addFrom (ch, 0, srt.out, ch, 0, n, S[k].second);
        }
        if (wide)   // tr.out already holds this track's own input + the delayed (PDC) inputs: add the 64-bit sum on top, never overwrite
            for (int ch = 0; ch < 2; ++ch)
            {
                float* y = tr.out.getWritePointer (ch);
                for (int i = 0; i < n; ++i) y[i] = (float) ((double) y[i] + acc[ch][i]);
            }
    }

    const float mag = tr.out.getMagnitude (0, n);
    if (mag > 1.0e-6f) tr.silentSamples = 0; else tr.silentSamples += n;
    const bool idle = smartOn.load() && tr.silentSamples > (int) (tr.tailSeconds * sampleRate);
    if (! idle && ! tr.frozen)
    {
        juce::AudioBuffer<float> view (tr.out.getArrayOfWritePointers(), 2, n);
        for (size_t k = 0; k < tr.fx.size() && k < mt.fx.size(); ++k)
        {
            auto& slot = mt.fx[k];
            auto& f = *tr.fx[k];
            const float mix = juce::jlimit (0.0f, 1.0f, slot.mix);
            if (slot.bypass || mix <= 0.0f) { f.load = 0.0f; continue; }
            struct LoadMeter { std::atomic<float>& out; juce::int64 t0; double block;
                ~LoadMeter() { const float v = (float) ((double) (juce::Time::getHighResolutionTicks() - t0) / block); out = std::max (v, out.load() * 0.95f); } }
                meterLoad { f.load, juce::Time::getHighResolutionTicks(), juce::jmax (1.0, (double) n / sampleRate * (double) juce::Time::getHighResolutionTicksPerSecond()) };
            const bool blend = mix < 0.999f && n <= tr.dry.getNumSamples();
            if (blend) for (int c = 0; c < 2; ++c) tr.dry.copyFrom (c, 0, view, c, 0, n);
            const int slotLat = f.builtin ? f.builtin->latencySamples() : f.plugin ? f.plugin->getLatencySamples() : 0;
            if (blend && slotLat > 0 && f.dryDelay.ready()) f.dryDelay.delayInPlace (tr.dry, n, slotLat);
            struct Blend { juce::AudioBuffer<float>& v; juce::AudioBuffer<float>& d; int n; float mix; bool on;
                ~Blend() { if (on) for (int c = 0; c < 2; ++c) { v.applyGain (c, 0, n, mix); v.addFrom (c, 0, d, c, 0, n, 1.0f - mix); } } }
                blendOut { view, tr.dry, n, mix, blend };
            if (f.builtin)
            {
                f.builtin->process (view, n, ctx);
                for (int ch = 0; ch < view.getNumChannels(); ++ch)   // one bad effect must never blow up the whole mix / meters
                    for (auto* x = view.getWritePointer (ch), * e = x + n; x < e; ++x) if (! std::isfinite (*x) || std::abs (*x) > 64.0f) *x = 0.0f;
            }
            else if (f.plugin)
            {
                auto& pl = *f.plugin;
                const juce::ScopedTryLock cl (pl.getCallbackLock());
                const int nch = f.buf.getNumChannels(), outs = pl.getTotalNumOutputChannels();
                if (! cl.isLocked() || pl.isSuspended() || outs == 0 || pluginChannels (pl) > nch || n > f.buf.getNumSamples()) continue;
                if (f.silentBypass.load()) continue;
                const float inMag = std::max (view.getMagnitude (0, 0, n), view.getMagnitude (1, 0, n));
                if (inMag < 1.0e-6f) f.quietSamples += n; else f.quietSamples = 0;
                if (f.quietSamples > (int) (juce::jlimit (0.5, 10.0, f.tail + 0.5) * sampleRate)) continue;   // FL-style smart disable: silent in, tail rung out = no CPU
                for (int c = 0; c < nch; ++c)
                    if (c < 2) f.buf.copyFrom (c, 0, view, c, 0, n); else f.buf.clear (c, 0, n);
                if (pl.getMainBusNumInputChannels() == 1)   // mono plugin on a stereo bus: feed it the mono sum, not just the left side
                { f.buf.addFrom (0, 0, view, 1, 0, n); f.buf.applyGain (0, 0, n, 0.5f); }
                juce::AudioBuffer<float> pv (f.buf.getArrayOfWritePointers(), nch, n);
                f.midi.clear();
                pl.processBlock (pv, f.midi);
                const float outMag = std::max (pv.getMagnitude (0, 0, n), pv.getMagnitude (outs > 1 ? 1 : 0, 0, n));
                const bool dead = inMag > 0.01f && ! (outMag >= inMag * 1.0e-3f);   // >60 dB quieter than its input, or NaN
                if (dead) f.deadSamples += n; else f.deadSamples = 0;
                if (f.deadSamples > (int) (0.5 * sampleRate)) { f.silentBypass.store (true); continue; }
                if (! dead)
                {
                    view.copyFrom (0, 0, pv, 0, 0, n);
                    view.copyFrom (1, 0, pv, outs > 1 ? 1 : 0, 0, n);
                    for (int ch = 0; ch < 2; ++ch)
                        for (auto* x = view.getWritePointer (ch), * e = x + n; x < e; ++x) if (! std::isfinite (*x)) *x = 0.0f;
                }
            }
        }
    }
    else if (idle && ! tr.frozen) tr.out.clear (0, n);
    if (! tr.frozen) applyMiniEq (tr, mt, n);

    tr.level.store (std::max (tr.out.getMagnitude (0, 0, n), tr.out.getMagnitude (1, 0, n)));
    // one-knob sidechain duck: this insert dips under its SC source (default: the armed vocal track)
    if (t > 0 && mt.duck > 0.0f)
    {
        int src = mt.duckSource;
        if (src < 0) for (auto& ln : aproj.lanes) if (ln.armed && ln.mixerTrack > 0) { src = ln.mixerTrack; break; }
        if (src < 0) src = duckAutoSrc.load();
        float target = 1.0f;
        if (src > 0 && src != t && src < (int) trackRt.size())
        {
            const float over = juce::Decibels::gainToDecibels (trackRt[(size_t) src]->level.load(), -100.0f) - mt.duckThreshold;
            const float grDb = over > 0.0f ? std::min (over * (1.0f - 1.0f / std::max (1.0f, mt.duckRatio)), 30.0f) * mt.duck : 0.0f;
            target = juce::Decibels::decibelsToGain (-grDb);
        }
        const float a = std::exp (-1.0f / (0.001f * std::max (0.1f, mt.duckAttack) * (float) sampleRate));
        const float r = std::exp (-1.0f / (0.001f * std::max (1.0f, mt.duckRelease) * (float) sampleRate));
        auto* L = tr.out.getWritePointer (0); auto* R = tr.out.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            const float c = target < tr.duckGain ? a : r;
            tr.duckGain = c * tr.duckGain + (1.0f - c) * target;
            L[i] *= tr.duckGain; R[i] *= tr.duckGain;
        }
    }
    else tr.duckGain = 1.0f;

    const bool muted = mt.muted || (t > 0 && t < (int) soloMuted.size() && soloMuted[(size_t) t]);
    const float vol = muted ? 0.0f : (mt.volume / 0.8f) * (mt.volume / 0.8f);
    if (std::abs (mt.stereoSep) > 1.0e-3f)   // M/S stereo separation: scale the side signal (0 = mono .. 2 = twice as wide)
    {
        const float side = 1.0f + juce::jlimit (-1.0f, 1.0f, mt.stereoSep);
        auto* L = tr.out.getWritePointer (0); auto* R = tr.out.getWritePointer (1);
        for (int i = 0; i < n; ++i) { const float m = 0.5f * (L[i] + R[i]), sd = 0.5f * (L[i] - R[i]) * side; L[i] = m + sd; R[i] = m - sd; }
    }
    const float gl = vol * panGain (mt.pan, false), gr = vol * panGain (mt.pan, true);
    if (tr.gainL < 0.0f) { tr.gainL = gl; tr.gainR = gr; }
    tr.out.applyGainRamp (0, 0, n, tr.gainL, gl);
    tr.out.applyGainRamp (1, 0, n, tr.gainR, gr);
    tr.gainL = gl; tr.gainR = gr;
    const float pk = std::max (tr.out.getMagnitude (0, 0, n), tr.out.getMagnitude (1, 0, n));
    tr.peak.store (std::max (pk, tr.peak.load() * 0.85f));
    { const auto w = tr.peakW.load (std::memory_order_relaxed); tr.peakHist[w & 255u] = tr.peak.load(); tr.peakW.store (w + 1, std::memory_order_release); }
    if (auto* sc = trackScope (t)) { sc->rate = sampleRate; sc->push (tr.out.getReadPointer (0), tr.out.getReadPointer (1), n); }
}

void Engine::processBlock (const float* const* in, int numIn, float* const* out, int numOut, int n)
{
    juce::ScopedNoDenormals noDenormals;
    // lock-free: a new song copy / runtime layout the UI staged is swapped in here (pointer swaps only)
    inBlock.store (true);
    struct InBlock { std::atomic<bool>& b; ~InBlock() { b.store (false); } } inBlockGuard { inBlock };
    struct LoadMeter   // real mix cost per block, whichever thread renders it (peak, slow fall)
    {
        Engine& e; int n; double t0;
        ~LoadMeter()
        {
            if (e.offline.load() || n <= 0) return;
            const double raw = (juce::Time::getMillisecondCounterHiRes() - t0) / (1000.0 * n / juce::jmax (1.0, e.sampleRate));
            if (raw >= 1.0) e.dropouts.fetch_add (1, std::memory_order_relaxed);   // a late block = a possible crackle
            const float l = juce::jmin (1.0f, (float) raw);   // a late block reads 100%, never thousands
            const float o = e.renderLoad.load();
            e.renderLoad.store (l > o ? l : o + (l - o) * 0.02f);
        }
    } loadMeter { *this, n, juce::Time::getMillisecondCounterHiRes() };
    if (! offline.load()) lastBlockMs = juce::Time::getMillisecondCounterHiRes();
    int hs = handoff.load();
    if (hs == 1 && handoff.compare_exchange_strong (hs, 2)) { adoptStaged(); handoff.store (3); }
    if (panicPending.exchange (false))
        for (auto* ch : channelOrder)
        {
            if (ch->inst) ch->inst->allNotesOff();
            ch->numActive = 0;
            if (ch->plugin) ch->panic = true;
        }
    if (hs == 5 || trackRt.empty())
    {
        for (int c = 0; c < numOut; ++c) if (out[c]) juce::FloatVectorOperations::clear (out[c], n);
        return;
    }
    struct LoadTrack
    {
        std::atomic<float>& frac; juce::int64 t0; double ticks;
        ~LoadTrack() { const float v = (float) ((double) (juce::Time::getHighResolutionTicks() - t0) / ticks); frac = std::max (v, frac.load (std::memory_order_relaxed) * 0.98f); }
    } loadTrack { blockLoadFrac, juce::Time::getHighResolutionTicks(), juce::jmax (1.0, (double) n / sampleRate * (double) juce::Time::getHighResolutionTicksPerSecond()) };
    currentBlock = n;
    idleThresholdBlocks = std::max (1, (int) (sampleRate * 2.0 / std::max (1, n)));
    for (auto& t : trackRt) t->in.clear (0, n);

    // preview events
    {
        int s1, n1, s2, n2;
        previewFifo.prepareToRead (previewFifo.getNumReady(), s1, n1, s2, n2);
        auto handle = [&] (const PreviewEvent& ev)
        {
            for (auto* rt : channelOrder)
                if (rt->id == ev.channelId && (ev.on || ! playing.load() || ! rt->holds (ev.key)))   // a preview release must not cut a playing note
                    rt->addEvent ({ 0, ev.key, ev.vel, ev.on, false });
        };
        for (int i = 0; i < n1; ++i) handle (previewEvents[(size_t) (s1 + i)]);
        for (int i = 0; i < n2; ++i) handle (previewEvents[(size_t) (s2 + i)]);
        previewFifo.finishedRead (n1 + n2);
    }

    // note-offs for notes running from earlier blocks
    for (auto* rt : channelOrder)
        for (int k = 0; k < rt->numActive;)
        {
            auto& a = rt->active[(size_t) k];
            if (a.remaining < n) { rt->addEvent ({ juce::jlimit (0, n - 1, (int) a.remaining), a.key, 0, false, false }); a = rt->active[(size_t) --rt->numActive]; }
            else { a.remaining -= n; ++k; }
        }

    ctx.bpm = aproj.bpm; ctx.keyRoot = aproj.keyRoot; ctx.scaleIndex = aproj.scaleIndex;
    ctx.playing = playing.load() && ! countingIn.load();
    ctx.beatPos = position.load();
    const double spb = sampleRate * 60.0 / aproj.bpm;
    const double blockBeats = n / spb;
    double clickB0 = 0, clickB1 = 0; bool doClick = false;
    juce::int64 recordFrom = -1; // offset in this block where recording audio starts

    if (playing.load())
    {
        if (countingIn.load())
        {
            const double pre = aproj.beatsPerBar - countInRemaining;
            clickB0 = pre; clickB1 = pre + blockBeats; doClick = true;
            countInRemaining -= blockBeats;
            if (countInRemaining <= 0) countingIn = false;
        }
        else
        {
            double pos = position.load();
            const bool recordSong = recording.load() && aproj.songMode && ! (aproj.hasPunch() && aproj.loopRecord);
            double loopStart = 0.0, loopEnd = loopLengthOf (aproj);
            if (aproj.songMode && aproj.hasPunch() && recording.load() && aproj.loopRecord)
            { loopStart = std::max (0.0, aproj.punchIn - aproj.preRollBars * aproj.beatsPerBar); loopEnd = aproj.punchOut; }
            else if (aproj.songMode && aproj.hasPunch() && ! recording.load() && ! offline.load()) { loopStart = aproj.punchIn; loopEnd = aproj.punchOut; }
            if (metronomeOn.load() || (recording.load() && countInOn.load() && false)) { clickB0 = pos; clickB1 = pos + blockBeats; doClick = metronomeOn.load(); }
            int done = 0;
            recordFrom = 0;
            while (done < n)
            {
                double segEnd = pos + (n - done) / spb;
                bool wrap = false;
                if (! recordSong && segEnd > loopEnd && pos < loopEnd) { segEnd = loopEnd; wrap = true; }
                int len = wrap ? (int) std::ceil ((loopEnd - pos) * spb - 1.0e-9) : n - done;
                len = juce::jlimit (1, n - done, len);
                renderSegment (pos, segEnd, done, len);
                done += len;
                pos = wrap ? loopStart : segEnd;
                if (wrap) { editChase = false; chasePending = true; }
                if (wrap)
                {
                    const juce::SpinLock::ScopedTryLockType tl (takeLock);
                    if (tl.isLocked())
                        for (auto& tk : takes)
                            if (tk->started && tk->numPasses < (int) tk->passes.size())
                                tk->passes[(size_t) tk->numPasses++] = { tk->samplesWritten + done, loopStart };
                }
                if (! recordSong && pos >= loopEnd) pos = loopStart;
            }
            position = pos;
        }
    }

    // automation is read at the end of this block: faders/pans/sends ramp across the block from the value at its
    // first sample to the value at its last, so the curve lands on every sample instead of one block late
    if (ctx.playing) applyAutomation (position.load());
    renderChannels (n);

    // channels -> inserts
    bool anySolo = false;
    for (auto& c : aproj.channels) anySolo |= c.solo;
    for (size_t ci = 0; ci < aproj.channels.size() && ci < channelOrder.size(); ++ci)
    {
        auto& ch = aproj.channels[ci];
        auto* rt = channelOrder[ci];
        const int t = juce::jlimit (0, (int) trackRt.size() - 1, ch.mixerTrack);
        const float v = (ch.volume / 0.8f) * (ch.volume / 0.8f), gl = v * panGain (ch.pan, false), gr = v * panGain (ch.pan, true);
        auto& dest = trackRt[(size_t) t]->in;
        if (rt->gainL < 0.0f) { rt->gainL = gl; rt->gainR = gr; }
        dest.addFromWithRamp (0, 0, rt->buf.getReadPointer (0), n, rt->gainL, gl);
        dest.addFromWithRamp (1, 0, rt->buf.getReadPointer (rt->buf.getNumChannels() > 1 ? 1 : 0), n, rt->gainR, gr);
        rt->gainL = gl; rt->gainR = gr;
    }

    // live input: armed lanes monitor through their insert and record
    const bool rolling = playing.load() && ! countingIn.load();
    if (! aproj.lanes.empty())
    {
        // Retro Record: always keep the last 60 s of the first armed track's input (track 1 if none is armed)
        int rl = 0;
        for (size_t l = 0; l < aproj.lanes.size(); ++l) if (aproj.lanes[l].armed) { rl = (int) l; break; }
        const auto& lane = aproj.lanes[(size_t) rl];
        const int c0 = lane.stereoIn ? lane.inputPair * 2 : lane.inputCh, c1 = lane.stereoIn ? c0 + 1 : c0;
        retroRl = rl; retroC0 = c0; retroC1 = c1;   // the device callback records it itself while playback renders ahead
        const float* il = numIn > 0 && in != nullptr && c0 >= 0 && c0 < numIn ? in[c0] : nullptr;
        const float* ir = numIn > 0 && in != nullptr && c1 >= 0 && c1 < numIn ? in[c1] : il;
        if (il != nullptr) writeRetro (il, ir, n, rolling ? position.load() : -1.0, blockBeats, rl);
    }
    {
        const juce::SpinLock::ScopedTryLockType tl (takeLock);
        for (size_t l = 0; l < aproj.lanes.size(); ++l)
        {
            auto& lane = aproj.lanes[l];
            if (! lane.armed) continue;
            const int c0 = lane.stereoIn ? lane.inputPair * 2 : lane.inputCh, c1 = lane.stereoIn ? c0 + 1 : c0;
            const float* il = c0 >= 0 && c0 < numIn ? in[c0] : nullptr;
            const float* ir = c1 < numIn ? in[c1] : il;
            if (il == nullptr) continue;
            if (ir == nullptr) ir = il;
            if (lane.autoNorm && l < normState.size() && n <= normScratch.getNumSamples())
            {
                // zero-latency auto-normalize: slow RMS leveler (target about -12 dBFS, up to +24 dB) + soft ceiling
                auto& ns = normState[l];
                auto* nl = normScratch.getWritePointer (0); auto* nr = normScratch.getWritePointer (1);
                auto ceil = [] (float y) { const float a = std::abs (y); return a <= 0.89f ? y : std::copysign (0.89f + 0.1f * std::tanh ((a - 0.89f) / 0.1f), y); };
                for (int i = 0; i < n; ++i)
                {
                    const float x = 0.5f * (il[i] + ir[i]);
                    ns.rms = ns.rms * 0.9995f + 0.0005f * x * x;
                    const float level = std::sqrt (ns.rms);
                    if (level > 0.001f)
                    {
                        const float tg = juce::jlimit (0.5f, 16.0f, 0.25f / level);
                        ns.gain += (tg - ns.gain) * (tg < ns.gain ? 0.002f : 0.0002f);
                    }
                    nl[i] = ceil (il[i] * ns.gain); nr[i] = ceil (ir[i] * ns.gain);
                }
                il = nl; ir = nr;
            }
            if (lane.monitor && lane.mixerTrack >= 0 && lane.mixerTrack < (int) trackRt.size())
            {
                auto& dest = trackRt[(size_t) lane.mixerTrack]->in;
                dest.addFrom (0, 0, il, n); dest.addFrom (1, 0, ir, n);
            }
            if (rolling && tl.isLocked())
                for (auto& tk : takes)
                    if (tk->lane == (int) l)
                    {
                        if (! tk->started) { tk->started = true; tk->startBeat = ctx.beatPos; tk->passes[0] = { 0, ctx.beatPos }; tk->numPasses = 1; }
                        const float* chans[2] { il, ir };
                        tk->writer->write (chans, n);
                        tk->samplesWritten += n;
                        for (int i = 0; i < n; ++i)
                        {
                            tk->peakAcc = std::max (tk->peakAcc, std::max (std::abs (il[i]), std::abs (ir[i])));
                            if (++tk->peakN >= 256)
                            {
                                const int k = tk->numPeaks.load();
                                if (k < (int) tk->peaks.size()) { tk->peaks[(size_t) k] = tk->peakAcc; tk->numPeaks = k + 1; }
                                tk->peakAcc = 0; tk->peakN = 0;
                            }
                        }
                    }
        }
    }
    juce::ignoreUnused (recordFrom);

    mixTracks (in, numIn, n);
    if (! aproj.ctlLinks.empty()) applyCtlLinks();

    auto& master = trackRt[0]->out;
    auto* l = master.getWritePointer (0); auto* r = master.getWritePointer (1);
    if (renderTrackOnly > 0 && renderTrackOnly < (int) trackRt.size())
    { l = trackRt[(size_t) renderTrackOnly]->out.getWritePointer (0); r = trackRt[(size_t) renderTrackOnly]->out.getWritePointer (1); }
    if (float gn = declickGain.load(); gn < 1.0f)
    {
        const float step = 1.0f / (float) (0.004 * sampleRate);
        for (int i = 0; i < n; ++i) { l[i] *= gn; r[i] *= gn; gn = std::min (1.0f, gn + step); }
        declickGain = gn;
    }
    if (doClick) click (l, r, n, clickB0, clickB1);

    // browser preview
    {
        const juce::SpinLock::ScopedTryLockType pl (previewLock);
        if (pl.isLocked() && previewSample && previewPos.load() >= 0)
        {
            int p = previewPos.load();
            const int len = std::min (n, previewSample->getNumSamples() - p);
            for (int i = 0; i < len; ++i) { l[i] += previewSample->getSample (0, p + i) * 0.8f; r[i] += previewSample->getSample (1 % previewSample->getNumChannels(), p + i) * 0.8f; }
            int expected = p;   // a stopPreview() from the UI during this block wins
            previewPos.compare_exchange_strong (expected, len < n ? -1 : p + len);
        }
    }

    if (! renderingAhead)
    {
        masterPeakL = std::max (juce::FloatVectorOperations::findMaximum (l, n), -juce::FloatVectorOperations::findMinimum (l, n));
        masterPeakR = std::max (juce::FloatVectorOperations::findMaximum (r, n), -juce::FloatVectorOperations::findMinimum (r, n));
    }
    masterScope().rate = sampleRate;
    masterScope().push (l, r, n);
    for (int c = 0; c < numOut; ++c)
    {
        if (! out[c]) continue;
        const bool dual = dualOut.load(std::memory_order_relaxed) && (c == 2 || c == 3);
        const float* src = c == 0 || (dual && c == 2) ? l : (c == 1 || (dual && c == 3) ? r : nullptr);
        if (src) { juce::FloatVectorOperations::copy (out[c], src, n); juce::FloatVectorOperations::clip (out[c], out[c], -4.0f, 4.0f, n); }
        else juce::FloatVectorOperations::clear (out[c], n);
    }
}

void Engine::writeRetro (const float* il, const float* ir, int n, double newEnd, double blockBeats, int rl)
{
    if (retroBuf.getNumSamples() <= n) return;
    if (ir == nullptr) ir = il;
    auto* bl = retroBuf.getWritePointer (0); auto* br = retroBuf.getWritePointer (1);
    const int cap = retroBuf.getNumSamples();
    int w = retroWrite.load();
    for (int i = 0; i < n; ++i) { bl[w] = il[i]; br[w] = ir[i]; if (++w >= cap) w = 0; }
    const double prevEnd = retroEndBeat.load();
    if (newEnd < 0.0 || prevEnd < 0.0 || std::abs (newEnd - (prevEnd + blockBeats)) > 0.5 * blockBeats)
        retroRunFrom = retroTotal.load();   // stopped, started, looped or seeked: a new straight run
    retroWrite = w; retroTotal += n; retroLane = rl;
    retroEndBeat = newEnd;
}

void Engine::renderChannels (int n)
{
    // cut groups: a hit on one channel chokes every other channel in the same group at that sample offset
    for (auto* a : channelOrder)
    {
        if (a->cutGroup <= 0) continue;
        for (int k = 0, num = a->numEvents; k < num; ++k)
        {
            const auto ev = a->events[(size_t) k];
            if (! ev.on || ev.key < 0) continue;
            for (auto* b : channelOrder) if (b != a && b->cutGroup == a->cutGroup) b->addEvent ({ ev.offset, -1, 0.0f, false, false });
        }
    }

    juce::ignoreUnused (n);
    const int count = (int) channelOrder.size();
    if ((int) chanJobOrder.size() == count && (int) chanCost.size() == count)   // heaviest first = best spread across cores
        std::sort (chanJobOrder.begin(), chanJobOrder.end(), [this] (int a, int b) { return chanCost[(size_t) a] > chanCost[(size_t) b]; });
    if (count >= 2 && mtOn.load() && pool && pool->size() > 0) pool->run (count, &EngineJobs::channel, this);
    else for (int i = 0; i < count; ++i) EngineJobs::channel (this, i);
}

// automation clips (FL): every block, each clip under the playhead sets its knob; the value stays after the clip ends
void Engine::applyAutomation (double beat)
{
    for (auto& c : aproj.clips)
    {
        if (! c.isAuto() || c.muted || c.hidden || c.autoPts.empty() || beat < c.start || beat >= c.start + c.length) continue;
        const float v = autoValueAt (c.autoPts, c.patOffset + beat - c.start);
        const auto& at = c.autoTarget;
        if (at.kind == AutoInsertVol || at.kind == AutoInsertPan)
        {
            if (at.track < 0 || at.track >= (int) aproj.mixer.size()) continue;
            auto& mt = aproj.mixer[(size_t) at.track];
            auto& dst = at.kind == AutoInsertVol ? mt.volume : mt.pan;
            const float x = autoToValue (at.kind, v);
            if (dst != x) { dst = x; pushAuto (at.kind, at.track, 0, x); }
        }
        else if (at.kind == AutoSendLevel)
        {
            if (at.track < 0 || at.track >= (int) aproj.mixer.size()) continue;
            const int bus = at.param.getIntValue();
            const float lv = autoToValue (at.kind, v);
            for (auto& sd : aproj.mixer[(size_t) at.track].sends) if (sd.target == bus && sd.level != lv) { sd.level = lv; pushAuto (at.kind, at.track, bus, lv); }
            if (bus >= 0 && bus < (int) sources.size())
                for (auto& src : sources[(size_t) bus]) if (src.first == at.track) src.second = lv;
        }
        else if (at.kind == AutoChanVol || at.kind == AutoChanPan || at.kind == AutoChanPitch)
        {
            auto* ch = aproj.findChannel (at.channel);
            if (ch == nullptr) continue;
            const float x = autoToValue (at.kind, v);
            if (at.kind == AutoChanVol) { if (ch->volume != x) { ch->volume = x; pushAuto (at.kind, 0, ch->id, x); } }
            else if (at.kind == AutoChanPan) { if (ch->pan != x) { ch->pan = x; pushAuto (at.kind, 0, ch->id, x); } }
            else if (std::abs (ch->pitch - x) > 1.0e-4f)
            {
                ch->pitch = x; pushAuto (at.kind, 0, ch->id, x);
                auto it = channelRt.find (ch->id);
                if (it != channelRt.end() && it->second->inst) it->second->inst->update (*ch);
            }
        }
        else if (at.kind == AutoChanParam)
        {
            auto it = channelRt.find (at.channel);
            if (it == channelRt.end() || ! it->second->plugin || ! at.param.startsWithChar ('#')) continue;
            const auto& ps = it->second->plugin->getParameters();
            const int i = at.param.substring (1).getIntValue();
            if (i >= 0 && i < ps.size() && std::abs (ps[i]->getValue() - v) > 1.0e-5f) ps[i]->setValue (v);
        }
        else if (at.kind == AutoFxParam && at.track >= 0 && at.track < (int) trackRt.size())
        {
            for (auto& f : trackRt[(size_t) at.track]->fx)
            {
                if (f->uid != at.fxUid) continue;
                if (f->builtin)
                {
                    const int i = f->builtin->paramIndex (at.param);
                    if (i < 0) break;
                    const auto& d = f->builtin->params()[(size_t) i];
                    float x = d.min + v * (d.max - d.min);
                    if (! d.choices.empty()) x = std::round (x);
                    f->builtin->setParamQuiet (i, juce::jlimit (d.min, d.max, x));
                }
                else if (f->plugin && at.param.startsWithChar ('#'))
                {
                    const auto& ps = f->plugin->getParameters();
                    const int i = at.param.substring (1).getIntValue();
                    if (i >= 0 && i < ps.size() && std::abs (ps[i]->getValue() - v) > 1.0e-5f) ps[i]->setValue (v);
                }
                break;
            }
        }
    }
}

void Engine::mixTracks (const float* const*, int, int)
{
    // plugin delay compensation (like FL): each insert's latency = its active FX; inputs wait for the slowest path
    for (auto& L : levels)
        for (int t : L)
        {
            if (t < 0 || t >= (int) trackRt.size() || t >= (int) aproj.mixer.size() || t >= (int) sources.size()) continue;
            auto& tr = *trackRt[(size_t) t];
            auto& mt = aproj.mixer[(size_t) t];
            int own = 0;
            if (! tr.frozen)
                for (size_t k = 0; k < tr.fx.size() && k < mt.fx.size(); ++k)
                {
                    if (mt.fx[k].bypass || mt.fx[k].mix <= 0.0f) continue;
                    auto& f = *tr.fx[k];
                    own += f.builtin ? f.builtin->latencySamples() : f.plugin ? f.plugin->getLatencySamples() : 0;
                }
            int in = 0;
            for (auto& sg : sources[(size_t) t]) if (sg.first < (int) trackRt.size()) in = std::max (in, trackRt[(size_t) sg.first]->arrival);
            tr.ownLatency = own; tr.inputAlign = std::min (in, kPdcSize - 1);
            tr.arrival = std::min (tr.inputAlign + std::max (0, own), kPdcSize - 1);
        }
    for (size_t lv = 0; lv < levels.size(); ++lv)
    {
        currentLevel = (int) lv;
        const int count = (int) levels[lv].size();
        auto& L = levels[lv];
        if (count > 2) std::sort (L.begin(), L.end(), [this] (int a, int b) {
            const float ca = a < (int) trackCost.size() ? trackCost[(size_t) a] : 0.0f, cb = b < (int) trackCost.size() ? trackCost[(size_t) b] : 0.0f;
            return ca > cb; });
        if (count >= 2 && mtOn.load() && pool && pool->size() > 0) pool->run (count, &EngineJobs::track, this);
        else for (int i = 0; i < count; ++i) EngineJobs::track (this, i);
    }
}

// =============================================================== offline
static bool writeExportWav (const juce::File& file, const juce::AudioBuffer<float>& mem, double sr, double outRate, int bits, int format = 0)
{
    juce::AudioBuffer<float> res;
    const juce::AudioBuffer<float>* src = &mem;
    if (std::abs (outRate - sr) > 0.5)
    {
        const int outLen = (int) ((double) mem.getNumSamples() * outRate / sr);
        res.setSize (2, juce::jmax (1, outLen));
        for (int ch = 0; ch < 2; ++ch) sincResample (mem.getReadPointer (ch), mem.getNumSamples(), res.getWritePointer (ch), outLen, sr / outRate);
        src = &res;
    }
    if (format == 1) return encodeMp3 (*src, outRate, file, 320);
    if (format == 2) return encodeM4a (*src, outRate, file, 256);
    file.deleteFile();
    const int len = src->getNumSamples();
    juce::AudioBuffer<float> dith;
    if (bits == 16 || bits == 24)
    {
        juce::Random rnd (0x484853);
        dith.makeCopyOf (*src);
        for (int at = 0; at < len; at += 4096) { juce::AudioBuffer<float> v (dith.getArrayOfWritePointers(), 2, at, std::min (4096, len - at)); tpdfDither (v, v.getNumSamples(), bits, rnd); }
        src = &dith;
    }
    if (bits == 64)
    {
        juce::FileOutputStream os (file);
        if (! os.openedOk()) return false;
        const juce::uint32 dataBytes = (juce::uint32) len * 16u;
        os.write ("RIFF", 4); os.writeInt ((int) (36 + dataBytes)); os.write ("WAVE", 4);
        os.write ("fmt ", 4); os.writeInt (16); os.writeShort (3); os.writeShort (2); os.writeInt ((int) outRate);
        os.writeInt ((int) outRate * 16); os.writeShort (16); os.writeShort (64);
        os.write ("data", 4); os.writeInt ((int) dataBytes);
        for (int i = 0; i < len; ++i) { os.writeDouble ((double) src->getSample (0, i)); os.writeDouble ((double) src->getSample (1, i)); }
        os.flush(); return ! os.getStatus().failed();
    }
    juce::WavAudioFormat wav;
    auto os = file.createOutputStream();
    if (! os) return false;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), outRate, 2, bits, {}, 0));
    if (! w) return false;
    os.release();
    return w->writeFromAudioSampleBuffer (*src, 0, len);
}

bool Engine::renderToWav (const juce::File& file, bool song, std::function<bool (float)> progress, int track, int bits, double rate, int format)
{
    format = juce::jlimit (0, 2, format);
    bits = bits == 16 || bits == 32 || bits == 64 ? bits : 24;
    const bool wasPlaying = playing.load();
    if (wasPlaying) stop();
    deviceManager.removeAudioCallback (this);
    antForceRealtime();
    const double sr = sampleRate; const int block = 512;
    const bool oldSong = proj.songMode;
    proj.songMode = song;
    const double lenBeats = renderLenBeats > 0 ? renderLenBeats : song ? std::max (proj.songLengthBeats(), 1.0) : loopLength();
    const double tail = 2.0;
    const juce::int64 total = (juce::int64) ((beatsToSeconds (lenBeats, proj.bpm) + tail) * sr);
    const double outRate = format != 0 ? compressedRate (rate > 0 ? rate : sr) : rate > 0 ? rate : sr;
    const bool viaMemory = format != 0 || bits == 64 || std::abs (outRate - sr) > 0.5;   // 64-bit float / other rates: render, then sinc-convert + write
    juce::AudioBuffer<float> mem;
    if (viaMemory) mem.setSize (2, (int) total);

    file.deleteFile();
    std::unique_ptr<juce::AudioFormatWriter> writer;
    if (! viaMemory)
    {
        juce::WavAudioFormat wav;
        if (auto os = file.createOutputStream())
            if (auto* w = wav.createWriterFor (os.get(), sr, 2, bits, {}, 0)) { os.release(); writer.reset (w); }
    }
    if (! writer && ! viaMemory) { proj.songMode = oldSong; deviceManager.addAudioCallback (this); return false; }

    publishProject();
    adoptNow();   // no callback runs now: take the staged song here
    {
        const juce::ScopedLock sl (lock);
        for (auto& t : trackRt) { t->silentSamples = 0; }
        position = renderLenBeats > 0 ? renderFromBeats : 0.0; playing = true; countingIn = false;
    }
    // export plays the whole song from bar 1 (no selection loop) and waits for every disk read (no silent gaps)
    auto setTimeouts = [this] (int ms) {
        for (auto& [uid, cr] : clipRt) if (auto* br = dynamic_cast<juce::BufferingAudioReader*> (cr->reader.get())) br->setReadTimeout (ms);
        for (auto& t : trackRt) if (auto* br = dynamic_cast<juce::BufferingAudioReader*> (t->frozen.get())) br->setReadTimeout (ms);
    };
    setTimeouts (10000);
    offline = true;
    renderTrackOnly = track;
    const bool rec = recording.load(); recording = false;
    juce::AudioBuffer<float> outBuf (2, block);
    juce::Random ditherRnd (0x484853);
    bool ok = true;
    lastRenderPeak = 0;
    for (juce::int64 done = 0; done < total; done += block)
    {
        const int n = (int) std::min<juce::int64> (block, total - done);
        if (done >= (juce::int64) (beatsToSeconds (lenBeats, proj.bpm) * sr)) playing = false;
        float* outs[2] { outBuf.getWritePointer (0), outBuf.getWritePointer (1) };
        // offline reads must not miss disk data
        processBlock (nullptr, 0, outs, 2, n);
        lastRenderPeak = std::max ({ lastRenderPeak, outBuf.getMagnitude (0, 0, n), outBuf.getMagnitude (1, 0, n) });
        if (viaMemory) { mem.copyFrom (0, (int) done, outBuf, 0, 0, n); mem.copyFrom (1, (int) done, outBuf, 1, 0, n); }
        else { tpdfDither (outBuf, n, bits, ditherRnd); writer->writeFromAudioSampleBuffer (outBuf, 0, n); }
        if (progress && ! progress ((float) done / (float) total)) { ok = false; break; }
    }
    writer.reset();
    if (ok && viaMemory) ok = writeExportWav (file, mem, sr, outRate, bits, format);
    offline = false;
    setTimeouts (0);
    renderTrackOnly = -1;
    recording = rec;
    {
        const juce::ScopedLock sl (lock);
        playing = false; position = 0.0;
        proj.songMode = oldSong;
        for (auto* c : channelOrder) { if (c->inst) c->inst->allNotesOff(); if (c->plugin) c->panic = true; c->numActive = 0; }
    }
    publishProject();
    deviceManager.addAudioCallback (this);
    return ok;
}

bool Engine::consolidateClips (const std::vector<int>& uids, const juce::File& file, double& fromBeat, double& lenBeats)
{
    const std::set<int> want (uids.begin(), uids.end());
    double lo = 1.0e12, hi = -1.0e12;
    for (auto& c : proj.clips)
        if (want.count (c.uid) && ! c.isAuto() && ! c.hidden) { lo = std::min (lo, c.start); hi = std::max (hi, c.start + c.length); }
    if (hi <= lo) return false;
    std::vector<bool> wasMuted;
    for (auto& c : proj.clips) { wasMuted.push_back (c.muted); if (! c.isAuto() && ! want.count (c.uid)) c.muted = true; }
    auto& m = proj.mixer[0];
    const float v = m.volume, pn = m.pan; const bool mu = m.muted;
    std::vector<bool> wasBypassed;
    for (auto& f : m.fx) { wasBypassed.push_back (f.bypass); f.bypass = true; }
    m.volume = std::sqrt (1.0f / 1.5f); m.pan = 0; m.muted = false;
    file.getParentDirectory().createDirectory();
    renderFromBeats = lo; renderLenBeats = hi - lo;
    const bool ok = renderToWav (file, true, {});
    renderFromBeats = 0.0; renderLenBeats = -1.0;
    m.volume = v; m.pan = pn; m.muted = mu;
    for (size_t i = 0; i < m.fx.size() && i < wasBypassed.size(); ++i) m.fx[i].bypass = wasBypassed[i];
    for (size_t i = 0; i < proj.clips.size() && i < wasMuted.size(); ++i) proj.clips[i].muted = wasMuted[i];
    fromBeat = lo; lenBeats = hi - lo;
    return ok;
}

bool Engine::freezeTrack (int track)
{
    if (track <= 0 || track >= (int) proj.mixer.size()) return false;
    auto& mt = proj.mixer[(size_t) track];
    if (mt.frozen) { mt.frozen = false; sync(); return true; }
    auto f = proj.recordingsFolder().getChildFile ("Freeze");
    f.createDirectory();
    auto file = f.getNonexistentChildFile (mt.name.replaceCharacters (" /\\", "___") + "_freeze", ".wav");
    // render pre-fader: temporarily unity volume / centre pan
    const float v = mt.volume, p = mt.pan; const bool m = mt.muted;
    mt.volume = std::sqrt (1.0f / 1.5f); mt.pan = 0; mt.muted = false;
    const bool ok = renderToWav (file, true, {}, track);
    mt.volume = v; mt.pan = p; mt.muted = m;
    if (! ok) return false;
    mt.frozen = true; mt.frozenFile = file;
    sync();
    return true;
}
}

namespace hhs
{
void Engine::rememberTake (const juce::String& path)
{
    if ((int) freshTakes.size() >= maxRamTakes) freshTakes.clear();   // older takes stream from disk on the next sync
    freshTakes.insert (path);
}

void Engine::releaseMemory()
{
    freshTakes.clear();
    for (auto it = sampleCache.begin(); it != sampleCache.end();)
        it = it->second.use_count() <= 1 ? sampleCache.erase (it) : std::next (it);
    sync();
}
}
