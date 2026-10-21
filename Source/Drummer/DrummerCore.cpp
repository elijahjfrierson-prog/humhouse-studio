#include "DrummerCore.h"
#include "GrooveCorpus.h"
#include "PerformanceEngine.h"
#include "KitEngine.h"
#include <DrummerData.h>
#include <mutex>
#include <map>

namespace hhs
{
juce::String DrummerParams::performanceKey() const
{
    juce::String k;
    for (float v : { complexity, loudness, intensity, fills, swing, humanize, ghost, hatOpen }) k << juce::String (v, 3) << ",";
    return k + juce::String ((int) halfTime) + juce::String ((int) ride) + "," + juce::String (character) + "," + juce::String (seed);
}

const char* drumGroupName (int g)
{
    static const char* n[] { "Kick", "Snare", "Hats", "Toms", "Cymbals" };
    return g >= 0 && g < NumDrumGroups ? n[g] : "";
}

int drumGroupOf (int lane)
{
    if (lane == hhx::LaneKick) return GroupKick;
    if (lane <= hhx::LaneSnareRoll) return GroupSnare;
    if (lane <= hhx::LaneHatBell) return GroupHats;
    if (lane >= hhx::LaneTom1 && lane <= hhx::LaneTom4) return GroupToms;
    return GroupCymbals;   // ride, crashes, china, splash, perc
}

static const hhx::GrooveCorpus& corpus()
{
    static hhx::GrooveCorpus c;
    static const bool loaded = c.loadFromMemory (DrummerData::rock_corpus_hhc, (std::size_t) DrummerData::rock_corpus_hhcSize);
    juce::ignoreUnused (loaded);
    return c;
}

static std::vector<DrumHit> render (const DrummerParams& p, double bpm, int bars)
{
    std::vector<DrumHit> out;
    if (! corpus().isLoaded() || bars <= 0) return out;
    hhx::PerformanceSettings s;
    s.complexity = p.complexity; s.intensity = p.loudness; s.sectionVelocity = p.intensity;
    s.fillAmount = p.fills; s.swing = p.swing; s.humanize = p.humanize; s.ghostAmount = p.ghost; s.hatOpenness = p.hatOpen;
    s.halfTime = p.halfTime; s.rideInsteadOfHat = p.ride; s.character = p.character; s.tempoBpm = (float) bpm;
    s.seed = (std::uint64_t) juce::jmax (1, p.seed);
    s.phraseBars = (bars % 2) != 0 ? 1 : 2;   // a phrase always ends on the region's last bar, so the fill lands there
    hhx::ArrangementSection a;
    a.id = 1; a.numBars = bars; a.section = hhx::SectionVerse;
    a.complexity = p.complexity; a.intensity = p.loudness; a.velocity = p.intensity; a.fillAmount = p.fills;
    a.swing = p.swing; a.halfTime = p.halfTime;
    hhx::ArrangementSection after = a; after.id = 2; after.numBars = 64; after.section = hhx::SectionChorus;   // the region ends into the next section: fill
    s.arrangement = { a, after };
    hhx::PerformanceEngine e; e.setCorpus (&corpus());
    for (auto& h : e.renderBars (s, 0, bars))
        out.push_back ({ (double) h.beat, (int) h.lane, (float) h.velocity / 127.0f, (int) h.variant });
    return out;
}

std::shared_ptr<const std::vector<DrumHit>> drummerHits (const DrummerParams& p, double bpm, double lengthBeats)
{
    static std::mutex m;
    static std::map<juce::String, std::shared_ptr<const std::vector<DrumHit>>> cache;
    const int bars = juce::jlimit (1, 1024, (int) std::ceil (lengthBeats / 4.0 - 1e-6));
    const auto key = p.performanceKey() + "|" + juce::String (juce::roundToInt (bpm)) + "|" + juce::String (bars);
    {
        std::lock_guard<std::mutex> l (m);
        if (auto it = cache.find (key); it != cache.end()) return it->second;
    }
    auto hits = std::make_shared<const std::vector<DrumHit>> (render (p, bpm, bars));
    std::lock_guard<std::mutex> l (m);
    if (cache.size() > 256) cache.clear();
    cache[key] = hits;
    return hits;
}

static juce::File bundledSoundsDir()
{
   #if JUCE_MAC
    auto d = juce::File::getSpecialLocation (juce::File::currentApplicationFile).getChildFile ("Contents/Resources/Sounds");
   #else
    auto d = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("Sounds");
   #endif
    if (! d.isDirectory())   // dev builds / tests: the repo's Sounds folder
        for (auto f = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory(); f.getParentDirectory() != f; f = f.getParentDirectory())
            if (f.getChildFile ("Sounds/Rock Drums").isDirectory()) return f.getChildFile ("Sounds");
    return d;
}

juce::Array<juce::File> drumKits()
{
    juce::Array<juce::File> kits;
    for (auto& f : bundledSoundsDir().getChildFile ("Rock Drums").findChildFiles (juce::File::findDirectories, false))
        if (f.getChildFile ("kit.json").existsAsFile()) kits.add (f);
    kits.sort();
    return kits;
}

juce::File defaultDrumKit()
{
    const auto kits = drumKits();
    for (auto& k : kits) if (k.getFileName().containsIgnoreCase ("Naked")) return k;
    return kits.isEmpty() ? juce::File() : kits[0];
}

int addDrummerTrack (Project& p, double startBeat, int bars)
{
    Channel c; c.type = ChannelType::Drummer; c.name = "Drummer"; c.mixerTrack = -1; c.keyTrack = false;
    c.sampleFile = defaultDrumKit(); c.colour = juce::Colour (0xffffc93d); c.volume = 0.8f;
    const int id = p.addChannel (c);
    Lane l; l.name = "Drummer"; l.colour = juce::Colour (0xffffc93d);
    for (auto& ch : p.channels) if (ch.id == id) l.mixerTrack = ch.mixerTrack;
    p.lanes.push_back (l);
    Clip r; r.uid = p.newUid(); r.lane = (int) p.lanes.size() - 1; r.start = startBeat; r.length = 4.0 * bars; r.drummer = id;
    p.clips.push_back (r);
    p.songMode = true;
    return id;
}

int makeLaneDrummer (Project& p, int lane, double startBeat, int bars)
{
    if (lane < 0 || lane >= (int) p.lanes.size()) return addDrummerTrack (p, startBeat, bars);
    auto& l = p.lanes[(size_t) lane];
    if (l.mixerTrack <= 0) l.mixerTrack = p.addMixerTrack ("Drummer");
    Channel c; c.type = ChannelType::Drummer; c.name = "Drummer"; c.mixerTrack = l.mixerTrack; c.keyTrack = false;
    c.sampleFile = defaultDrumKit(); c.colour = juce::Colour (0xffffc93d); c.volume = 0.8f;
    const int id = p.addChannel (c);
    if (l.name.startsWith ("Track ") || l.name.isEmpty()) l.name = "Drummer";
    if (l.colour.isTransparent()) l.colour = juce::Colour (0xffffc93d);
    l.armed = false;
    Clip r; r.uid = p.newUid(); r.lane = lane; r.start = startBeat; r.length = 4.0 * bars; r.drummer = id;
    p.clips.push_back (r);
    p.songMode = true;
    return id;
}

// ---------------------------------------------------------------- instrument: the multi-mic kit + its mix
class DrummerInstrument : public ChannelInstrument
{
public:
    explicit DrummerInstrument (juce::File folder) : kitFolder (std::move (folder)) {}
    void prepare (double s, int maxBlock) override
    {
        sr = s; kit->prepare (s, juce::jmax (maxBlock, 4096)); tmp.setSize (2, juce::jmax (maxBlock, 4096));
        if (! started && kitFolder.isDirectory())
        {
            started = true;
            auto k = kit; auto f = kitFolder;
            juce::Thread::launch ([k, f] { k->loadKitFolder (f); });   // big multi-mic kits load in the background; strokes are skipped until ready
        }
    }
    void noteOn (int key, float v, bool) override { kit->noteOn (key & 31, v, key >> 5); ring = (juce::int64) (sr * 8.0); }
    void noteOff (int) override {}
    void allNotesOff() override { kit->allNotesOff(); }
    void choke() override {}
    bool isActive() const override { return ring > 0; }
    void render (juce::AudioBuffer<float>& out, int start, int num) override
    {
        if (num <= 0) return;
        for (int done = 0; done < num;)
        {
            const int n = juce::jmin (num - done, tmp.getNumSamples());
            tmp.clear (0, n);
            kit->renderNextBlock (tmp, 0, n);
            for (int ch = 0; ch < out.getNumChannels(); ++ch) out.addFrom (ch, start + done, tmp, juce::jmin (ch, 1), 0, n);
            done += n;
        }
        ring -= num;
    }
    void update (const Channel& c) override
    {
        auto& d = c.drum;
        kit->setMixVoicing (d.voicing); kit->setPunch (d.punch); kit->setGlue (d.glue); kit->setDrive (d.drive);
        kit->setMicBlend (d.micBlend); kit->setRoomSpace (d.space);
        kit->setRoom (kit->getRoomSize(), kit->getRoomDamping(), d.roomMix);
        kit->setSqueeze (d.squeeze); kit->setSqueezeGlow (d.glow);
        for (int l = 0; l < hhx::NumLanes; ++l)
        {
            const int g = drumGroupOf (l);
            kit->setLaneGainDb (l, d.pieceDb[(size_t) g]); kit->setLanePan (l, d.piecePan[(size_t) g]);
            kit->setLaneTune (l, d.pieceTune[(size_t) g]); kit->setLaneDamp (l, d.pieceDamp[(size_t) g]);
        }
    }
private:
    std::shared_ptr<hhx::KitEngine> kit = std::make_shared<hhx::KitEngine>();
    juce::File kitFolder; juce::AudioBuffer<float> tmp;
    double sr = 44100; juce::int64 ring = 0; bool started = false;
};

std::unique_ptr<ChannelInstrument> createDrummerInstrument (const Channel& c, double sampleRate)
{
    auto i = std::make_unique<DrummerInstrument> (c.sampleFile.isDirectory() ? c.sampleFile : defaultDrumKit());
    i->prepare (sampleRate, 4096); i->update (c);
    return i;
}
}
