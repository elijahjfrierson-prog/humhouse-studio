#include "KitEngine.h"

#if __has_include(<BundledKitData.h>)
 #include <BundledKitData.h>
 #define HHX_HAS_BUNDLED_KIT 1
#else
 #define HHX_HAS_BUNDLED_KIT 0
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace hhx
{
    KitEngine::KitEngine()
    {
        formats.registerBasicFormats();
    }

    void KitEngine::prepare (double sampleRate, int maxBlockSize)
    {
        currentRate = sampleRate;
        bleedLine.fill (0.0f);
        bleedWrite = 0;

        const int size = juce::jmax (64, maxBlockSize);
        laneBus.setSize (2, size, false, true, true);
        reverbBus.setSize (2, size, false, true, true);
        room.setSampleRate (sampleRate);
        room.reset();
        roomDelayLine.fill (0.0f);
        roomDelayWrite = 0;
        roomToneL = 0.0f;
        roomToneR = 0.0f;
        roomDuckEnv = 0.0f;
        for (auto& l : lanes)
        {
            l.compEnv = 0.0f;
            l.hpState[0] = l.hpState[1] = 0.0f;
            l.fastEnv = l.slowEnv = 0.0f;
        }

        busCompEnv = 0.0f;
        busTiltL = busTiltR = 0.0f;
        busCeiling = 1.0f;

        allNotesOff();
    }

    float KitEngine::releaseCoefficient (float seconds) const
    {
        return (float) std::pow (0.001, 1.0 / juce::jmax (1.0, (double) seconds * currentRate));
    }

    int KitEngine::pieceNameToLane (const juce::String& piece)
    {
        const auto p = piece.toLowerCase();
        if (p.startsWith ("kick"))          return LaneKick;
        if (p.startsWith ("snare_ghost"))   return LaneSnareGhost;
        if (p.startsWith ("snare_flam"))    return LaneSnareFlam;
        if (p.startsWith ("snare_roll"))    return LaneSnareRoll;
        if (p.startsWith ("snare_rim"))     return LaneSnareRim;
        if (p.startsWith ("snare"))         return LaneSnare;
        if (p.startsWith ("rim"))           return LaneSnareRim;
        if (p.startsWith ("sidestick"))     return LaneSideStick;
        if (p.startsWith ("side_stick"))    return LaneSideStick;
        if (p.startsWith ("hat_closed"))    return LaneHatClosed;
        if (p.startsWith ("hat_tight"))     return LaneHatTight;
        if (p.startsWith ("hat_pedal"))     return LaneHatPedal;
        if (p.startsWith ("hat_splash"))    return LaneHatSplash;
        if (p.startsWith ("hat_bell"))      return LaneHatBell;
        if (p.startsWith ("hat_open_1"))    return LaneHatOpen1;
        if (p.startsWith ("hat_open_2"))    return LaneHatOpen2;
        if (p.startsWith ("hat_open_3"))    return LaneHatOpen3;
        if (p.startsWith ("hat_open_4"))    return LaneHatOpen4;
        if (p.startsWith ("hat_open"))      return LaneHatOpen2;
        if (p.startsWith ("tom_high"))      return LaneTom1;
        if (p.startsWith ("tom_1"))         return LaneTom1;
        if (p.startsWith ("tom_mid"))       return LaneTom2;
        if (p.startsWith ("tom_2"))         return LaneTom2;
        if (p.startsWith ("tom_low"))       return LaneTom3;
        if (p.startsWith ("tom_3"))         return LaneTom3;
        if (p.startsWith ("tom_4"))         return LaneTom4;
        if (p.startsWith ("floor_tom"))     return LaneTom3;
        if (p.startsWith ("left_crash"))    return LaneCrashL;
        if (p.startsWith ("right_crash"))   return LaneCrashR;
        if (p.startsWith ("crash_3"))       return LaneCrash3;
        if (p.startsWith ("crash"))         return LaneCrashR;
        if (p.startsWith ("china"))         return LaneChina;
        if (p.startsWith ("splash"))        return LaneSplash;
        if (p.startsWith ("ride_bell"))     return LaneRideBell;
        if (p.startsWith ("ride_edge"))     return LaneRideEdge;
        if (p.startsWith ("ride_crash"))    return LaneRideCrash;
        if (p.startsWith ("ride"))          return LaneRideBow;
        if (p.startsWith ("perc"))          return LanePerc;
        return -1;
    }

    int KitEngine::micNameToIndex (const juce::String& mic)
    {
        const auto m = mic.toLowerCase();
        if (m.startsWith ("oh") || m.startsWith ("over")) return MicOverhead;
        if (m.startsWith ("room") || m.startsWith ("amb")) return MicRoom;
        return MicClose;
    }

    KitEngine::Placement KitEngine::placementFromFilename (const juce::String& name)
    {
        // `<piece>[_v<layer>][_rr<variant>][_<mic>]`, e.g. snare_v3_rr2_oh.
        Placement p;
        auto piece = name;

        const auto strip = [&piece] (const juce::String& token) -> juce::String
        {
            const auto lower = piece.toLowerCase();
            const int at = lower.lastIndexOf ("_" + token);
            if (at < 0)
                return {};
            const auto tail = piece.substring (at + 1 + token.length());
            if (! tail.containsOnly ("0123456789") || tail.isEmpty())
                return {};
            piece = piece.substring (0, at);
            return tail;
        };

        for (const int mic : { MicOverhead, MicRoom })
        {
            const juce::StringArray tokens = mic == MicOverhead
                ? juce::StringArray { "oh", "overhead" }
                : juce::StringArray { "room", "amb" };
            for (const auto& t : tokens)
                if (piece.toLowerCase().endsWith ("_" + t))
                {
                    p.mic = mic;
                    piece = piece.dropLastCharacters (t.length() + 1);
                }
        }

        if (const auto rr = strip ("rr"); rr.isNotEmpty())
            p.variant = std::max (0, rr.getIntValue() - 1);
        if (const auto v = strip ("v"); v.isNotEmpty())
            p.layer = std::max (0, v.getIntValue() - 1);

        p.lane = pieceNameToLane (piece);
        return p;
    }

    int KitEngine::resolveLane (int lane) const
    {
        if (lane < 0 || lane >= NumLanes)
            return -1;
        if (! lanes[(std::size_t) lane].layers.empty())
            return lane;

        // Fallback chains, nearest relative first.
        static const std::vector<std::vector<int>> chains = {
            { LaneSnareGhost,  LaneSnare },
            { LaneSnareFlam,   LaneSnare },
            { LaneSnareRoll,   LaneSnare },
            { LaneSnareRim,    LaneSnare },
            { LaneSideStick,   LaneSnareRim, LaneSnare },
            { LaneHatTight,    LaneHatClosed },
            { LaneHatOpen1,    LaneHatOpen2, LaneHatClosed },
            { LaneHatOpen2,    LaneHatOpen3, LaneHatClosed },
            { LaneHatOpen3,    LaneHatOpen2, LaneHatClosed },
            { LaneHatOpen4,    LaneHatOpen3, LaneHatOpen2, LaneHatClosed },
            { LaneHatPedal,    LaneHatClosed },
            { LaneHatSplash,   LaneHatOpen2, LaneHatClosed },
            { LaneHatBell,     LaneHatClosed },
            { LaneRideEdge,    LaneRideBow },
            { LaneRideCrash,   LaneCrashR, LaneRideBow },
            { LaneRideBell,    LaneRideBow },
            { LaneCrash3,      LaneCrashR, LaneCrashL },
            { LaneChina,       LaneCrashL, LaneCrashR },
            { LaneSplash,      LaneCrashL, LaneCrashR },
            { LaneTom4,        LaneTom3, LaneTom2 },
            { LaneTom3,        LaneTom2, LaneTom1 },
            { LaneTom2,        LaneTom1, LaneTom3 },
            { LanePerc,        LaneSideStick, LaneSnareRim },
        };

        for (const auto& chain : chains)
        {
            if (chain.front() != lane)
                continue;
            for (std::size_t i = 1; i < chain.size(); ++i)
                if (! lanes[(std::size_t) chain[i]].layers.empty())
                    return chain[i];
        }
        return -1;
    }

    void KitEngine::clearKit()
    {
        {
            // Voices own their samples, so a kit swap lets the ringing ones fade
            // out over ~40 ms instead of being cut, which would click.
            const juce::ScopedLock sl (voiceLock);
            const float fade = releaseCoefficient (0.040f);
            for (auto& v : voices)
                if (v.active)
                    v.release = juce::jmin (v.release, fade);
        }

        for (auto& l : lanes)
            l.layers.clear();

        clearKitVoicing();
    }

    void KitEngine::place (const Placement& p, std::shared_ptr<Sample> s)
    {
        if (p.lane < 0 || p.lane >= NumLanes || s == nullptr)
            return;

        auto& slot = lanes[(std::size_t) p.lane];

        // A file with no declared layer becomes its own layer; loudness sorting
        // afterwards puts the set in order.
        const int layerIndex = p.layer >= 0 ? p.layer : (int) slot.layers.size();
        if ((int) slot.layers.size() <= layerIndex)
            slot.layers.resize ((std::size_t) layerIndex + 1);

        auto& layer = slot.layers[(std::size_t) layerIndex];
        if ((int) layer.variants.size() <= p.variant)
            layer.variants.resize ((std::size_t) p.variant + 1);

        layer.variants[(std::size_t) p.variant].mics[(std::size_t) p.mic] = std::move (s);
    }

    int KitEngine::numLoadedSamples() const
    {
        const juce::ScopedLock layers (layerLock);
        int total = 0;
        for (const auto& slot : lanes)
            for (const auto& layer : slot.layers)
                for (const auto& variant : layer.variants)
                    for (const auto& sample : variant.mics)
                        if (sample != nullptr)
                            ++total;
        return total;
    }

    void KitEngine::finaliseKit (bool sortByLoudness)
    {
        const auto loudness = [] (const Layer& l) -> float
        {
            float peak = 0.0f;
            for (const auto& v : l.variants)
                if (const auto& s = v.mics[MicClose])
                    peak = std::max (peak, s->peak);
            return peak;
        };

        for (auto& slot : lanes)
        {
            slot.layers.erase (std::remove_if (slot.layers.begin(), slot.layers.end(),
                                               [] (const Layer& l)
                                               {
                                                   for (const auto& v : l.variants)
                                                       if (v.mics[MicClose] != nullptr)
                                                           return false;
                                                   return true;
                                               }),
                               slot.layers.end());

            if (sortByLoudness)
                std::stable_sort (slot.layers.begin(), slot.layers.end(),
                                  [&loudness] (const Layer& a, const Layer& b)
                                  { return loudness (a) < loudness (b); });

            for (auto& layer : slot.layers)
                layer.variants.erase (std::remove_if (layer.variants.begin(), layer.variants.end(),
                                                      [] (const Variant& v)
                                                      { return v.mics[MicClose] == nullptr; }),
                                      layer.variants.end());
        }

        dropRinglessShutHat();
    }

    float KitEngine::laneRingRatio (int lane) const
    {
        if (lane < 0 || lane >= NumLanes)
            return 0.0f;

        const auto& slot = lanes[(std::size_t) lane];
        if (slot.layers.empty())
            return 0.0f;

        // Energy left after the stroke has gone by, against the energy of the
        // stroke itself: a cymbal that rings keeps a fraction of its attack
        // going, a damped tick keeps nothing.
        float most = 0.0f;
        for (const auto& variant : slot.layers.back().variants)
        {
            const auto& s = variant.mics[MicClose];
            if (s == nullptr || s->numFrames <= 0)
                continue;

            const int chans  = std::max (1, s->numChannels);
            const int attack = (int) (0.020 * s->sourceRate) * chans;
            const int tail   = (int) (0.060 * s->sourceRate) * chans;
            const int n      = (int) s->data.size();
            if (n <= tail)
                continue;

            double attackEnergy = 0.0, tailEnergy = 0.0;
            for (int i = 0; i < std::min (attack, n); ++i)
                attackEnergy += (double) s->data[(std::size_t) i] * s->data[(std::size_t) i];
            for (int i = tail; i < n; ++i)
                tailEnergy += (double) s->data[(std::size_t) i] * s->data[(std::size_t) i];

            if (attackEnergy > 0.0)
                most = std::max (most, (float) (tailEnergy / attackEnergy));
        }
        return most;
    }

    void KitEngine::dropRinglessShutHat()
    {
        // The shut hat a groove keeps time on has to be the one that rings; a
        // damped tick next to it in the openness ladder is heard as a click in
        // the middle of the part.
        const float closedRing = laneRingRatio (LaneHatClosed);
        const float tightRing  = laneRingRatio (LaneHatTight);
        if (closedRing <= 0.0f || tightRing <= 0.0f)
            return;

        if (tightRing < closedRing * 0.35f)
            lanes[(std::size_t) LaneHatTight].layers.clear();
        else if (closedRing < tightRing * 0.35f)
            lanes[(std::size_t) LaneHatClosed].layers = lanes[(std::size_t) LaneHatTight].layers;
    }

    std::shared_ptr<KitEngine::Sample> KitEngine::readSample (juce::InputStream* stream)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (
            std::unique_ptr<juce::InputStream> (stream)));
        if (reader == nullptr || reader->lengthInSamples <= 0)
            return nullptr;

        const int channels = (int) juce::jmin<juce::uint32> (2u, reader->numChannels);
        const int frames   = (int) reader->lengthInSamples;

        juce::AudioBuffer<float> scratch (channels, frames);
        reader->read (&scratch, 0, frames, 0, true, reader->numChannels > 1);

        auto sample = std::make_shared<Sample>();
        sample->sourceRate  = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
        sample->numChannels = channels;
        sample->numFrames   = frames;
        sample->peak        = scratch.getMagnitude (0, frames);
        sample->data.resize ((std::size_t) frames * (std::size_t) channels);

        for (int ch = 0; ch < channels; ++ch)
        {
            const auto* src = scratch.getReadPointer (ch);
            for (int i = 0; i < frames; ++i)
                sample->data[(std::size_t) i * (std::size_t) channels + (std::size_t) ch]
                    = (std::int16_t) juce::jlimit (-32768, 32767,
                                                   (int) std::lround (src[i] * 32767.0f));
        }
        return sample;
    }

    int KitEngine::loadBundledKit (const juce::String& kitPrefix)
    {
       #if HHX_HAS_BUNDLED_KIT
        const juce::ScopedLock layers (layerLock);
        clearKit();

        int loaded = 0;
        for (int i = 0; i < BundledKitData::namedResourceListSize; ++i)
        {
            const juce::String resName (BundledKitData::namedResourceList[i]);
            const juce::String original (BundledKitData::originalFilenames[i]);
            if (! original.startsWith (kitPrefix + "__"))
                continue;

            const auto piece = original.fromFirstOccurrenceOf ("__", false, false)
                                       .upToLastOccurrenceOf (".wav", false, false);
            const auto placement = placementFromFilename (piece);
            if (placement.lane < 0)
                continue;

            int size = 0;
            const auto* data = BundledKitData::getNamedResource (resName.toRawUTF8(), size);
            if (data == nullptr || size <= 0)
                continue;

            if (auto sample = readSample (new juce::MemoryInputStream (data, (std::size_t) size, false)))
            {
                place (placement, std::move (sample));
                ++loaded;
            }
        }

        finaliseKit (true);
        kitTrimDb.store (0.0f);
        {
            const juce::ScopedLock sl (kitNameLock);
            kitName = kitPrefix;
            kitVersion = "1";
        }
        return loaded;
       #else
        juce::ignoreUnused (kitPrefix);
        return 0;
       #endif
    }

    void KitEngine::clearKitVoicing()
    {
        for (auto& v : kitVoicing)
        {
            v.tune.store (0.0f);
            v.damp.store (0.0f);
            v.gainDb.store (0.0f);
        }
    }

    int KitEngine::loadFromManifest (const juce::File& folder, const juce::var& manifest)
    {
        if (auto* obj = manifest.getDynamicObject())
        {
            const juce::ScopedLock sl (kitNameLock);
            kitName    = obj->getProperty ("name").toString();
            kitVersion = obj->getProperty ("version").toString();
            if (kitName.isEmpty())
                kitName = folder.getFileName();
            if (kitVersion.isEmpty())
                kitVersion = "1";
        }

        // Kits are recorded and normalised by whoever made them, so each one
        // carries the trim that brings it to the same working level.
        kitTrimDb.store (manifest.hasProperty ("trim")
                             ? juce::jlimit (-12.0f, 12.0f, (float) (double) manifest["trim"])
                             : 0.0f);

        // A kit may be a voicing of recordings that already ship: "samples"
        // points at the folder that holds them, so a re-tuned, taped-up kit
        // costs a manifest rather than a second copy of the audio.
        auto sampleFolder = folder;
        if (manifest.hasProperty ("samples"))
        {
            const auto rel = manifest["samples"].toString();
            if (rel.isNotEmpty())
            {
                const auto resolved = folder.getChildFile (rel);
                if (resolved.isDirectory())
                    sampleFolder = resolved;
            }
        }

        clearKitVoicing();
        if (const auto* voicings = manifest["voicing"].getArray())
        {
            for (const auto& entry : *voicings)
            {
                const int lane = pieceNameToLane (entry["piece"].toString());
                if (lane < 0 || lane >= NumLanes)
                    continue;

                auto& v = kitVoicing[(std::size_t) lane];
                if (entry.hasProperty ("tune"))
                    v.tune.store (juce::jlimit (-12.0f, 12.0f, (float) (double) entry["tune"]));
                if (entry.hasProperty ("damp"))
                    v.damp.store (juce::jlimit (0.0f, 1.0f, (float) (double) entry["damp"]));
                if (entry.hasProperty ("gain"))
                    v.gainDb.store (juce::jlimit (-24.0f, 12.0f, (float) (double) entry["gain"]));
            }
        }

        const auto* pieces = manifest["pieces"].getArray();
        if (pieces == nullptr)
            return 0;

        // Several articulations share a recording, so a file is decoded once and
        // the buffer is handed to every placement that names it.
        std::map<juce::String, std::shared_ptr<Sample>> decoded;

        int loaded = 0;
        for (const auto& entry : *pieces)
        {
            Placement p;
            p.lane = pieceNameToLane (entry["piece"].toString());
            if (p.lane < 0)
                continue;

            p.layer   = entry.hasProperty ("layer")   ? std::max (0, (int) entry["layer"] - 1) : -1;
            p.variant = entry.hasProperty ("variant") ? std::max (0, (int) entry["variant"] - 1) : 0;
            p.mic     = micNameToIndex (entry["mic"].toString());

            const auto name = entry["file"].toString();
            const auto file = sampleFolder.getChildFile (name);
            if (! file.existsAsFile())
                continue;

            auto& cached = decoded[name];
            if (cached == nullptr)
                cached = readSample (new juce::FileInputStream (file));

            if (cached != nullptr)
            {
                place (p, cached);
                ++loaded;
            }
        }
        return loaded;
    }

    int KitEngine::loadKitFolder (const juce::File& folder)
    {
        if (! folder.isDirectory())
            return 0;

        const juce::ScopedLock layers (layerLock);
        clearKit();

        int loaded = 0;
        bool declaredLayers = false;
        const auto manifestFile = folder.getChildFile ("kit.json");
        if (manifestFile.existsAsFile())
        {
            const auto manifest = juce::JSON::parse (manifestFile);
            loaded = loadFromManifest (folder, manifest);
            declaredLayers = loaded > 0;
        }

        if (loaded == 0)
        {
            for (const auto& entry : juce::RangedDirectoryIterator (folder, false, "*.wav;*.flac"))
            {
                const auto name  = entry.getFile().getFileNameWithoutExtension();
                const auto piece = name.contains ("__") ? name.fromFirstOccurrenceOf ("__", false, false) : name;
                const auto placement = placementFromFilename (piece);
                if (placement.lane < 0)
                    continue;

                if (auto sample = readSample (new juce::FileInputStream (entry.getFile())))
                {
                    place (placement, std::move (sample));
                    ++loaded;
                }
            }

            kitTrimDb.store (0.0f);
            const juce::ScopedLock sl (kitNameLock);
            kitName    = folder.getFileName();
            kitVersion = "1";
        }

        finaliseKit (! declaredLayers);
        return loaded;
    }

    juce::String KitEngine::getKitName() const
    {
        const juce::ScopedLock sl (kitNameLock);
        return kitName;
    }

    juce::String KitEngine::getKitVersion() const
    {
        const juce::ScopedLock sl (kitNameLock);
        return kitVersion;
    }

    int KitEngine::numLayersForLane (int lane) const
    {
        if (lane < 0 || lane >= NumLanes)
            return 0;
        const juce::ScopedLock layers (layerLock);
        return (int) lanes[(std::size_t) lane].layers.size();
    }

    int KitEngine::numVariantsForLane (int lane, int layer) const
    {
        if (lane < 0 || lane >= NumLanes)
            return 0;
        const juce::ScopedLock layers (layerLock);
        const auto& slot = lanes[(std::size_t) lane];
        if (layer < 0 || layer >= (int) slot.layers.size())
            return 0;
        return (int) slot.layers[(std::size_t) layer].variants.size();
    }

    bool KitEngine::laneHasMic (int lane, int mic) const
    {
        if (lane < 0 || lane >= NumLanes || mic < 0 || mic >= NumMics)
            return false;
        const juce::ScopedLock layers (layerLock);
        for (const auto& layer : lanes[(std::size_t) lane].layers)
            for (const auto& v : layer.variants)
                if (v.mics[(std::size_t) mic] != nullptr)
                    return true;
        return false;
    }

    void KitEngine::setLaneSampleSwitch (int lane, int offset)
    {
        if (lane >= 0 && lane < NumLanes)
            lanes[(std::size_t) lane].sampleSwitch.store (offset);
    }

    int KitEngine::getLaneSampleSwitch (int lane) const
    {
        return (lane >= 0 && lane < NumLanes) ? lanes[(std::size_t) lane].sampleSwitch.load() : 0;
    }

    void KitEngine::setLaneGainDb (int lane, float db)
    {
        if (lane >= 0 && lane < NumLanes)
            lanes[(std::size_t) lane].gainDb.store (db);
    }

    float KitEngine::getLaneGainDb (int lane) const
    {
        return (lane >= 0 && lane < NumLanes) ? lanes[(std::size_t) lane].gainDb.load() : 0.0f;
    }

    void KitEngine::setLaneTune (int lane, float semitones)
    {
        if (lane >= 0 && lane < NumLanes)
            lanes[(std::size_t) lane].tune.store (juce::jlimit (-12.0f, 12.0f, semitones));
    }

    float KitEngine::getLaneTune (int lane) const
    {
        return (lane >= 0 && lane < NumLanes) ? lanes[(std::size_t) lane].tune.load() : 0.0f;
    }

    void KitEngine::setLaneDamp (int lane, float amount01)
    {
        if (lane >= 0 && lane < NumLanes)
            lanes[(std::size_t) lane].damp.store (juce::jlimit (0.0f, 1.0f, amount01));
    }

    float KitEngine::getLaneDamp (int lane) const
    {
        return (lane >= 0 && lane < NumLanes) ? lanes[(std::size_t) lane].damp.load() : 0.0f;
    }

    void KitEngine::setLaneCompression (int lane, float amount01)
    {
        if (lane >= 0 && lane < NumLanes)
            lanes[(std::size_t) lane].compression.store (juce::jlimit (0.0f, 1.0f, amount01));
    }

    float KitEngine::getLaneCompression (int lane) const
    {
        return (lane >= 0 && lane < NumLanes) ? lanes[(std::size_t) lane].compression.load() : 0.0f;
    }

    void KitEngine::setLaneReverbSend (int lane, float amount01)
    {
        if (lane >= 0 && lane < NumLanes)
            lanes[(std::size_t) lane].reverbSend.store (juce::jlimit (0.0f, 1.0f, amount01));
    }

    float KitEngine::getLaneReverbSend (int lane) const
    {
        return (lane >= 0 && lane < NumLanes) ? lanes[(std::size_t) lane].reverbSend.load() : 0.0f;
    }

    void KitEngine::setRoom (float size01, float damping01, float mix01)
    {
        roomSize.store (juce::jlimit (0.0f, 1.0f, size01));
        roomDamping.store (juce::jlimit (0.0f, 1.0f, damping01));
        roomMix.store (juce::jlimit (0.0f, 1.0f, mix01));
    }

    void KitEngine::setRoomSpace (int space)
    {
        roomSpace.store (juce::jlimit (0, (int) NumRoomSpaces - 1, space));
    }

    int KitEngine::getRoomSpace() const { return roomSpace.load(); }

    void KitEngine::setRoomDuck (float amount01)
    {
        roomDuck.store (juce::jlimit (0.0f, 1.0f, amount01));
    }

    float KitEngine::getRoomDuck() const { return roomDuck.load(); }

    float KitEngine::getRoomSize()    const { return roomSize.load(); }
    float KitEngine::getRoomDamping() const { return roomDamping.load(); }
    float KitEngine::getRoomMix()     const { return roomMix.load(); }

    void KitEngine::setLanePan (int lane, float pan)
    {
        if (lane >= 0 && lane < NumLanes)
            lanes[(std::size_t) lane].pan.store (juce::jlimit (-1.0f, 1.0f, pan));
    }

    float KitEngine::getLanePan (int lane) const
    {
        return (lane >= 0 && lane < NumLanes) ? lanes[(std::size_t) lane].pan.load() : 0.0f;
    }

    void KitEngine::setMicBlend (float blend01) { micBlend.store (juce::jlimit (0.0f, 1.0f, blend01)); }
    float KitEngine::getMicBlend() const       { return micBlend.load(); }
    void KitEngine::setBleed (float amount01)  { bleed.store (juce::jlimit (0.0f, 1.0f, amount01)); }
    float KitEngine::getBleed() const          { return bleed.load(); }
    void KitEngine::setCrush (float amount01)  { crush.store (juce::jlimit (0.0f, 1.0f, amount01)); }
    float KitEngine::getCrush() const          { return crush.load(); }

    void KitEngine::setMixVoicing (int voicing)
    {
        mixVoicing.store (juce::jlimit (0, (int) NumMixVoicings - 1, voicing));
    }

    int   KitEngine::getMixVoicing() const     { return mixVoicing.load(); }
    void  KitEngine::setPunch (float amount01) { punch.store (juce::jlimit (0.0f, 1.0f, amount01)); }
    float KitEngine::getPunch() const          { return punch.load(); }
    void  KitEngine::setGlue (float amount01)  { glue.store (juce::jlimit (0.0f, 1.0f, amount01)); }
    float KitEngine::getGlue() const           { return glue.load(); }
    void  KitEngine::setDrive (float amount01) { drive.store (juce::jlimit (0.0f, 1.0f, amount01)); }
    float KitEngine::getDrive() const          { return drive.load(); }

    void  KitEngine::setSqueeze (float amount01) { squeeze.store (juce::jlimit (0.0f, 1.0f, amount01)); }
    float KitEngine::getSqueeze() const          { return squeeze.load(); }
    void  KitEngine::setSqueezeGlow (int glow)
    {
        squeezeGlow.store (juce::jlimit (0, (int) NumSqueezeGlows - 1, glow));
    }
    int   KitEngine::getSqueezeGlow() const      { return squeezeGlow.load(); }

    KitEngine::LaneVoicing KitEngine::voicingForLane (int lane, int voicing)
    {
        if (voicing == MixRaw)
            return {};

        // Where each family stops being useful at the bottom, and how much of it
        // is stick rather than shell. These are the moves an engineer makes on
        // every rock kit before anything else: the kick keeps its fundamental
        // and loses the rumble under it, the snare gives up the low end it only
        // shares with the kick, and the cymbals are high-passed hard because
        // everything they have below 300 Hz is spill.
        LaneVoicing v;
        if (lane == LaneKick)                       v = { 28.0f,  0.55f, 0.8f };
        else if (isSnareLane (lane))                v = { 85.0f,  0.70f, 0.6f };
        else if (isTomLane (lane))                  v = { 70.0f,  0.45f, 0.4f };
        else if (isHatLane (lane))                  v = { 340.0f, 0.35f, 0.0f };
        else if (isRideLane (lane) || isCymbalLane (lane)) v = { 260.0f, 0.20f, 0.0f };
        else                                        v = { 180.0f, 0.30f, 0.2f };

        // The voicings differ in how hard those moves are pushed, not in what
        // they are: Modern is the mixed-record default, Punch leans on the
        // transient, Room leaves the low end and the ring alone, Vintage is the
        // softest treatment of the four.
        switch (voicing)
        {
            case MixPunch:   v.attack *= 1.6f; v.sustainTrim *= 1.6f; break;
            case MixRoom:    v.attack *= 0.6f; v.sustainTrim  = 0.0f;
                             v.highPassHz *= 0.7f;                    break;
            case MixVintage: v.attack *= 0.5f; v.sustainTrim *= 0.4f;
                             v.highPassHz *= 0.8f;                    break;
            default: break;   // Modern
        }

        return v;
    }

    void KitEngine::shapeLane (LaneSlot& slot, int lane, float* busL, float* busR, int numSamples)
    {
        const int   voicing = mixVoicing.load();
        const auto  v       = voicingForLane (lane, voicing);
        const float amount  = punch.load();

        if (voicing == MixRaw || numSamples <= 0)
            return;

        if (v.highPassHz > 1.0f)
        {
            // One-pole high-pass per side, taken as the difference between the
            // signal and its own low-passed copy.
            const float coeff = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi
                                                 * v.highPassHz / (float) currentRate);
            for (int i = 0; i < numSamples; ++i)
            {
                slot.hpState[0] += coeff * (busL[i] - slot.hpState[0]);
                busL[i] -= slot.hpState[0];
                if (busR != nullptr)
                {
                    slot.hpState[1] += coeff * (busR[i] - slot.hpState[1]);
                    busR[i] -= slot.hpState[1];
                }
            }
        }

        // Transient design: two envelope followers on the same signal, one that
        // keeps up with the stick and one that only hears the shell. Where they
        // differ is the attack, so their difference is the gain. This is what
        // makes a close mic read in a dense mix without simply being louder.
        const float attackAmt  = v.attack      * (0.4f + 1.2f * amount);
        const float sustainAmt = v.sustainTrim * (0.4f + 1.2f * amount);
        if (attackAmt <= 0.001f && sustainAmt <= 0.001f)
            return;

        const float fast = 1.0f - std::exp (-1.0f / (0.0015f * (float) currentRate));
        const float slow = 1.0f - std::exp (-1.0f / (0.030f  * (float) currentRate));

        for (int i = 0; i < numSamples; ++i)
        {
            const float peak = juce::jmax (std::abs (busL[i]),
                                           busR != nullptr ? std::abs (busR[i]) : 0.0f);
            slot.fastEnv += fast * (peak - slot.fastEnv);
            slot.slowEnv += slow * (peak - slot.slowEnv);

            const float diff = slot.fastEnv - slot.slowEnv;
            const float norm = diff / (slot.fastEnv + 1.0e-4f);

            float gain = 1.0f;
            if (norm > 0.0f)  gain += attackAmt  * norm;          // stick
            else              gain += sustainAmt * norm * 0.5f;   // ring

            gain = juce::jlimit (0.25f, 4.0f, gain);
            busL[i] *= gain;
            if (busR != nullptr)
                busR[i] *= gain;
        }
    }

    void KitEngine::processSqueeze (float* outL, float* outR, int numSamples)
    {
        const float amount = squeeze.load();
        const int   glow   = squeezeGlow.load();
        if (amount <= 0.001f || numSamples <= 0)
            return;

        // Crossovers, and what each band is levelled towards. The band a kit
        // most often has too much of is 180-600 Hz - the boxiness - and the one
        // that turns harsh under compression is the stick band, so those two are
        // held tightest. The bottom is allowed to stay big and the air band is
        // mostly lifted rather than squeezed.
        static constexpr float kSplitHz[3]  { 180.0f, 600.0f, 4000.0f };
        static constexpr float kThreshDb[4] { -20.0f, -26.0f, -25.0f, -30.0f };
        static constexpr float kRatio[4]    {   2.0f,   4.0f,   3.2f,   2.2f };
        static constexpr float kFloorDb[4]  { -34.0f, -40.0f, -38.0f, -44.0f };
        static constexpr float kLiftDb[4]   {   1.5f,   0.0f,   2.0f,   3.0f };

        float coeff[3];
        for (int b = 0; b < 3; ++b)
            coeff[b] = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi
                                        * kSplitHz[b] / (float) currentRate);

        const float attack  = 1.0f - std::exp (-1.0f / (0.004f * (float) currentRate));
        const float release = 1.0f - std::exp (-1.0f / (0.120f * (float) currentRate));
        const float smooth  = 1.0f - std::exp (-1.0f / (0.010f * (float) currentRate));

        // The glow stage: how the harmonics are made once the bands are level.
        // Clean adds none, tube is asymmetric and mostly in the mids and top,
        // tape rounds the bottom, and the transformer thickens the low mids.
        const float glowAmt = glow == GlowOff ? 0.0f : amount;

        const bool stereo = outR != nullptr;

        for (int i = 0; i < numSamples; ++i)
        {
            float in[2] { outL[i], stereo ? outR[i] : 0.0f };
            const int channels = stereo ? 2 : 1;

            float band[4][2] {};
            for (int ch = 0; ch < channels; ++ch)
            {
                float remaining = in[ch];
                for (int b = 0; b < 3; ++b)
                {
                    squeezeLp[b][ch] += coeff[b] * (remaining - squeezeLp[b][ch]);
                    band[b][ch] = squeezeLp[b][ch];
                    remaining  -= squeezeLp[b][ch];
                }
                band[3][ch] = remaining;
            }

            for (int b = 0; b < kSqueezeBands; ++b)
            {
                // The detector is summed across the sides so the stage never
                // moves the stereo image of the kit.
                float peak = std::abs (band[b][0]);
                if (stereo)
                    peak = juce::jmax (peak, std::abs (band[b][1]));

                squeezeEnv[b] += (peak > squeezeEnv[b] ? attack : release)
                                 * (peak - squeezeEnv[b]);

                const float env    = juce::jmax (1.0e-6f, squeezeEnv[b]);
                const float envDb  = juce::Decibels::gainToDecibels (env);
                float moveDb = 0.0f;
                if (envDb > kThreshDb[b])
                    moveDb = (kThreshDb[b] - envDb) * (1.0f - 1.0f / kRatio[b]);
                else if (envDb < kFloorDb[b] && kLiftDb[b] > 0.0f)
                    moveDb = juce::jmin (kLiftDb[b], (kFloorDb[b] - envDb) * 0.5f);

                const float target = juce::Decibels::decibelsToGain (moveDb * amount);
                squeezeGain[b] += smooth * (target - squeezeGain[b]);

                for (int ch = 0; ch < channels; ++ch)
                    band[b][ch] *= squeezeGain[b];
            }

            for (int ch = 0; ch < channels; ++ch)
            {
                if (glowAmt > 0.0f)
                {
                    switch (glow)
                    {
                        case GlowTube:
                            // Asymmetric: even harmonics, and only where the
                            // stick and the body live.
                            for (int b = 1; b < kSqueezeBands; ++b)
                            {
                                const float x = band[b][ch];
                                const float s = x - 0.22f * x * std::abs (x);
                                band[b][ch] += (s - x) * glowAmt;
                            }
                            break;
                        case GlowTape:
                            for (int b = 0; b < 2; ++b)
                            {
                                const float x = band[b][ch];
                                band[b][ch] += (std::tanh (x * 1.8f) / 1.8f - x) * glowAmt;
                            }
                            break;
                        case GlowTransformer:
                        {
                            const float x = band[1][ch];
                            band[1][ch] += (std::tanh (x * 2.4f) / 2.4f - x) * glowAmt;
                            band[2][ch] *= 1.0f + 0.05f * glowAmt;
                            break;
                        }
                        default:
                            break;
                    }
                }

                float sum = 0.0f;
                for (int b = 0; b < kSqueezeBands; ++b)
                    sum += band[b][ch];

                // Make-up: the squeeze is a level move, and the point of it is
                // that the kit arrives louder and denser, not thinner.
                sum *= juce::Decibels::decibelsToGain (2.5f * amount);

                if (ch == 0) outL[i] = sum;
                else         outR[i] = sum;
            }
        }
    }

    void KitEngine::processKitBus (float* outL, float* outR, int numSamples)
    {
        const int voicing = mixVoicing.load();
        if (voicing == MixRaw)
        {
            // The squeeze is a mix tool in its own right, so it still works on
            // a raw kit - it is the one stage Raw does not bypass.
            processSqueeze (outL, outR, numSamples);
            return;
        }

        processSqueeze (outL, outR, numSamples);

        const float glueAmt  = glue.load();
        const float driveAmt = drive.load();

        // Bus compression: slow enough to let the attack through, released over
        // a bar's worth of time so the kit breathes as one instrument. This is
        // the difference between separate samples and a kit.
        if (glueAmt > 0.001f)
        {
            const float threshold = juce::Decibels::decibelsToGain (-14.0f - 6.0f * glueAmt);
            const float ratio     = 1.6f + 2.4f * glueAmt;
            const float attack    = 1.0f - std::exp (-1.0f / (0.015f * (float) currentRate));
            const float release   = 1.0f - std::exp (-1.0f / (0.250f * (float) currentRate));
            const float makeUp    = juce::Decibels::decibelsToGain (3.5f * glueAmt);

            for (int i = 0; i < numSamples; ++i)
            {
                const float peak = juce::jmax (std::abs (outL[i]),
                                               outR != nullptr ? std::abs (outR[i]) : 0.0f);
                const float coeff = peak > busCompEnv ? attack : release;
                busCompEnv += coeff * (peak - busCompEnv);

                float gain = makeUp;
                if (busCompEnv > threshold)
                    gain *= std::pow (busCompEnv / threshold, 1.0f / ratio - 1.0f);

                outL[i] *= gain;
                if (outR != nullptr)
                    outR[i] *= gain;
            }
        }

        // Tilt: how the finished kit is balanced top to bottom. Vintage gives
        // some of the top back to the shells, Modern and Punch lift it.
        const float tiltDb = voicing == MixVintage ? -2.0f
                           : voicing == MixRoom    ? -0.5f
                           : voicing == MixPunch   ?  1.5f
                                                   :  1.0f;
        if (std::abs (tiltDb) > 0.01f)
        {
            const float coeff = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi
                                                 * 2200.0f / (float) currentRate);
            const float lift  = juce::Decibels::decibelsToGain (tiltDb) - 1.0f;
            for (int i = 0; i < numSamples; ++i)
            {
                busTiltL += coeff * (outL[i] - busTiltL);
                outL[i] += (outL[i] - busTiltL) * lift;
                if (outR != nullptr)
                {
                    busTiltR += coeff * (outR[i] - busTiltR);
                    outR[i] += (outR[i] - busTiltR) * lift;
                }
            }
        }

        // Saturation, then a ceiling. The drive is where the loudness comes
        // from - the peaks round over instead of growing - and the ceiling only
        // ever works on what is left, so a hard chorus cannot clip the host.
        if (driveAmt > 0.001f || voicing == MixVintage)
        {
            const float amt   = juce::jlimit (0.0f, 1.0f,
                                              driveAmt + (voicing == MixVintage ? 0.2f : 0.0f));
            const float pre   = 1.0f + 3.0f * amt;
            const float post  = 1.0f / std::tanh (pre * 0.7f) * 0.7f;
            for (int i = 0; i < numSamples; ++i)
            {
                outL[i] = outL[i] * (1.0f - amt) + std::tanh (outL[i] * pre) * post * amt;
                if (outR != nullptr)
                    outR[i] = outR[i] * (1.0f - amt) + std::tanh (outR[i] * pre) * post * amt;
            }
        }

        const float ceiling = juce::Decibels::decibelsToGain (-0.5f);
        const float recover = 1.0f - std::exp (-1.0f / (0.100f * (float) currentRate));
        for (int i = 0; i < numSamples; ++i)
        {
            const float peak = juce::jmax (std::abs (outL[i]),
                                           outR != nullptr ? std::abs (outR[i]) : 0.0f);
            const float needed = peak > ceiling ? ceiling / peak : 1.0f;
            busCeiling = needed < busCeiling ? needed
                                             : busCeiling + recover * (needed - busCeiling);

            outL[i] *= busCeiling;
            if (outR != nullptr)
                outR[i] *= busCeiling;
        }
    }

    float KitEngine::getLaneActivity (int lane) const
    {
        return (lane >= 0 && lane < NumLanes) ? lanes[(std::size_t) lane].activity.load() : 0.0f;
    }

    void KitEngine::startVoice (const std::shared_ptr<Sample>& sample, int lane, int articulation,
                                int mic, float gainL, float gainR, double increment, float envDecay,
                                float toneCoeff)
    {
        Voice* target = nullptr;
        for (auto& v : voices)
            if (! v.active) { target = &v; break; }

        if (target == nullptr)
        {
            // Steal the least audible voice, so a ring that is still loud keeps
            // its tail and a stroke that has almost died out goes instead.
            float quietest = std::numeric_limits<float>::max();
            for (auto& v : voices)
            {
                const float audible = v.env * juce::jmax (v.gainL, v.gainR);
                if (audible < quietest) { quietest = audible; target = &v; }
            }
        }

        if (target == nullptr)
            return;

        target->sample       = sample;
        target->position     = 0.0;
        target->increment    = increment;
        target->gainL        = gainL;
        target->gainR        = gainR;
        target->env          = 1.0f;
        target->envDecay     = envDecay;
        target->release      = 1.0f;
        target->toneCoeff    = toneCoeff;
        // The tail is darker than the strike: by the last frame of the recording
        // the voice is hearing the shell ring rather than the stick, which is
        // what stops a long decay reading as a sample being turned down.
        target->tailCoeff    = toneCoeff * 0.45f;
        target->invFrames    = sample != nullptr && sample->numFrames > 1
                             ? 1.0f / (float) sample->numFrames : 0.0f;
        target->toneL        = 0.0f;
        target->toneR        = 0.0f;
        target->lane         = lane;
        target->articulation = articulation;
        target->mic          = mic;
        target->active       = true;
    }

    void KitEngine::noteOn (int lane, float velocity01, int variant)
    {
        // A kit swap rebuilds the lanes, so a stroke that lands mid-swap is
        // dropped: the audio thread never waits for a disk load.
        const juce::ScopedTryLock layers (layerLock);
        if (! layers.isLocked())
            return;

        const int articulation = lane;
        lane = resolveLane (lane);
        if (lane < 0)
            return;

        auto& slot = lanes[(std::size_t) lane];
        const int numLayers = (int) slot.layers.size();
        if (numLayers == 0)
            return;

        const float vel = juce::jlimit (0.02f, 1.0f, velocity01);
        const int rr = slot.roundRobin.fetch_add (1) + variant;
        const bool stickLane = isSnareLane (lane) || isTomLane (lane);

        // A hash of the round-robin counter, used to dither the choices below
        // so that they stay deterministic without ever needing rand().
        const auto dither = [rr] (int salt)
        {
            const auto h = (juce::uint32) rr * 2654435761u + (juce::uint32) salt * 40503u;
            return (float) ((h >> 9) & 0xffffu) / 65535.0f;
        };

        // Velocity picks a point on the layer ladder rather than a bucket, and
        // the boundary is dithered: a stroke sitting between two layers takes
        // one or the other. Hard buckets are what made repeated hats step.
        const float ladder = vel * (float) numLayers - 0.5f;
        const int   below  = (int) std::floor (ladder);
        int layerIndex = below + (dither (1) < (ladder - (float) below) ? 1 : 0);
        layerIndex = juce::jlimit (0, numLayers - 1, layerIndex);

        const auto& layer = slot.layers[(std::size_t) layerIndex];
        const int numVariants = (int) layer.variants.size();
        if (numVariants == 0)
            return;

        // The round-robin slot rotates within the layer when the kit supplies
        // several takes of it; otherwise it nudges to a neighbouring layer so
        // repeated strokes still differ.
        //
        // On the drums played with sticks the slot is taken from the
        // performance alone, not from the free-running counter, because the
        // performance is alternating hands there: keeping its parity is what
        // makes consecutive strokes land on two different takes of the drum
        // instead of wherever the counter happens to be.
        const int rrBase = stickLane ? variant : rr;
        int variantIndex = ((rrBase % numVariants) + numVariants) % numVariants;
        variantIndex = (variantIndex + slot.sampleSwitch.load()) % numVariants;
        if (variantIndex < 0)
            variantIndex += numVariants;

        if (numVariants == 1 && numLayers > 1 && (variant & 1) != 0)
            layerIndex = juce::jlimit (0, numLayers - 1, layerIndex + ((variant & 2) ? 1 : -1));

        const auto& chosen = slot.layers[(std::size_t) layerIndex]
                                 .variants[(std::size_t) juce::jlimit (
                                     0, (int) slot.layers[(std::size_t) layerIndex].variants.size() - 1,
                                     variantIndex)];
        if (chosen.mics[MicClose] == nullptr)
            return;

        // A hat or a ride is struck dozens of times a bar, so it is the lane
        // where an unvarying sample reads as plastic first: it gets a wider
        // spread of tuning and level per stroke than a shell does.
        const bool ting = isHatLane (lane) || isRideLane (lane);

        // Micro-variation: a few cents and a fraction of a dB per hit keeps
        // repeated notes from phase-cancelling into an obvious loop.
        const auto& kv = kitVoicing[(std::size_t) lane];
        const float cents = (dither (2) - 0.5f) * (ting ? 34.0f : 22.0f)
                          + (slot.tune.load() + kv.tune.load()) * 100.0f;
        const float trim  = juce::Decibels::decibelsToGain (
                                (dither (3) - 0.5f) * (ting ? 2.2f : 1.3f));

        // How much of the dynamic range the recordings already carry. On a deep
        // kit the layers do the work and the fader only tops it up; on a
        // one-layer kit the fader is all there is.
        const float depth   = juce::jmin (1.0f, (float) numLayers / 5.0f);
        // Cymbals carry more of their dynamics in level than the shells do, so
        // a light stroke really is a light stroke instead of the same ting at a
        // slightly lower fader - and they sit a little behind the kit, where a
        // room mic would put them.
        const float curve   = std::pow (vel, ting ? 1.15f : 0.75f);
        const float velGain = juce::jmap (depth, curve, 0.45f + 0.55f * curve);

        // A ride is played through a whole section rather than struck for
        // effect, so it sits further back still than the hats: at the level a
        // close mic gives it, it reads as the loudest thing in the kit.
        // A shut hat is the quietest thing a drummer plays: it keeps time under
        // the kit and leaves the open hat to be the contrast you hear.
        // An open hat is the opposite: it is the stroke the part is heard on,
        // so it speaks at the level of the kit rather than being trimmed back
        // with the time-keeping cymbals.
        const bool  shutHat = lane == LaneHatClosed || lane == LaneHatTight;
        const bool  openHat = lane >= LaneHatOpen1 && lane <= LaneHatOpen4;
        const float tingTrimDb = isRideLane (lane) ? -5.5f
                               : shutHat           ? -3.0f
                               : openHat           ? -0.5f
                               : (ting ? -3.5f : 0.0f);
        const float gain = velGain * trim
                         * juce::Decibels::decibelsToGain (slot.gainDb.load()
                                                           + kv.gainDb.load()
                                                           + kitTrimDb.load()
                                                           + tingTrimDb);
        // Sticking: the performance alternates hands on the drums it plays with
        // sticks, odd round-robin slots being the off hand. The two hands do not
        // strike from the same place, so they do not sit in quite the same spot
        // in the image either - which is what makes a roll travel.
        const float handPan = stickLane ? ((variant & 1) != 0 ? -0.05f : 0.05f)
                                        : 0.0f;
        const float pan  = juce::jlimit (-1.0f, 1.0f,
                                         slot.pan.load() + handPan
                                         + (dither (4) - 0.5f) * 0.05f);
        const float gl   = gain * std::sqrt (0.5f * (1.0f - pan));
        const float gr   = gain * std::sqrt (0.5f * (1.0f + pan));

        // Damping shortens the decay, the way a drummer's tape or a felt does.
        const float damp = juce::jlimit (0.0f, 1.0f, slot.damp.load() + kv.damp.load());
        const float decayTime = juce::jmap (damp, 0.0f, 1.0f, 8.0f, 0.09f);
        const float envDecay = damp <= 0.001f ? 1.0f
                             : (float) std::pow (0.001, 1.0 / (decayTime * currentRate));

        // Stick hardness: a soft stroke is darker as well as quieter, because a
        // lightly struck cymbal never excites its top octave. Without this a
        // ladder of level-matched multisamples reads as one flat, boxy hit.
        const float reach = juce::jmap (depth, 0.85f, 0.4f);
        // A shut hat recorded on its own close mic is nearly all top octave:
        // three quarters of its energy sits above 6 kHz and it has no tail to
        // speak of, which is exactly what reads as a click rather than a hat.
        // Rolling that octave off leaves the stick and the body of the cymbal.
        const float dark  = (1.0f - std::pow (vel, 0.8f)) * reach
                          + (ting ? 0.16f : 0.0f)     // takes the wire off the top
                          + (shutHat ? 0.24f : 0.0f); // and the click off a shut hat
        const float cutoff = 20000.0f * std::exp (-3.1f * dark);
        const float tone = dark < 0.02f
                         ? 1.0f
                         : juce::jlimit (0.02f, 1.0f,
                                         1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi
                                                          * cutoff / (float) currentRate));

        const juce::ScopedLock sl (voiceLock);
        chokeArticulations (articulation);

        // Where the stroke is heard from. A shut hat has almost no ring of its
        // own, so what makes it sound like a cymbal in a room instead of a
        // sampled tick is the overheads and the room, not more of the hat mic.
        const std::array<float, NumMics> micWeight = shutHat
            ? std::array<float, NumMics> { 0.90f, 2.60f, 2.20f }
            : std::array<float, NumMics> { 1.00f, 1.00f, 1.00f };

        for (int mic = 0; mic < NumMics; ++mic)
        {
            const auto& sample = chosen.mics[(std::size_t) mic];
            if (sample == nullptr)
                continue;
            const double increment = (sample->sourceRate / currentRate)
                                   * std::pow (2.0, cents / 1200.0);
            const float w = micWeight[(std::size_t) mic];
            startVoice (sample, lane, articulation, mic, gl * w, gr * w, increment,
                        envDecay, tone);
        }

        slot.activity.store (1.0f);
    }

    void KitEngine::chokeArticulations (int articulation)
    {
        // Hat pedal state machine: closing the hats kills anything ringing on
        // the openness ladder. Cymbal chokes stop their own cymbal.
        // A real hat closing damps the cymbal over a few milliseconds; killing
        // the voice outright is what made this sound like a noise gate.
        const auto killIf = [this] (float seconds, auto&& predicate)
        {
            const float choke = releaseCoefficient (seconds);
            for (auto& v : voices)
                if (v.active && predicate (v.articulation))
                    v.release = juce::jmin (v.release, choke);
        };

        if (articulation == LaneHatClosed || articulation == LaneHatTight
            || articulation == LaneHatPedal)
        {
            killIf (0.085f, [] (int a) { return a >= LaneHatOpen1 && a <= LaneHatOpen4; });
        }
        else if (articulation >= LaneHatOpen1 && articulation <= LaneHatOpen4)
        {
            // A wider hat replaces a narrower one rather than stacking, but the
            // foot has not come down: the cymbal it replaces washes out under
            // the new stroke instead of being shut off.
            killIf (0.16f, [articulation] (int a)
                    { return a >= LaneHatOpen1 && a <= LaneHatOpen4 && a != articulation; });
        }
        else if (articulation == LaneChina || articulation == LaneSplash)
        {
            killIf (0.085f, [articulation] (int a) { return a == articulation; });
        }
    }

    void KitEngine::allNotesOff()
    {
        const juce::ScopedLock sl (voiceLock);
        for (auto& v : voices)
        {
            v.active = false;
            v.sample.reset();
        }
    }

    void KitEngine::renderNextBlock (juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
    {
        const juce::ScopedLock sl (voiceLock);

        const int numOut = buffer.getNumChannels();
        auto* outL = buffer.getWritePointer (0, startSample);
        auto* outR = numOut > 1 ? buffer.getWritePointer (1, startSample) : nullptr;

        const float blend     = micBlend.load();
        const float bleedAmt  = bleed.load();
        const float crushAmt  = crush.load();

        // Mic blend: close dominates until the blend is turned up, and the far
        // mics carry the bleed.
        std::array<float, NumMics> micGain { 1.0f - 0.45f * blend,
                                             blend * (0.4f + 0.6f * bleedAmt),
                                             blend * blend * (0.3f + 0.7f * bleedAmt) };

        // Each lane is summed on its own bus so its compressor and reverb send
        // only ever see that instrument, then folded into the output.
        const float mix = roomMix.load();
        const bool  useRoom = mix > 0.001f && roomSpace.load() != SpaceDry
                           && reverbBus.getNumSamples() >= numSamples;
        const bool  perLane = laneBus.getNumSamples() >= numSamples;

        if (useRoom)
            reverbBus.clear (0, numSamples);

        for (int lane = 0; lane < NumLanes; ++lane)
        {
            auto& slot = lanes[(std::size_t) lane];

            bool anyVoice = false;
            for (const auto& v : voices)
                if (v.active && v.lane == lane) { anyVoice = true; break; }

            if (! anyVoice)
            {
                slot.compEnv *= 0.5f;
                continue;
            }

            float* busL = outL;
            float* busR = outR;
            if (perLane)
            {
                laneBus.clear (0, numSamples);
                busL = laneBus.getWritePointer (0);
                busR = laneBus.getWritePointer (1);
            }

            for (auto& v : voices)
            {
                if (! v.active || v.sample == nullptr || v.lane != lane)
                    continue;

                const auto* src  = v.sample->data.data();
                const int   len  = v.sample->numFrames;
                const int   ch   = v.sample->numChannels;
                const float mg   = micGain[(std::size_t) v.mic];
                constexpr float toFloat = 1.0f / 32768.0f;

                // A recording that still has level at its last frame would stop
                // dead there and click, so the very end is ramped out - but only
                // over the last few milliseconds, short enough that the drum is
                // heard ringing to the end of its own recording rather than being
                // faded off it.
                const int fadeFrom = len - (int) (0.004 * v.sample->sourceRate);

                // The stick is the first few milliseconds of the recording, and
                // it is left alone: darkening a stroke from its very first frame
                // takes the attack off the drum, which is what makes a sampled
                // kit sound soft next to a real one. The tone only closes in
                // once the strike has spoken.
                const int strikeTo = (int) (0.0035 * v.sample->sourceRate);
                const int bodyFrom = strikeTo + (int) (0.012 * v.sample->sourceRate);

                for (int i = 0; i < numSamples; ++i)
                {
                    const int pos = (int) v.position;
                    if (pos + 1 >= len)
                    {
                        v.active = false;
                        v.sample.reset();
                        break;
                    }

                    if (pos > fadeFrom)
                        v.env *= 1.0f - 1.0f / (float) juce::jmax (1, len - pos);

                    const auto* a = src + (std::size_t) pos * (std::size_t) ch;
                    const auto* b = a + ch;
                    const float frac = (float) (v.position - (double) pos);
                    float l = (a[0] + frac * (b[0] - a[0])) * toFloat;
                    float r = ch > 1 ? (a[1] + frac * (b[1] - a[1])) * toFloat : l;

                    // Darken as the stroke rings down, over its own length.
                    const float age    = (float) pos * v.invFrames;
                    float coeff        = v.toneCoeff
                                       + (v.tailCoeff - v.toneCoeff) * age;

                    if (pos < bodyFrom && bodyFrom > strikeTo)
                    {
                        const float open = pos <= strikeTo
                                         ? 1.0f
                                         : 1.0f - (float) (pos - strikeTo)
                                                  / (float) (bodyFrom - strikeTo);
                        coeff += (1.0f - coeff) * open;
                    }

                    v.toneL += coeff * (l - v.toneL);
                    v.toneR += coeff * (r - v.toneR);
                    l = v.toneL;
                    r = v.toneR;

                    busL[i] += l * v.gainL * v.env * mg;
                    if (busR != nullptr)
                        busR[i] += r * v.gainR * v.env * mg;

                    v.position += v.increment;
                    v.env *= v.envDecay * v.release;
                    if (v.env < 1.0e-4f)
                    {
                        v.active = false;
                        v.sample.reset();
                        break;
                    }
                }
            }

            if (! perLane)
                continue;

            // The piece is engineered before it is levelled, so its compressor
            // sees a mix-ready instrument rather than a raw close mic.
            shapeLane (slot, lane, busL, busR, numSamples);

            const float comp = slot.compression.load();
            if (comp > 0.001f)
            {
                // One peak compressor per lane: fast attack, programme-dependent
                // release, with make-up so turning it up sounds like control
                // rather than a volume drop.
                const float threshold = juce::Decibels::decibelsToGain (-6.0f - 18.0f * comp);
                const float ratio     = 1.5f + 6.0f * comp;
                const float attack    = 1.0f - std::exp (-1.0f / (float) (0.002 * currentRate));
                const float release   = 1.0f - std::exp (-1.0f / (float) (0.120 * currentRate));
                const float makeUp    = juce::Decibels::decibelsToGain (5.0f * comp);

                for (int i = 0; i < numSamples; ++i)
                {
                    const float peak = juce::jmax (std::abs (busL[i]),
                                                   busR != nullptr ? std::abs (busR[i]) : 0.0f);
                    const float coeff = peak > slot.compEnv ? attack : release;
                    slot.compEnv += coeff * (peak - slot.compEnv);

                    float gain = makeUp;
                    if (slot.compEnv > threshold)
                    {
                        const float over = slot.compEnv / threshold;
                        gain *= std::pow (over, 1.0f / ratio - 1.0f);
                    }

                    busL[i] *= gain;
                    if (busR != nullptr)
                        busR[i] *= gain;
                }
            }

            const float send = useRoom ? slot.reverbSend.load() : 0.0f;
            auto* verbL = useRoom ? reverbBus.getWritePointer (0) : nullptr;
            auto* verbR = useRoom ? reverbBus.getWritePointer (1) : nullptr;

            for (int i = 0; i < numSamples; ++i)
            {
                const float l = busL[i];
                const float r = busR != nullptr ? busR[i] : l;
                outL[i] += l;
                if (outR != nullptr)
                    outR[i] += r;

                if (send > 0.001f)
                {
                    verbL[i] += l * send;
                    verbR[i] += r * send;
                }
            }
        }

        if (useRoom)
        {
            // Each space has its own size, darkness, pre-delay and how bright
            // its return is allowed to be; the Size and Damping knobs then trim
            // around whichever one is chosen.
            struct Space { float size, damping, preDelayMs, topHz; };
            static constexpr Space spaces[NumRoomSpaces] {
                { 0.20f, 0.70f,  4.0f, 3800.0f },   // Dry: the booth itself
                { 0.32f, 0.62f,  9.0f, 4600.0f },   // Studio
                { 0.50f, 0.48f, 21.0f, 5200.0f },   // Room
                { 0.82f, 0.26f, 34.0f, 4200.0f },   // Hall
                { 0.62f, 0.14f,  5.0f, 7400.0f },   // Plate: bright, no pre-delay
            };
            const auto& space = spaces[(std::size_t) juce::jlimit (0, (int) NumRoomSpaces - 1,
                                                                   roomSpace.load())];

            juce::Reverb::Parameters params;
            params.roomSize  = juce::jlimit (0.05f, 0.98f,
                                             space.size + (roomSize.load() - 0.45f) * 0.6f);
            params.damping   = juce::jlimit (0.0f, 1.0f,
                                             space.damping + (roomDamping.load() - 0.5f) * 0.6f);
            params.width     = 1.0f;
            params.wetLevel  = 1.0f;
            params.dryLevel  = 0.0f;
            params.freezeMode = 0.0f;
            room.setParameters (params);

            // Pre-delay: the strike reaches the close mic before it reaches the
            // walls. Without this the return arrives with the attack and reads
            // as an effect on the drum rather than the space around it.
            const int preDelay = juce::jlimit (1, kRoomPreDelay,
                                               (int) (space.preDelayMs * 0.001f
                                                      * (float) currentRate));
            {
                auto* sendL = reverbBus.getWritePointer (0);
                auto* sendR = reverbBus.getWritePointer (1);
                for (int i = 0; i < numSamples; ++i)
                {
                    const int slot = roomDelayWrite * 2;
                    const float dl = roomDelayLine[(std::size_t) slot];
                    const float dr = roomDelayLine[(std::size_t) slot + 1];
                    roomDelayLine[(std::size_t) slot]     = sendL[i];
                    roomDelayLine[(std::size_t) slot + 1] = sendR[i];
                    roomDelayWrite = (roomDelayWrite + 1) % preDelay;
                    sendL[i] = dl;
                    sendR[i] = dr;
                }
            }

            room.processStereo (reverbBus.getWritePointer (0), reverbBus.getWritePointer (1),
                                numSamples);

            // And the return is darker than the kit: a room gives back body, not
            // the top end a close mic hears.
            const float toneCoeff = 1.0f - std::exp (-2.0f * juce::MathConstants<float>::pi
                                                     * space.topHz / (float) currentRate);

            // Ducking: the room is held down while the kit is being struck and
            // opens up in the gaps, which is what keeps a long decay from
            // sitting on top of the next stroke. Fast to close, slow to open,
            // the way a drummer hears the room answer.
            const float duck    = roomDuck.load();
            const float attack  = 1.0f - std::exp (-1.0f / (0.004f * (float) currentRate));
            const float release = 1.0f - std::exp (-1.0f / (0.18f  * (float) currentRate));

            const auto* wetL = reverbBus.getReadPointer (0);
            const auto* wetR = reverbBus.getReadPointer (1);
            for (int i = 0; i < numSamples; ++i)
            {
                roomToneL += toneCoeff * (wetL[i] - roomToneL);
                roomToneR += toneCoeff * (wetR[i] - roomToneR);

                const float dry = std::max (std::abs (outL[i]),
                                            outR != nullptr ? std::abs (outR[i]) : 0.0f);
                roomDuckEnv += (dry > roomDuckEnv ? attack : release) * (dry - roomDuckEnv);

                const float open = 1.0f / (1.0f + duck * 6.0f * roomDuckEnv);
                outL[i] += roomToneL * mix * open;
                if (outR != nullptr)
                    outR[i] += roomToneR * mix * open;
            }
        }

        // A close-mic-only kit has no recorded leak, so the bleed control feeds
        // a delayed mono copy of the kit into the room instead.
        const bool haveFarMics = laneHasMic (LaneKick, MicRoom) || laneHasMic (LaneKick, MicOverhead);
        if (bleedAmt > 0.001f && ! haveFarMics)
        {
            const float send = 0.35f * bleedAmt * (0.4f + 0.6f * blend);
            for (int i = 0; i < numSamples; ++i)
            {
                const float mono = 0.5f * (outL[i] + (outR != nullptr ? outR[i] : outL[i]));
                const float delayed = bleedLine[(std::size_t) bleedWrite];
                bleedLine[(std::size_t) bleedWrite] = mono;
                bleedWrite = (bleedWrite + 1) % kBleedDelay;

                outL[i] += delayed * send;
                if (outR != nullptr)
                    outR[i] += delayed * send * 0.85f;
            }
        }

        if (crushAmt > 0.001f)
        {
            const float crushDrive = 1.0f + 6.0f * crushAmt;
            for (int i = 0; i < numSamples; ++i)
            {
                const float mono = 0.5f * (outL[i] + (outR != nullptr ? outR[i] : outL[i]));
                const float crushed = std::tanh (mono * crushDrive) * 0.35f;
                outL[i] = outL[i] * (1.0f - 0.5f * crushAmt) + crushed * crushAmt;
                if (outR != nullptr)
                    outR[i] = outR[i] * (1.0f - 0.5f * crushAmt) + crushed * crushAmt;
            }
        }

        processKitBus (outL, outR, numSamples);

        const float decay = std::pow (0.5f, (float) numSamples / (float) (currentRate * 0.12));
        for (auto& l : lanes)
            l.activity.store (l.activity.load() * decay);
    }
}
