#include "Engine.h"
#include <thread>
#include "DrumSynth.h"
#include "AudioEncode.h"
#include <algorithm>

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
            if (std::chrono::steady_clock::now() - lastWork < std::chrono::microseconds (300)) std::this_thread::yield();   // short spin, then sleep: long spins starve plugin threads + UI
            else
            {
                std::unique_lock<std::mutex> lk (m);
                cv.wait_for (lk, std::chrono::milliseconds (2), [&] { return generation.load() != seen || quit.load(); });
            }
        }
        lastWork = std::chrono::steady_clock::now();
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
    int cutGroup = 0;
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
    std::vector<std::unique_ptr<FxRuntime>> fx;
    std::atomic<float> peak { 0.0f };
    int silentSamples = 0;
    double tailSeconds = 0.0;
    std::unique_ptr<juce::AudioFormatReader> frozen;
    juce::File frozenFile;
};

// short takes/clips live in RAM: punch-in and jumps play them instantly (no disk buffering wait)
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
         + "|" + juce::String (c.smpBoostDb) + "|" + juce::String (c.smpFadeIn) + "|" + juce::String (c.smpFadeOut)
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

Engine::Engine()
{
    formats.addDefaultFormats();
    knownPlugins.setCustomScanner (std::make_unique<ChildProcessScanner>());
    proj = Project::createDefault();
    const int hw = (int) std::thread::hardware_concurrency();
    pool = std::make_unique<WorkerPool> (juce::jlimit (0, 31, hw - 1));
    diskThread.startThread (juce::Thread::Priority::high);
    recordingFolder = proj.recordingsFolder();
    prepare (44100.0, 512);
}

Engine::~Engine()
{
    alive->store (false);
    deviceManager.removeAudioCallback (this);
    finishRecordingWriters();
    diskThread.stopThread (2000);
}

void Engine::initialiseAudio (const juce::XmlElement* saved)
{
    deviceManager.initialise (2, 2, saved, true);
    deviceManager.addAudioCallback (this);
}

void Engine::newProject (Project p)
{
    if (recording.load()) setRecording (false);
    if (playing.load()) stop();
    {
        const juce::ScopedLock sl (lock);
        playing = false;
        for (auto& [id, c] : channelRt) if (c->inst) c->inst->allNotesOff();
    }
    std::map<int, std::unique_ptr<ChannelRuntime>> oldC;
    std::vector<std::unique_ptr<TrackRuntime>> oldT;
    std::map<int, std::unique_ptr<ClipRuntime>> oldClips;
    {
        const juce::ScopedLock sl (lock);
        proj = std::move (p);
        std::swap (oldC, channelRt); std::swap (oldT, trackRt); std::swap (oldClips, clipRt);
        channelOrder.clear(); levels.clear(); sources.clear();
        position = 0.0;
    }
    if (onPluginDiscard)
    {
        for (auto& [id, c] : oldC) if (c && c->plugin) onPluginDiscard (c->plugin.get());
        for (auto& t : oldT) if (t) for (auto& f : t->fx) if (f && f->plugin) onPluginDiscard (f->plugin.get());
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
    {   // Drummer regions: resizing one re-writes its part right away (the fill moves to the new end)
        std::map<int, std::shared_ptr<const std::vector<DrumHit>>> hits;
        for (auto& c : proj.clips)
            if (c.isDrummer())
                for (auto& ch : proj.channels)
                    if (ch.id == c.drummer && ch.type == ChannelType::Drummer) hits[c.uid] = drummerHits (ch.drum, proj.bpm, c.length);
        const juce::ScopedLock sl (lock);
        drumHits.swap (hits);
    }
    // ---- build new objects outside the audio lock (sample loading / plugin creation can be slow)
    std::map<int, std::unique_ptr<ChannelRuntime>> newC;
    for (auto& ch : proj.channels)
    {
        const auto sig = channelSignature (ch, proj.bpm);
        auto it = channelRt.find (ch.id);
        if (it != channelRt.end() && it->second->signature == sig) continue;
        auto rt = std::make_unique<ChannelRuntime>();
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
                s = processSample (c->second, ch, sampleRate, proj.bpm);
            }
            rt->inst = createInstrument (ch, s, sampleRate);
        }
        const int nch = rt->plugin ? pluginChannels (*rt->plugin) : 2;
        rt->buf.setSize (nch, maxBlock);
        rt->midi.ensureSize (4096);
        newC[ch.id] = std::move (rt);
    }

    std::map<int, std::unique_ptr<FxRuntime>> newFx;
    for (size_t t = 0; t < proj.mixer.size(); ++t)
        for (auto& slot : proj.mixer[t].fx)
        {
            const auto sig = slot.type + "|" + slot.pluginId;
            bool exists = false;
            if (t < trackRt.size())
                for (auto& f : trackRt[t]->fx) if (f->uid == slot.uid && f->signature == sig) exists = true;
            if (exists) continue;
            auto f = std::make_unique<FxRuntime>();
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
            newFx[slot.uid] = std::move (f);
        }

    std::map<int, std::unique_ptr<ClipRuntime>> newClips;
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::map<juce::String, const MemoryClipReader*> decoded;
    for (auto& [id, rt] : clipRt)
        if (rt) if (auto* m = dynamic_cast<const MemoryClipReader*> (rt->reader.get())) decoded[rt->file.getFullPathName()] = m;
    for (auto& c : proj.clips)
    {
        if (! c.isAudio()) continue;
        auto it = clipRt.find (c.uid);
        if (it != clipRt.end() && it->second->file == c.audioFile) continue;
        auto rt = std::make_unique<ClipRuntime>();
        rt->uid = c.uid; rt->file = c.audioFile;
        if (auto d = decoded.find (c.audioFile.getFullPathName()); d != decoded.end())
        {
            rt->fileRate = d->second->sampleRate; rt->length = d->second->lengthInSamples;
            rt->reader = std::make_unique<MemoryClipReader> (*d->second);
        }
        else if (auto* r = fm.createReaderFor (c.audioFile))
        {
            rt->fileRate = r->sampleRate; rt->length = r->lengthInSamples;
            if (r->lengthInSamples > 0 && r->lengthInSamples < (juce::int64) (r->sampleRate * 360))
            {
                auto m = std::make_unique<MemoryClipReader> (*r);
                decoded[c.audioFile.getFullPathName()] = m.get();
                rt->reader = std::move (m);
                delete r;
            }
            else
            {
                rt->reader = std::make_unique<juce::BufferingAudioReader> (r, diskThread, (int) (r->sampleRate * 3));
                static_cast<juce::BufferingAudioReader*> (rt->reader.get())->setReadTimeout (0);
            }
        }
        rt->tmp.setSize (2, maxBlock * 4 + 8);
        newClips[c.uid] = std::move (rt);
    }

    std::vector<std::unique_ptr<juce::AudioFormatReader>> newFrozen (proj.mixer.size());
    for (size_t t = 0; t < proj.mixer.size(); ++t)
    {
        auto& mt = proj.mixer[t];
        const bool have = t < trackRt.size() && trackRt[t]->frozen && trackRt[t]->frozenFile == mt.frozenFile;
        if (mt.frozen && mt.frozenFile.existsAsFile() && ! have)
            if (auto* r = fm.createReaderFor (mt.frozenFile))
            {
                auto* br = new juce::BufferingAudioReader (r, diskThread, (int) (r->sampleRate * 3));
                br->setReadTimeout (0);
                newFrozen[t].reset (br);
            }
    }

    // ---- swap in under the lock
    std::vector<std::unique_ptr<ChannelRuntime>> discardC;
    std::vector<std::unique_ptr<FxRuntime>> discardFx;
    std::vector<std::unique_ptr<ClipRuntime>> discardClips;
    std::vector<std::unique_ptr<TrackRuntime>> discardT;
    {
        const juce::ScopedLock sl (lock);
        // channels
        std::map<int, std::unique_ptr<ChannelRuntime>> merged;
        for (auto& ch : proj.channels)
        {
            if (auto n = newC.find (ch.id); n != newC.end()) merged[ch.id] = std::move (n->second);
            else if (auto o = channelRt.find (ch.id); o != channelRt.end()) merged[ch.id] = std::move (o->second);
        }
        for (auto& [id, rt] : channelRt) if (rt) discardC.push_back (std::move (rt));
        channelRt = std::move (merged);
        channelOrder.clear();
        for (auto& ch : proj.channels)
        {
            auto* rt = channelRt[ch.id].get();
            if (rt->inst) rt->inst->update (ch);
            rt->cutGroup = ch.cutGroup;
            channelOrder.push_back (rt);
        }
        chanJobOrder.resize (channelOrder.size());
        for (size_t i = 0; i < chanJobOrder.size(); ++i) chanJobOrder[i] = (int) i;
        chanCost.assign (channelOrder.size(), 0.0f);
        trackCost.assign (proj.mixer.size() + 1, 0.0f);

        // tracks
        std::vector<std::unique_ptr<TrackRuntime>> tracks;
        for (size_t t = 0; t < proj.mixer.size(); ++t)
        {
            std::unique_ptr<TrackRuntime> rt;
            if (t < trackRt.size() && trackRt[t]) rt = std::move (trackRt[t]);
            else { rt = std::make_unique<TrackRuntime>(); rt->in.setSize (2, maxBlock); rt->out.setSize (2, maxBlock); rt->dry.setSize (2, maxBlock); }
            std::vector<std::unique_ptr<FxRuntime>> chain;
            for (auto& slot : proj.mixer[t].fx)
            {
                std::unique_ptr<FxRuntime> f;
                if (auto n = newFx.find (slot.uid); n != newFx.end()) f = std::move (n->second);
                else for (auto& o : rt->fx) if (o && o->uid == slot.uid) { f = std::move (o); break; }
                if (f) chain.push_back (std::move (f));
            }
            for (auto& o : rt->fx) if (o) discardFx.push_back (std::move (o));
            rt->fx = std::move (chain);
            for (auto& f : rt->fx) if (! f->dryDelay.ready()) f->dryDelay.prepare (8192);
            rt->tailSeconds = 0.5;
            for (auto& f : rt->fx) rt->tailSeconds = std::max (rt->tailSeconds, f->tail);
            auto& mt = proj.mixer[t];
            if (newFrozen[t]) { rt->frozen = std::move (newFrozen[t]); rt->frozenFile = mt.frozenFile; }
            if (! mt.frozen) rt->frozen.reset();
            tracks.push_back (std::move (rt));
        }
        for (auto& o : trackRt) if (o) discardT.push_back (std::move (o));
        trackRt = std::move (tracks);

        // clips
        std::map<int, std::unique_ptr<ClipRuntime>> mergedClips;
        for (auto& c : proj.clips)
        {
            if (! c.isAudio()) continue;
            if (auto n = newClips.find (c.uid); n != newClips.end()) mergedClips[c.uid] = std::move (n->second);
            else if (auto o = clipRt.find (c.uid); o != clipRt.end() && o->second) mergedClips[c.uid] = std::move (o->second);
        }
        for (auto& [id, rt] : clipRt) if (rt) discardClips.push_back (std::move (rt));
        clipRt = std::move (mergedClips);

        // routing
        levels = proj.processingLevels();
        sources.assign (proj.mixer.size(), {});
        for (int t = 1; t < (int) proj.mixer.size(); ++t)
        {
            auto& mt = proj.mixer[(size_t) t];
            for (auto& s : mt.sends) if (s.target >= 0 && s.target < (int) proj.mixer.size()) sources[(size_t) s.target].push_back ({ t, s.level });
            if (mt.routesToMaster) sources[0].push_back ({ t, 1.0f });
        }
        for (size_t t = 0; t < trackRt.size() && t < sources.size(); ++t)
        {
            auto& rt = *trackRt[t];
            if (! rt.inDelay.ready()) rt.inDelay.prepare (kPdcSize);
            if (rt.edge.size() != sources[t].size()) { rt.edge.resize (sources[t].size()); for (auto& d : rt.edge) if (! d.ready()) d.prepare (kPdcSize); }
        }
        updateSolo();
    }
    // close editors before their plugin instance is deleted (the discard vectors die at the end of this scope)
    if (onPluginDiscard)
    {
        for (auto& c : discardC) if (c && c->plugin) onPluginDiscard (c->plugin.get());
        for (auto& f : discardFx) if (f && f->plugin) onPluginDiscard (f->plugin.get());
        for (auto& t : discardT) if (t) for (auto& f : t->fx) if (f && f->plugin) onPluginDiscard (f->plugin.get());
    }
    sendChangeMessage();
}

void Engine::updateSolo()
{
    const size_t n = proj.mixer.size();
    soloMuted.assign (n, 0);
    bool any = false;
    for (size_t t = 1; t < n; ++t) any |= proj.mixer[t].solo;
    if (! any) return;
    // a track stays audible if it is soloed, feeds a soloed track, or is fed by one
    std::vector<char> keep (n, 0);
    std::function<void (int)> down = [&] (int t) { if (keep[(size_t) t] & 1) return; keep[(size_t) t] |= 1; for (int o : proj.outputsOf (t)) down (o); };
    std::function<void (int)> up = [&] (int t) { if (keep[(size_t) t] & 2) return; keep[(size_t) t] |= 2; for (auto& s : sources[(size_t) t]) up (s.first); };
    for (size_t t = 1; t < n; ++t) if (proj.mixer[t].solo) { down ((int) t); keep[t] &= ~1; down ((int) t); up ((int) t); }
    for (size_t t = 1; t < n; ++t) soloMuted[t] = keep[t] ? 0 : 1;
}

void Engine::prepare (double sr, int block)
{
    const juce::ScopedLock sl (lock);
    sampleRate = sr; maxBlock = std::max (block, 64);
    scratch.setSize (2, maxBlock); inputScratch.setSize (8, maxBlock);
    prepareRuntimes();
}

void Engine::prepareRuntimes()
{
    for (auto& [id, rt] : channelRt)
    {
        rt->buf.setSize (rt->buf.getNumChannels(), maxBlock);
        if (rt->inst) rt->inst->prepare (sampleRate, maxBlock);
        if (rt->plugin)
        {
            rt->plugin->releaseResources(); rt->plugin->setRateAndBufferSizeDetails (sampleRate, maxBlock); rt->plugin->prepareToPlay (sampleRate, maxBlock);
            rt->buf.setSize (pluginChannels (*rt->plugin), maxBlock);
        }
    }
    for (auto& t : trackRt)
    {
        t->in.setSize (2, maxBlock); t->out.setSize (2, maxBlock); t->dry.setSize (2, maxBlock);
        for (auto& f : t->fx)
        {
            if (f->builtin) f->builtin->prepare (sampleRate, maxBlock);
            if (f->plugin)
            {
                f->plugin->releaseResources(); f->plugin->setRateAndBufferSizeDetails (sampleRate, maxBlock); f->plugin->prepareToPlay (sampleRate, maxBlock);
                f->buf.setSize (pluginChannels (*f->plugin), maxBlock);
            }
        }
    }
    for (auto& [id, c] : clipRt) c->tmp.setSize (2, maxBlock * 4 + 8);
    // sample-based instruments must be rebuilt at the new rate
    for (auto& [id, rt] : channelRt) if (rt->inst) rt->signature = {};
}

void Engine::audioDeviceAboutToStart (juce::AudioIODevice* d)
{
    prepare (d->getCurrentSampleRate(), d->getCurrentBufferSizeSamples());
    recordLatency = juce::jmax (0, d->getInputLatencyInSamples() + d->getOutputLatencyInSamples());
    {
        const juce::ScopedLock sl (lock);
        deviceWorkgroup = d->getWorkgroup();
        if (pool) pool->setWorkgroup (deviceWorkgroup);
    }
    juce::MessageManager::callAsync ([this, a = alive] { if (a->load()) sync(); });
}
void Engine::audioDeviceStopped() {}

double Engine::cpuLoad() const { return deviceManager.getCpuUsage(); }

void Engine::setPerformance (int threads, bool multithreaded, bool smartDisable)
{
    const int hw = juce::jmax (1, (int) std::thread::hardware_concurrency());
    const int workers = threads <= 0 ? juce::jlimit (0, 31, hw - 1) : juce::jlimit (0, 63, threads - 1);
    std::unique_ptr<WorkerPool> fresh;
    if (! pool || pool->size() != workers) fresh = std::make_unique<WorkerPool> (workers);
    {
        const juce::ScopedLock sl (lock);
        if (fresh) { fresh->setWorkgroup (deviceWorkgroup); std::swap (pool, fresh); }
        mtOn = multithreaded; smartOn = smartDisable;
    }
}

void Engine::forEachLiveTake (const std::function<void (int, double, const float*, int, double)>& fn)
{
    if (! recording.load()) return;
    struct Info { int lane; double start; const float* pk; int num; };
    std::vector<Info> live;
    {
        const juce::SpinLock::ScopedTryLockType tl (takeLock);   // brief: copy, then draw outside the lock
        if (! tl.isLocked()) return;
        for (auto& tk : takes) if (tk->started) live.push_back ({ tk->lane, tk->startBeat, tk->peaks.data(), tk->numPeaks.load() });
    }
    const double beatsPerPeak = 256.0 / sampleRate * proj.bpm / 60.0;
    for (auto& i : live) fn (i.lane, i.start, i.pk, i.num, beatsPerPeak);
}

float Engine::fxLoad (int track, int uid) const
{
    if (track < 0 || track >= (int) trackRt.size() || ! trackRt[(size_t) track]) return 0.0f;
    for (auto& f : trackRt[(size_t) track]->fx) if (f->uid == uid) return f->load.load();
    return 0.0f;
}

float Engine::trackPeak (int t) const
{
    return t >= 0 && t < (int) trackRt.size() ? trackRt[(size_t) t]->peak.load() : 0.0f;
}

juce::AudioPluginInstance* Engine::channelPlugin (int id)
{
    auto it = channelRt.find (id);
    return it != channelRt.end() ? it->second->plugin.get() : nullptr;
}
juce::AudioPluginInstance* Engine::fxPlugin (int track, int uid)
{
    if (track < 0 || track >= (int) trackRt.size()) return nullptr;
    for (auto& f : trackRt[(size_t) track]->fx) if (f->uid == uid) return f->plugin.get();
    return nullptr;
}
BuiltinEffect* Engine::builtinFx (int track, int uid)
{
    if (track < 0 || track >= (int) trackRt.size()) return nullptr;
    for (auto& f : trackRt[(size_t) track]->fx) if (f->uid == uid) return f->builtin.get();
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
double Engine::loopLength() const
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
        auto it = clipRt.find (clip.uid);
        if (it == clipRt.end() || ! it->second->reader) continue;
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
    const bool rec = recording.load(), punch = rec && proj.songMode && proj.hasPunch();
    const double here = position.load();
    const bool loopSel = ! rec && proj.songMode && proj.hasPunch() && (here < proj.punchIn || here >= proj.punchOut);   // FL: Space plays the selected range
    const double from = punch ? std::max (0.0, proj.punchIn - proj.preRollBars * proj.beatsPerBar) : loopSel ? proj.punchIn : here;
    primeClipReaders (from);              // load earlier takes/beats where playback really starts (incl. punch pre-roll)
    if (rec) startRecordingWriters();     // create take files outside the audio lock, so nothing drops out as recording starts
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
    stopPreview();
    {
        const juce::ScopedLock sl (lock);
        if (! playing && takes.empty()) { position = 0.0; return; }
        playing = false; countingIn = false;
        for (auto* c : channelOrder)
        {
            if (c->inst) c->inst->allNotesOff();
            c->numActive = 0;
            if (c->plugin) { c->midi.clear(); c->midi.addEvent (juce::MidiMessage::allNotesOff (1), 0); }
        }
    }
    finishRecordingWriters();
    if (proj.returnToStart) position = startBeat;
}

void Engine::setPositionBeats (double b)
{
    position = std::max (0.0, b); startBeat = position; editChase = false; chasePending = true;
    if (playing.load()) declickGain = 0.0f;
}

void Engine::setRecording (bool r)
{
    if (r == recording.load()) return;
    recording = r;
    if (! r && playing) finishRecordingWriters();
    else if (r && playing) startRecordingWriters();   // punch in on the fly
}

// =============================================================== recording
void Engine::startRecordingWriters()
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
                                                           + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + "_take", ".wav");
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
            c.fadeIn = c.fadeOut = proj.hasPunch() ? 0.02 : 0.0;
            newClips.push_back (c);
        }
        if (newClips.empty()) continue;
        if (! proj.hasPunch())
        {
            // FL-style: a new take never covers an earlier one - it drops to the next free lane (same vocal insert)
            const double a0 = newClips.front().start, a1 = a0 + newClips.front().length;
            auto overlaps = [&] (int ln) {
                for (auto& c : proj.clips) if (c.lane == ln && c.isAudio() && ! c.hidden && c.start < a1 && c.start + c.length > a0) return true;
                return false; };
            int ln = t->lane;
            while (overlaps (ln)) ++ln;
            if (ln != t->lane)
            {
                while (ln >= (int) proj.lanes.size()) { Lane nl; nl.name = "Track " + juce::String ((int) proj.lanes.size() + 1); nl.mixerTrack = insert; proj.lanes.push_back (nl); }
                for (size_t i = 0; i < newClips.size(); ++i)
                {
                    newClips[i].lane = ln;
                    newClips[i].hidden = i + 1 < newClips.size();
                    proj.clips.push_back (newClips[i]);
                }
                continue;
            }
        }
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
    int s1, n1, s2, n2;
    previewFifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0) previewEvents[(size_t) s1] = { channelId, key, vel, on };
    else if (n2 > 0) previewEvents[(size_t) s2] = { channelId, key, vel, on };
    previewFifo.finishedWrite (n1 + n2);
}

void Engine::previewFile (const juce::File& f)
{
    auto s = loadSampleFile (f, sampleRate);
    const juce::SpinLock::ScopedLockType sl (previewLock);
    previewSample = s; previewPos = s ? 0 : -1;
    previewStarted = juce::Time::getMillisecondCounter();
}

// =============================================================== audio
juce::Optional<juce::AudioPlayHead::PositionInfo> Engine::getPosition() const
{
    PositionInfo info;
    info.setBpm (proj.bpm);
    info.setTimeSignature (juce::AudioPlayHead::TimeSignature { proj.beatsPerBar, 4 });
    info.setPpqPosition (position.load());
    info.setTimeInSeconds (beatsToSeconds (position.load(), proj.bpm));
    info.setIsPlaying (playing.load());
    info.setIsRecording (recording.load() && playing.load());
    info.setIsLooping (true);
    return info;
}

void Engine::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                               const juce::AudioIODeviceCallbackContext&)
{
    int done = 0;
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
    const double spb = sampleRate * 60.0 / proj.bpm;
    const double firstBeat = std::ceil (b0 - 1.0e-9);
    for (double b = firstBeat; b < b1; b += 1.0)
    {
        const int off = (int) ((b - b0) * spb);
        if (off < 0 || off >= n) continue;
        clickLeft = (int) (sampleRate * 0.03);
        clickFreq = ((long) std::llround (b) % proj.beatsPerBar) == 0 ? 1600.0 : 1000.0;
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

void Engine::renderSegment (double b0, double b1, int off, int n)
{
    const double spb = sampleRate * 60.0 / proj.bpm;
    auto toOffset = [&] (double beat) { return juce::jlimit (off, off + n - 1, off + (int) ((beat - b0) * spb)); };
    auto swingBeat = [&] (double s) {
        if (proj.swing <= 0.0) return s;
        const double six = s / 0.25;
        return (std::abs (six - std::round (six)) < 1.0e-6 && ((long) std::llround (six) % 2) == 1) ? s + proj.swing * 0.0833 : s;
    };
    bool anySolo = false;
    for (auto& c : proj.channels) anySolo |= c.solo;

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
        auto& c = proj.channels[ci];
        const bool shot = c.type == ChannelType::Sampler && c.oneShot;   // one-shots play the whole sample, whatever the note length
        const double end = std::min (absStart + note.length, limit);
        if (shot ? (quietChase || b0 - absStart > 64.0) : end <= b0 + 1.0 / 32.0) return;   // after an edit a one-shot is still ringing: never re-hit it
        auto* rt = channelOrder[ci];
        for (int k = 0; k < rt->numActive; ++k) if (rt->active[(size_t) k].key == note.key) return;
        Note tail = note; tail.length = std::max (end - b0, 1.0 / 32.0); tail.slide = false;
        emit (ci, tail, b0, (b0 - absStart) * 60.0 / proj.bpm);   // samplers start from the matching point in the sample
        if (! quietChase) declickGain = 0.0f;   // starting mid-note: fade in instead of a hard, boxy cut-in (edits: the voice fades itself, the mix keeps going)
    };

    for (size_t ci = 0; ci < proj.channels.size() && ci < channelOrder.size(); ++ci)
    {
        auto& ch = proj.channels[ci];
        if (ch.muted || (anySolo && ! ch.solo)) continue;
        const double shiftBeats = (double) ch.shift * 0.001 * proj.bpm / 60.0;   // channel Shift (ms) in beats
        if (! proj.songMode)
        {
            if (proj.currentPattern < 0 || proj.currentPattern >= (int) proj.patterns.size()) continue;
            auto& pat = proj.patterns[(size_t) proj.currentPattern];
            auto it = pat.notes.find (ch.id);
            if (it == pat.notes.end()) continue;
            const double plen = pat.lengthBeats();
            for (auto& note : it->second)
            {
                if (note.muted) continue;
                double s = swingBeat (note.start) + shiftBeats;
                if (s < 0.0) s += plen; else if (s >= plen) s -= plen;   // shifted past the loop edge: wraps like FL
                if (s >= b0 && s < b1) emit (ci, note, s);
                else if (chase) chaseNote (ci, note, s, pat.lengthBeats());
            }
        }
        else if (ch.type == ChannelType::Drummer)
        {
            for (auto& clip : proj.clips)
            {
                if (clip.drummer != ch.id || clip.muted || clip.hidden) continue;
                if (clip.lane >= 0 && clip.lane < (int) proj.lanes.size() && proj.lanes[(size_t) clip.lane].muted) continue;
                if (clip.start >= b1 || clip.start + clip.length <= b0) continue;
                auto it = drumHits.find (clip.uid);
                if (it == drumHits.end() || ! it->second) continue;
                for (auto& h : *it->second)
                {
                    if (h.beat >= clip.length) break;
                    const double s = clip.start + h.beat + shiftBeats;
                    if (s < b0 || s >= b1) continue;
                    Note nt; nt.key = h.lane + 32 * (h.variant & 3); nt.start = h.beat; nt.length = 0.125; nt.velocity = h.vel;
                    emit (ci, nt, s);
                }
            }
        }
        else
        {
            for (auto& clip : proj.clips)
            {
                if (clip.isAudio() || clip.isDrummer() || clip.patternIndex < 0 || clip.muted || clip.hidden) continue;
                if (clip.lane >= 0 && clip.lane < (int) proj.lanes.size() && proj.lanes[(size_t) clip.lane].muted) continue;
                if (clip.start >= b1 || clip.start + clip.length <= b0) continue;
                if (clip.patternIndex >= (int) proj.patterns.size()) continue;
                auto& pat = proj.patterns[(size_t) clip.patternIndex];
                auto it = pat.notes.find (ch.id);
                if (it == pat.notes.end()) continue;
                const double plen = pat.lengthBeats();
                const int r0 = std::max (0, (int) std::floor ((b0 - clip.start) / plen) - 1);
                const int r1 = (int) std::floor ((b1 - clip.start) / plen) + 1;
                for (int rep = r0; rep <= r1; ++rep)
                    for (auto& note : it->second)
                    {
                        if (note.muted) continue;
                        const double s = clip.start + rep * plen + swingBeat (note.start) + shiftBeats;
                        if (s >= b0 && s < b1 && s < clip.start + clip.length + juce::jmax (0.0, shiftBeats) && s >= clip.start + juce::jmin (0.0, shiftBeats)) emit (ci, note, s);
                        else if (chase && s < b0 && s >= clip.start) chaseNote (ci, note, s, clip.start + clip.length);
                    }
            }
        }
    }

    // audio clips -> their insert (FL-style: routing follows the recording)
    if (proj.songMode)
    {
        for (auto& clip : proj.clips)
        {
            if (! clip.isAudio() || clip.muted || clip.hidden) continue;
            if (clip.lane >= 0 && clip.lane < (int) proj.lanes.size() && proj.lanes[(size_t) clip.lane].muted) continue;
            const double shB = (clip.lane >= 0 && clip.lane < (int) proj.lanes.size() ? proj.lanes[(size_t) clip.lane].shiftMs : 0.0f) * 0.001 * proj.bpm / 60.0;   // Soft
            const double c0 = std::max (b0, clip.start + shB), c1 = std::min (b1, clip.start + clip.length + shB);
            if (c1 <= c0) continue;
            auto it = clipRt.find (clip.uid);
            if (it == clipRt.end() || ! it->second->reader) continue;
            auto& cr = *it->second;
            const int insert = proj.clipInsert (clip);
            if (insert < 0 || insert >= (int) trackRt.size()) continue;
            auto& dest = trackRt[(size_t) insert]->in;
            const int o0 = off + (int) std::llround ((c0 - b0) * spb);
            const int o1 = std::min (off + n, off + (int) std::llround ((c1 - b0) * spb));
            const int len = o1 - o0;
            if (len <= 0) continue;
            const double ratio = cr.fileRate / sampleRate;
            const double srcSec = clip.sourceOffset + beatsToSeconds (c0 - clip.start - shB, proj.bpm);
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
                float g = clip.gain;
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
            const juce::int64 src = (juce::int64) (beatsToSeconds (b0, proj.bpm) * tr.frozen->sampleRate);
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
    static void channelJob (Engine* e, int i)
    {
        auto* rt = e->channelOrder[(size_t) i];
        const int n = e->currentBlock;
        rt->buf.clear (0, n);
        if (rt->plugin)
        {
            rt->midi.clear();
            for (int k = 0; k < rt->numEvents; ++k)
            {
                auto& ev = rt->events[(size_t) k];
                if (ev.key < 0) continue;
                rt->midi.addEvent (ev.on ? juce::MidiMessage::noteOn (1, ev.key, juce::jlimit (0.0f, 1.0f, ev.vel))
                                         : juce::MidiMessage::noteOff (1, ev.key), ev.offset);
            }
            if (rt->numEvents == 0 && rt->numActive == 0 && ++rt->silentBlocks > e->idleThresholdBlocks * 4 && e->smartOn.load()) { rt->numEvents = 0; return; }
            if (rt->numEvents > 0) rt->silentBlocks = 0;
            auto& pl = *rt->plugin;
            const juce::ScopedTryLock cl (pl.getCallbackLock());
            if (! cl.isLocked() || pl.isSuspended() || pluginChannels (pl) > rt->buf.getNumChannels() || n > rt->buf.getNumSamples())
            { rt->numEvents = 0; return; }
            juce::AudioBuffer<float> view (rt->buf.getArrayOfWritePointers(), rt->buf.getNumChannels(), n);
            pl.processBlock (view, rt->midi);
            if (pl.getTotalNumOutputChannels() == 1) view.copyFrom (1, 0, view, 0, 0, n);
            if (view.getMagnitude (0, n) > 1.0e-5f) rt->silentBlocks = 0;
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

void Engine::processTrack (int t)
{
    const int n = currentBlock;
    auto& tr = *trackRt[(size_t) t];
    auto& mt = proj.mixer[(size_t) t];
    tr.out.makeCopyOf (tr.in, true);
    // delay compensation: everything arriving here is lined up with the latest (most delayed) input
    if (tr.inputAlign > 0 && tr.inDelay.ready()) tr.inDelay.delayInPlace (tr.out, n, tr.inputAlign);
    if (! tr.frozen)
    {
        auto& S = sources[(size_t) t];
        for (size_t k = 0; k < S.size(); ++k)
        {
            auto& srt = *trackRt[(size_t) S[k].first];
            const int dl = tr.inputAlign - srt.arrival;
            if (dl > 0 && k < tr.edge.size() && tr.edge[k].ready()) tr.edge[k].addDelayed (srt.out, tr.out, n, dl, S[k].second);
            else for (int ch = 0; ch < 2; ++ch) tr.out.addFrom (ch, 0, srt.out, ch, 0, n, S[k].second);
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
                for (int c = 0; c < nch; ++c)
                    if (c < 2) f.buf.copyFrom (c, 0, view, c, 0, n); else f.buf.clear (c, 0, n);
                if (pl.getMainBusNumInputChannels() == 1)   // mono plugin on a stereo bus: feed it the mono sum, not just the left side
                { f.buf.addFrom (0, 0, view, 1, 0, n); f.buf.applyGain (0, 0, n, 0.5f); }
                juce::AudioBuffer<float> pv (f.buf.getArrayOfWritePointers(), nch, n);
                f.midi.clear();
                pl.processBlock (pv, f.midi);
                view.copyFrom (0, 0, pv, 0, 0, n);
                view.copyFrom (1, 0, pv, outs > 1 ? 1 : 0, 0, n);
            }
        }
    }
    else if (idle && ! tr.frozen) tr.out.clear (0, n);

    tr.level.store (std::max (tr.out.getMagnitude (0, 0, n), tr.out.getMagnitude (1, 0, n)));
    // one-knob sidechain duck: this insert dips under its SC source (default: the armed vocal track)
    if (t > 0 && mt.duck > 0.0f)
    {
        int src = mt.duckSource;
        if (src < 0) for (auto& ln : proj.lanes) if (ln.armed && ln.mixerTrack > 0) { src = ln.mixerTrack; break; }
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
    const float gl = vol * panGain (mt.pan, false), gr = vol * panGain (mt.pan, true);
    tr.out.applyGain (0, 0, n, gl);
    tr.out.applyGain (1, 0, n, gr);
    const float pk = std::max (tr.out.getMagnitude (0, 0, n), tr.out.getMagnitude (1, 0, n));
    tr.peak.store (std::max (pk, tr.peak.load() * 0.85f));
    if (auto* sc = trackScope (t)) { sc->rate = sampleRate; sc->push (tr.out.getReadPointer (0), tr.out.getReadPointer (1), n); }
}

void Engine::processBlock (const float* const* in, int numIn, float* const* out, int numOut, int n)
{
    juce::ScopedNoDenormals noDenormals;
    // an edit holds the lock for a moment: wait briefly for it instead of dropping a silent block (that was the click / pause)
    bool locked = lock.tryEnter();
    if (! locked && ! offline.load())
    {
        const double until = juce::Time::getMillisecondCounterHiRes() + 0.4 * 1000.0 * n / sampleRate;
        while (! locked && juce::Time::getMillisecondCounterHiRes() < until) { std::this_thread::yield(); locked = lock.tryEnter(); }
    }
    struct Unlock { const juce::CriticalSection* c; ~Unlock() { if (c != nullptr) c->exit(); } } unlock { locked ? &lock : nullptr };
    if (! locked || trackRt.empty())
    {
        for (int c = 0; c < numOut; ++c) if (out[c]) juce::FloatVectorOperations::clear (out[c], n);
        return;
    }
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

    ctx.bpm = proj.bpm; ctx.keyRoot = proj.keyRoot; ctx.scaleIndex = proj.scaleIndex;
    ctx.playing = playing.load() && ! countingIn.load();
    ctx.beatPos = position.load();
    const double spb = sampleRate * 60.0 / proj.bpm;
    const double blockBeats = n / spb;
    double clickB0 = 0, clickB1 = 0; bool doClick = false;
    juce::int64 recordFrom = -1; // offset in this block where recording audio starts

    if (playing.load())
    {
        if (countingIn.load())
        {
            const double pre = proj.beatsPerBar - countInRemaining;
            clickB0 = pre; clickB1 = pre + blockBeats; doClick = true;
            countInRemaining -= blockBeats;
            if (countInRemaining <= 0) countingIn = false;
        }
        else
        {
            double pos = position.load();
            const bool recordSong = recording.load() && proj.songMode && ! (proj.hasPunch() && proj.loopRecord);
            double loopStart = 0.0, loopEnd = loopLength();
            if (proj.songMode && proj.hasPunch() && recording.load() && proj.loopRecord)
            { loopStart = std::max (0.0, proj.punchIn - proj.preRollBars * proj.beatsPerBar); loopEnd = proj.punchOut; }
            else if (proj.songMode && proj.hasPunch() && ! recording.load() && ! offline.load()) { loopStart = proj.punchIn; loopEnd = proj.punchOut; }
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

    renderChannels (n);

    // channels -> inserts
    bool anySolo = false;
    for (auto& c : proj.channels) anySolo |= c.solo;
    for (size_t ci = 0; ci < proj.channels.size() && ci < channelOrder.size(); ++ci)
    {
        auto& ch = proj.channels[ci];
        auto* rt = channelOrder[ci];
        const int t = juce::jlimit (0, (int) trackRt.size() - 1, ch.mixerTrack);
        const float v = (ch.volume / 0.8f) * (ch.volume / 0.8f), gl = v * panGain (ch.pan, false), gr = v * panGain (ch.pan, true);
        auto& dest = trackRt[(size_t) t]->in;
        dest.addFrom (0, 0, rt->buf, 0, 0, n, gl);
        dest.addFrom (1, 0, rt->buf, rt->buf.getNumChannels() > 1 ? 1 : 0, 0, n, gr);
    }

    // live input: armed lanes monitor through their insert and record
    const bool rolling = playing.load() && ! countingIn.load();
    {
        const juce::SpinLock::ScopedTryLockType tl (takeLock);
        for (size_t l = 0; l < proj.lanes.size(); ++l)
        {
            auto& lane = proj.lanes[l];
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

    masterPeakL = std::max (juce::FloatVectorOperations::findMaximum (l, n), -juce::FloatVectorOperations::findMinimum (l, n));
    masterPeakR = std::max (juce::FloatVectorOperations::findMaximum (r, n), -juce::FloatVectorOperations::findMinimum (r, n));
    masterScope().rate = sampleRate;
    masterScope().push (l, r, n);
    for (int c = 0; c < numOut; ++c)
    {
        if (! out[c]) continue;
        const float* src = c == 0 ? l : (c == 1 ? r : nullptr);
        if (src) { juce::FloatVectorOperations::copy (out[c], src, n); juce::FloatVectorOperations::clip (out[c], out[c], -4.0f, 4.0f, n); }
        else juce::FloatVectorOperations::clear (out[c], n);
    }
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

void Engine::mixTracks (const float* const*, int, int)
{
    // plugin delay compensation (like FL): each insert's latency = its active FX; inputs wait for the slowest path
    for (auto& L : levels)
        for (int t : L)
        {
            if (t < 0 || t >= (int) trackRt.size() || t >= (int) proj.mixer.size() || t >= (int) sources.size()) continue;
            auto& tr = *trackRt[(size_t) t];
            auto& mt = proj.mixer[(size_t) t];
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
    const double sr = sampleRate; const int block = 512;
    const bool oldSong = proj.songMode;
    proj.songMode = song;
    const double lenBeats = song ? std::max (proj.songLengthBeats(), 1.0) : loopLength();
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

    {
        const juce::ScopedLock sl (lock);
        for (auto& t : trackRt) { t->silentSamples = 0; }
        position = 0.0; playing = true; countingIn = false;
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
        for (auto* c : channelOrder) { if (c->inst) c->inst->allNotesOff(); c->numActive = 0; }
    }
    deviceManager.addAudioCallback (this);
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
