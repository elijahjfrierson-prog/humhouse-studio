#include "PluginSandbox.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <thread>

#if ! JUCE_IOS
namespace hhs
{
namespace
{
constexpr const char* kHostId = "hhs-plugin-host";
constexpr int kMaxBlock = 4096, kMaxMidi = 512, kMaxParams = 256;

// audio + MIDI + transport for one block, in a memory-mapped file both processes see
struct Shared
{
    std::atomic<int> seq, done, n, numMidi, pqWrite, pqRead;
    double sampleRate, bpm, ppq, timeSec;
    int playing, pad;
    struct Midi { int offset, size; juce::uint8 bytes[4]; } midi[kMaxMidi];
    struct Param { int index; float value; } pq[kMaxParams];
    float in[2][kMaxBlock], out[2][kMaxBlock];
};

juce::MemoryBlock pack (const juce::ValueTree& v) { juce::MemoryOutputStream os; v.writeToStream (os); return os.getMemoryBlock(); }
juce::ValueTree unpack (const juce::MemoryBlock& m) { return juce::ValueTree::readFromData (m.getData(), m.getSize()); }

// ------------------------------------------------------------------ host side
class SandboxedPlugin;

struct ProxyParam : juce::AudioPluginInstance::HostedParameter
{
    ProxyParam (SandboxedPlugin& o, int i, juce::String nm, float v) : owner (o), index (i), name (std::move (nm)), value (v) {}
    float getValue() const override { return value.load(); }
    void setValue (float v) override;
    float getDefaultValue() const override { return 0.0f; }
    juce::String getName (int maxLen) const override { return name.substring (0, maxLen); }
    juce::String getLabel() const override { return {}; }
    float getValueForText (const juce::String& t) const override { return t.getFloatValue(); }
    juce::String getParameterID() const override { return juce::String (index); }
    SandboxedPlugin& owner; int index; juce::String name; std::atomic<float> value;
};

class SandboxedPlugin : public juce::AudioPluginInstance
{
public:
    struct Coordinator : juce::ChildProcessCoordinator
    {
        SandboxedPlugin& o;
        explicit Coordinator (SandboxedPlugin& p) : o (p) {}
        void handleMessageFromWorker (const juce::MemoryBlock& m) override { { const juce::ScopedLock sl (o.replyLock); o.reply = unpack (m); } o.replied.signal(); }
        void handleConnectionLost() override { o.crashed = true; o.replied.signal(); }
    };

    SandboxedPlugin (const juce::PluginDescription& d)
        : juce::AudioPluginInstance (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), ! d.isInstrument)
                                                      .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
          desc (d), coord (*this) {}

    ~SandboxedPlugin() override
    {
        if (! crashed) coord.sendMessageToWorker (pack (juce::ValueTree ("close")));
        coord.killWorkerProcess();
        map.reset();
        shmFile.deleteFile();
    }

    bool start (double sr, int block, juce::String& error)
    {
        shmFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("hhs-plug", ".shm");
        {
            juce::FileOutputStream os (shmFile);
            if (! os.openedOk()) { error = "Can't create the plug-in sandbox"; return false; }
            juce::MemoryBlock zeros (sizeof (Shared), true);
            os.write (zeros.getData(), zeros.getSize());
        }
        map = std::make_unique<juce::MemoryMappedFile> (shmFile, juce::MemoryMappedFile::readWrite, false);
        if (map->getData() == nullptr || map->getSize() < sizeof (Shared)) { error = "Can't map the plug-in sandbox"; return false; }
        sh = static_cast<Shared*> (map->getData());
        if (! coord.launchWorkerProcess (juce::File::getSpecialLocation (juce::File::currentExecutableFile), kHostId, 0))
        { error = "Can't start the plug-in sandbox"; return false; }
        juce::ValueTree msg ("create");
        msg.setProperty ("desc", desc.createXml()->toString(), nullptr);
        msg.setProperty ("shm", shmFile.getFullPathName(), nullptr);
        msg.setProperty ("sr", sr, nullptr); msg.setProperty ("block", block, nullptr);
        auto r = request (msg, 30000);
        if (! r.isValid() || ! (bool) r["ok"]) { error = r.isValid() ? r["error"].toString() : juce::String ("Plug-in sandbox didn't answer"); return false; }
        setLatencySamples ((int) r["latency"]);
        tail = (double) r["tail"];
        hasEd = (bool) r["editor"];
        for (auto p : r.getChildWithName ("params"))
            addHostedParameter (std::make_unique<ProxyParam> (*this, (int) p["i"], p["name"].toString(), (float) p["v"]));
        setRateAndBufferSizeDetails (sr, block);
        return true;
    }

    juce::ValueTree request (const juce::ValueTree& msg, int timeoutMs)
    {
        if (crashed) return {};
        replied.reset();
        { const juce::ScopedLock sl (replyLock); reply = {}; }
        if (! coord.sendMessageToWorker (pack (msg))) return {};
        replied.wait (timeoutMs);
        const juce::ScopedLock sl (replyLock);
        return reply;
    }

    void pushParam (int index, float v)
    {
        if (sh == nullptr) return;
        const int w = sh->pqWrite.load();
        if (w - sh->pqRead.load() >= kMaxParams) return;
        sh->pq[w % kMaxParams] = { index, v };
        sh->pqWrite.store (w + 1);
    }

    // AudioProcessor
    const juce::String getName() const override { return desc.name; }
    void fillInPluginDescription (juce::PluginDescription& d) const override { d = desc; }
    void prepareToPlay (double sr, int block) override
    {
        juce::ValueTree m ("prepare"); m.setProperty ("sr", sr, nullptr); m.setProperty ("block", block, nullptr);
        request (m, 5000);
    }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& midi) override
    {
        const int n = juce::jmin (b.getNumSamples(), kMaxBlock);
        if (crashed || sh == nullptr) { if (desc.isInstrument) b.clear(); return; }   // crashed: FX pass the sound through, instruments go quiet
        const int nc = juce::jmin (2, b.getNumChannels());
        for (int c = 0; c < 2; ++c) std::memcpy (sh->in[c], b.getReadPointer (juce::jmin (c, nc - 1)), sizeof (float) * (size_t) n);
        int k = 0;
        for (const auto meta : midi)
        {
            if (k >= kMaxMidi || meta.numBytes > 4) continue;
            auto& e = sh->midi[k++]; e.offset = meta.samplePosition; e.size = meta.numBytes;
            std::memcpy (e.bytes, meta.data, (size_t) meta.numBytes);
        }
        sh->numMidi.store (k);
        sh->n.store (n);
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
            {
                sh->bpm = pos->getBpm().orFallback (120.0); sh->ppq = pos->getPpqPosition().orFallback (0.0);
                sh->timeSec = pos->getTimeInSeconds().orFallback (0.0); sh->playing = pos->getIsPlaying() ? 1 : 0;
            }
        sh->sampleRate = getSampleRate();
        const int want = sh->seq.load() + 1;
        sh->seq.store (want);
        // wait for the helper: at most about one block, so a hung plug-in can't stall the whole mix
        const double until = juce::Time::getMillisecondCounterHiRes() + 2.0 + 1500.0 * n / juce::jmax (1.0, getSampleRate());
        while (sh->done.load() != want)
        {
            if (juce::Time::getMillisecondCounterHiRes() > until)
            {
                if (++misses > 200) crashed = true;   // ~a second of silence from it: treat as crashed
                if (desc.isInstrument) b.clear();
                return;
            }
            std::this_thread::yield();
        }
        misses = 0;
        for (int c = 0; c < b.getNumChannels(); ++c) std::memcpy (b.getWritePointer (c), sh->out[juce::jmin (c, 1)], sizeof (float) * (size_t) n);
    }
    double getTailLengthSeconds() const override { return tail; }
    bool acceptsMidi() const override { return desc.isInstrument; }
    bool producesMidi() const override { return false; }
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& dest) override
    {
        auto r = request (juce::ValueTree ("getstate"), 10000);
        if (r.isValid()) if (auto* mb = r["state"].getBinaryData()) dest = *mb;
    }
    void setStateInformation (const void* data, int size) override
    {
        juce::ValueTree m ("setstate"); m.setProperty ("state", juce::var (juce::MemoryBlock (data, (size_t) size)), nullptr);
        request (m, 10000);
    }
    void showWindow() { coord.sendMessageToWorker (pack (juce::ValueTree ("showeditor"))); }

    juce::PluginDescription desc;
    Coordinator coord;
    juce::CriticalSection replyLock;
    juce::ValueTree reply;
    juce::WaitableEvent replied;
    std::atomic<bool> crashed { false };
    juce::File shmFile;
    std::unique_ptr<juce::MemoryMappedFile> map;
    Shared* sh = nullptr;
    double tail = 0.0;
    bool hasEd = true;
    int misses = 0;
};

void ProxyParam::setValue (float v) { value = v; owner.pushParam (index, v); }

struct SandboxEditor : juce::AudioProcessorEditor
{
    explicit SandboxEditor (SandboxedPlugin& p) : juce::AudioProcessorEditor (p), plug (p)
    {
        addAndMakeVisible (open);
        addAndMakeVisible (note);
        open.setButtonText ("Open " + p.desc.name + " window");
        open.onClick = [this] { plug.showWindow(); };
        note.setText (p.crashed ? p.desc.name + " crashed. Your song is safe: re-add it to reload."
                                : "Crash-protected: runs in its own process.", juce::dontSendNotification);
        note.setJustificationType (juce::Justification::centred);
        setSize (360, 110);
        if (! p.crashed) plug.showWindow();
    }
    void resized() override { auto r = getLocalBounds().reduced (10); open.setBounds (r.removeFromTop (40)); note.setBounds (r); }
    SandboxedPlugin& plug;
    juce::TextButton open;
    juce::Label note;
};

juce::AudioProcessorEditor* SandboxedPlugin::createEditor() { return new SandboxEditor (*this); }

// ------------------------------------------------------------------ helper (child) side
struct ChildPlayHead : juce::AudioPlayHead
{
    Shared* sh = nullptr;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo i;
        if (sh == nullptr) return i;
        i.setBpm (sh->bpm); i.setPpqPosition (sh->ppq); i.setTimeInSeconds (sh->timeSec); i.setIsPlaying (sh->playing != 0);
        return i;
    }
};

struct HostWorker : juce::ChildProcessWorker, juce::Thread
{
    HostWorker() : juce::Thread ("HHS plug-in audio") {}
    ~HostWorker() override { stopThread (2000); window.reset(); plugin.reset(); }

    void handleMessageFromCoordinator (const juce::MemoryBlock& m) override
    {
        auto v = unpack (m);
        juce::MessageManager::callAsync ([this, v] { handle (v); });
    }
    void handleConnectionLost() override { juce::MessageManager::callAsync ([] { juce::JUCEApplicationBase::quit(); }); }

    void answer (const juce::ValueTree& v) { sendMessageToCoordinator (pack (v)); }

    void handle (const juce::ValueTree& v)
    {
        juce::ValueTree r ("reply");
        if (v.hasType ("create"))
        {
            juce::PluginDescription d;
            if (auto xml = juce::parseXML (v["desc"].toString())) d.loadFromXml (*xml);
            map = std::make_unique<juce::MemoryMappedFile> (juce::File (v["shm"].toString()), juce::MemoryMappedFile::readWrite, false);
            sh = map->getData() != nullptr && map->getSize() >= sizeof (Shared) ? static_cast<Shared*> (map->getData()) : nullptr;
            fm.addDefaultFormats();
            juce::String err;
            const double sr = v["sr"]; block = juce::jmin ((int) v["block"], kMaxBlock);
            plugin = sh != nullptr ? fm.createPluginInstance (d, sr, block, err) : nullptr;
            if (plugin)
            {
                plugin->enableAllBuses();
                ph.sh = sh; plugin->setPlayHead (&ph);
                plugin->setRateAndBufferSizeDetails (sr, block);
                plugin->prepareToPlay (sr, block);
                buf.setSize (juce::jmax (2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()), kMaxBlock);
                r.setProperty ("ok", true, nullptr);
                r.setProperty ("latency", plugin->getLatencySamples(), nullptr);
                r.setProperty ("tail", plugin->getTailLengthSeconds(), nullptr);
                r.setProperty ("editor", plugin->hasEditor(), nullptr);
                juce::ValueTree ps ("params");
                const auto& params = plugin->getParameters();
                for (int i = 0; i < params.size(); ++i)
                {
                    juce::ValueTree p ("p");
                    p.setProperty ("i", i, nullptr); p.setProperty ("name", params[i]->getName (64), nullptr); p.setProperty ("v", params[i]->getValue(), nullptr);
                    ps.appendChild (p, nullptr);
                }
                r.appendChild (ps, nullptr);
                startRealtimeThread (juce::Thread::RealtimeOptions{}.withPriority (9)) || startThread (juce::Thread::Priority::highest);
            }
            else { r.setProperty ("ok", false, nullptr); r.setProperty ("error", err.isEmpty() ? juce::String ("Plug-in failed to load") : err, nullptr); }
        }
        else if (plugin == nullptr) {}
        else if (v.hasType ("getstate")) { juce::MemoryBlock mb; plugin->getStateInformation (mb); r.setProperty ("state", juce::var (mb), nullptr); }
        else if (v.hasType ("setstate")) { if (auto* mb = v["state"].getBinaryData()) plugin->setStateInformation (mb->getData(), (int) mb->getSize()); }
        else if (v.hasType ("prepare"))
        {
            const juce::ScopedLock sl (plugin->getCallbackLock());
            block = juce::jmin ((int) v["block"], kMaxBlock);
            plugin->releaseResources(); plugin->setRateAndBufferSizeDetails ((double) v["sr"], block); plugin->prepareToPlay ((double) v["sr"], block);
        }
        else if (v.hasType ("showeditor"))
        {
            if (window == nullptr)
            {
                window = std::make_unique<juce::DocumentWindow> (plugin->getName(), juce::Colours::black, juce::DocumentWindow::closeButton);
                window->setUsingNativeTitleBar (true);
                window->setContentOwned (plugin->hasEditor() ? plugin->createEditorIfNeeded() : new juce::GenericAudioProcessorEditor (*plugin), true);
                window->setAlwaysOnTop (true);
                window->centreWithSize (window->getWidth(), window->getHeight());
            }
            window->setVisible (true); window->toFront (true);
        }
        else if (v.hasType ("close")) { juce::JUCEApplicationBase::quit(); return; }
        answer (r);
    }

    void run() override
    {
        int last = sh->seq.load();
        auto lastWork = juce::Time::getMillisecondCounterHiRes();
        juce::MidiBuffer midi;
        midi.ensureSize (4096);
        while (! threadShouldExit())
        {
            const int s = sh->seq.load();
            if (s == last)
            {
                // blocks arrive every few ms while playing: stay awake then, nap when the host is idle
                if (juce::Time::getMillisecondCounterHiRes() - lastWork < 50.0) std::this_thread::yield();
                else juce::Thread::sleep (1);
                continue;
            }
            last = s;
            const int n = juce::jlimit (0, kMaxBlock, sh->n.load());
            for (int w = sh->pqWrite.load(), r = sh->pqRead.load(); r < w; ++r)
            {
                const auto& p = sh->pq[r % kMaxParams];
                const auto& params = plugin->getParameters();
                if (p.index >= 0 && p.index < params.size()) params[p.index]->setValue (p.value);
                sh->pqRead.store (r + 1);
            }
            midi.clear();
            for (int k = 0, nm = juce::jmin (kMaxMidi, sh->numMidi.load()); k < nm; ++k)
                midi.addEvent (sh->midi[k].bytes, sh->midi[k].size, juce::jlimit (0, juce::jmax (0, n - 1), sh->midi[k].offset));
            juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), buf.getNumChannels(), n);
            view.clear();
            for (int c = 0; c < 2 && c < view.getNumChannels(); ++c) std::memcpy (view.getWritePointer (c), sh->in[c], sizeof (float) * (size_t) n);
            {
                const juce::ScopedLock sl (plugin->getCallbackLock());
                plugin->processBlock (view, midi);
            }
            const int outs = juce::jmax (1, plugin->getTotalNumOutputChannels());
            for (int c = 0; c < 2; ++c) std::memcpy (sh->out[c], view.getReadPointer (juce::jmin (c, outs - 1)), sizeof (float) * (size_t) n);
            sh->done.store (s);
            lastWork = juce::Time::getMillisecondCounterHiRes();
        }
    }

    juce::AudioPluginFormatManager fm;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    std::unique_ptr<juce::MemoryMappedFile> map;
    std::unique_ptr<juce::DocumentWindow> window;
    juce::AudioBuffer<float> buf;
    ChildPlayHead ph;
    Shared* sh = nullptr;
    int block = 512;
};

std::unique_ptr<HostWorker>& hostWorker() { static std::unique_ptr<HostWorker> w; return w; }
}

std::unique_ptr<juce::AudioPluginInstance> createSandboxedPlugin (const juce::PluginDescription& d, double sr, int block, juce::String& error)
{
    auto p = std::make_unique<SandboxedPlugin> (d);
    if (! p->start (sr, block, error)) return {};
    return p;
}

bool isSandboxed (const juce::AudioPluginInstance& p) { return dynamic_cast<const SandboxedPlugin*> (&p) != nullptr; }
bool sandboxCrashed (const juce::AudioPluginInstance& p) { auto* s = dynamic_cast<const SandboxedPlugin*> (&p); return s != nullptr && s->crashed.load(); }

bool isPluginHostChild (const juce::String& cmd) { return cmd.contains (kHostId); }

bool runPluginHostChild (const juce::String& cmd)
{
   #if JUCE_MAC
    juce::Process::setDockIconVisible (false);
   #endif
    auto& w = hostWorker();
    w = std::make_unique<HostWorker>();
    if (w->initialiseFromCommandLine (cmd, kHostId, 0)) return true;
    w.reset();
    return false;
}
}
#endif
