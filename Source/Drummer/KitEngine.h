#pragma once

#include "GrooveCorpus.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace hhx
{
    /** The microphone positions a kit can supply per stroke. Close is the only
        one a kit must have; the others are mixed in behind it and double as the
        kit's natural bleed.
    */
    enum Mic : int
    {
        MicClose = 0,
        MicOverhead,
        MicRoom,
        NumMics
    };

    /** Multisampled kit player.

        Each lane holds velocity layers, softest first, and each layer holds
        round-robin variants; each variant holds one recording per microphone.
        A hit picks the layer its velocity falls into and a variant the
        performance engine chose, so consecutive strokes are never
        bit-identical - the behaviour whose absence made the previous plugin's
        hats sound like a machine gun.
    */
    class KitEngine
    {
    public:
        KitEngine();

        void prepare (double sampleRate, int maxBlockSize);

        /** Loads the kit compiled into the binary. */
        int loadBundledKit (const juce::String& kitPrefix);

        /** Loads a kit folder. A `kit.json` describing pieces, velocity layers,
            round robins and mics is used when present; otherwise the WAV names
            (`<piece>_<n>.wav`) are parsed.

            Safe to call while audio is running: strokes are skipped rather
            than played from a half-loaded kit, and the voices still ringing
            hold their own samples until they finish. */
        int loadKitFolder (const juce::File& folder);

        juce::String getKitName() const;
        juce::String getKitVersion() const;

        /** Total loaded samples across every lane, layer, round robin and mic. */
        int numLoadedSamples() const;

        int numLayersForLane (int lane) const;
        int numVariantsForLane (int lane, int layer) const;
        bool laneHasMic (int lane, int mic) const;

        /** Sample-switch: rotates which variant of the lane's set is favoured.
            Persisted with the project; takes effect on the next hit. */
        void setLaneSampleSwitch (int lane, int offset);
        int  getLaneSampleSwitch (int lane) const;

        void setLaneGainDb (int lane, float db);
        float getLaneGainDb (int lane) const;
        void setLanePan (int lane, float pan);
        float getLanePan (int lane) const;
        void setLaneTune (int lane, float semitones);
        float getLaneTune (int lane) const;
        void setLaneDamp (int lane, float amount01);
        float getLaneDamp (int lane) const;

        // --- per-instrument channel strip --------------------------------
        /** Compression on the lane's own bus: 0 = off, 1 = squashed. One
            feed-forward peak compressor per lane, so a snare can be levelled
            without touching the cymbals. */
        void  setLaneCompression (int lane, float amount01);
        float getLaneCompression (int lane) const;

        /** How much of the lane goes to the shared room reverb. */
        void  setLaneReverbSend (int lane, float amount01);
        float getLaneReverbSend (int lane) const;

        /** The shared plate/room the sends feed. `mix` is how much of it comes
            back into the output. */
        void  setRoom (float size01, float damping01, float mix01);
        float getRoomSize() const;
        float getRoomDamping() const;
        float getRoomMix() const;

        /** The spaces the kit can be heard in. Each one sets the room's own
            character - its size, how dark it is, and how long the sound takes
            to reach the walls - so the choice is a place rather than three
            numbers to guess at. */
        enum RoomSpace { SpaceDry = 0, SpaceStudio, SpaceRoom, SpaceHall, SpacePlate, NumRoomSpaces };
        void setRoomSpace (int space);
        int  getRoomSpace() const;

        /** How far the room is pushed out of the way while the kit is being
            struck, so the space is heard between the hits instead of over
            them. */
        void  setRoomDuck (float amount01);
        float getRoomDuck() const;

        // --- kit mix -----------------------------------------------------
        /** Mic blend: 0 = close only, 1 = all the way back in the room. */
        void  setMicBlend (float blend01);
        float getMicBlend() const;

        /** How much of the kit leaks into the far mics. With a close-mic-only
            kit this generates the leak from a delayed mono copy instead. */
        void  setBleed (float amount01);
        float getBleed() const;

        /** Mono-crush bus: a saturated mono squash of the whole kit, blended
            back in behind the stereo mix. */
        void  setCrush (float amount01);
        float getCrush() const;

        // --- produced mix ------------------------------------------------
        /** How the kit is engineered rather than how it is played. A raw close
            mic never sits in a mix on its own: it is high-passed to make room
            for the bass, shaped so the stick is heard over a dense arrangement,
            glued on a bus and driven a little. Each voicing is one of those
            treatments, the way a mixed kit arrives already sitting. */
        enum MixVoicing { MixRaw = 0, MixModern, MixPunch, MixRoom, MixVintage, NumMixVoicings };
        void setMixVoicing (int voicing);
        int  getMixVoicing() const;

        /** Transient design on each piece: how far the stick is pushed in front
            of the shell. Above 0.5 the attack is emphasised, below it the
            sustain is. */
        void  setPunch (float amount01);
        float getPunch() const;

        /** Bus compression across the whole kit: the thing that makes a kit
            read as one instrument instead of separate samples. */
        void  setGlue (float amount01);
        float getGlue() const;

        /** Saturation on the kit bus, which is where the last few dB of
            loudness come from without the peaks growing. */
        void  setDrive (float amount01);
        float getDrive() const;

        /** Industry Squeeze: spectral compression across four bands of the kit
            bus. Each band is levelled against the band's own target, so the
            boxy 200-500 Hz region and a harsh stick are pulled in while a shy
            band is lifted, then a glow stage adds harmonics. Unlike a single
            bus compressor this cannot let one loud band duck the whole kit. */
        enum SqueezeGlow { GlowOff = 0, GlowClean, GlowTube, GlowTape, GlowTransformer,
                           NumSqueezeGlows };
        void  setSqueeze (float amount01);
        float getSqueeze() const;
        void  setSqueezeGlow (int glow);
        int   getSqueezeGlow() const;

        /** `variant` is the round-robin slot the performance engine chose, so
            two consecutive strokes on a lane never fire the same sample. */
        void noteOn (int lane, float velocity01, int variant = 0);
        void allNotesOff();

        /** Which lane actually holds samples for an articulation. A 30-piece
            performance still plays on a kit that only ships one snare: the
            articulation falls back to its nearest relative rather than going
            silent. */
        int resolveLane (int lane) const;

        void renderNextBlock (juce::AudioBuffer<float>& buffer, int startSample, int numSamples);

        /** 0..1 decaying meter per lane, for the UI's animated kit pieces. */
        float getLaneActivity (int lane) const;

    private:
        /** One recording, kept as interleaved 16-bit so a kit with hundreds of
            multisamples costs half the memory of a float copy. */
        struct Sample
        {
            std::vector<std::int16_t> data;
            int    numChannels = 1;
            int    numFrames   = 0;
            double sourceRate  = 44100.0;
            float  peak        = 0.0f;
        };

        struct Variant
        {
            std::array<std::shared_ptr<Sample>, NumMics> mics {};
        };

        struct Layer
        {
            std::vector<Variant> variants;
        };

        struct LaneSlot
        {
            std::vector<Layer> layers;               // softest first
            std::atomic<int>   sampleSwitch { 0 };
            std::atomic<float> gainDb { 0.0f };
            std::atomic<float> pan    { 0.0f };
            std::atomic<float> tune   { 0.0f };   // semitones
            std::atomic<float> damp   { 0.0f };   // 0 = open, 1 = heavily muted
            std::atomic<float> compression { 0.0f };
            std::atomic<float> reverbSend  { 0.0f };
            std::atomic<float> activity { 0.0f };
            std::atomic<int>   roundRobin { 0 };
            float              compEnv { 0.0f };   // audio thread only

            // Mix state, audio thread only.
            float hpState[2] { 0.0f, 0.0f };   // high-pass integrator per side
            float fastEnv { 0.0f };            // transient design: stick
            float slowEnv { 0.0f };            //                   shell
        };

        /** What a piece needs before it sits in a mix: where its low end stops
            being useful, and how much of its top belongs to the stick. */
        struct LaneVoicing
        {
            float highPassHz  = 0.0f;
            float attack      = 0.0f;   // how much transient design it takes
            float sustainTrim = 0.0f;   // dB taken off its ring
        };

        static LaneVoicing voicingForLane (int lane, int voicing);

        /** The lane's own shaping: high-pass, then transient design. Runs on the
            lane bus so a snare can be pushed forward without the cymbals
            following it. */
        void shapeLane (LaneSlot& slot, int lane, float* busL, float* busR, int numSamples);

        /** The kit bus: glue compression, saturation, the voicing's tilt, and a
            ceiling so a hard-hit chorus cannot clip the host. */
        void processKitBus (float* outL, float* outR, int numSamples);

        /** Four-band spectral compression plus the glow stage. */
        void processSqueeze (float* outL, float* outR, int numSamples);

        struct Voice
        {
            std::shared_ptr<Sample> sample;
            double position = 0.0;
            double increment = 1.0;
            float  gainL = 0.0f;
            float  gainR = 0.0f;
            float  env   = 1.0f;
            float  envDecay = 1.0f;
            float  release  = 1.0f;   // < 1 once the voice is being let go
            float  toneCoeff = 1.0f;   // one-pole low-pass, 1 = wide open
            /** Where that low-pass ends up by the end of the recording. A drum
                loses its top end as it rings down - the stick is heard at the
                attack and the shell at the tail - so the voice darkens as it
                plays. A sample held at one brightness and faded in level is
                exactly what reads as plastic. */
            float  tailCoeff = 1.0f;
            float  invFrames = 0.0f;   // 1 / numFrames, for the tail above
            float  toneL = 0.0f;
            float  toneR = 0.0f;
            int    lane  = -1;
            int    articulation = -1;
            int    mic   = MicClose;
            bool   active = false;
        };

        /** Where a loaded file belongs in the kit. */
        struct Placement
        {
            int lane  = -1;
            int layer = -1;      // -1 = decide from loudness after loading
            int variant = 0;
            int mic   = MicClose;
        };

        void clearKit();
        /** Lets ringing voices go over a few milliseconds instead of cutting them
            dead, which is what made chokes and stolen voices sound gated. */
        void chokeArticulations (int articulation);
        float releaseCoefficient (float seconds) const;
        void place (const Placement& p, std::shared_ptr<Sample> s);

        /** Drops empty layers and variants. Kits that declare their velocity
            layers keep the order they declared; kits that do not (loose files)
            are ordered by how loud they are. */
        void finaliseKit (bool sortByLoudness);

        /** How much of a lane's loudest recording is ring rather than stroke:
            the energy left after the attack against the attack's own. */
        float laneRingRatio (int lane) const;

        /** Some libraries record the tight/edge hat as a bare stick tick with
            the cymbal damped: no ring at all, which plays back as a click
            rather than a hat. Such a lane is dropped so the ringing shut hat
            covers it through the usual fallback chain. */
        void dropRinglessShutHat();
        std::shared_ptr<Sample> readSample (juce::InputStream* stream);
        int loadFromManifest (const juce::File& folder, const juce::var& manifest);
        void startVoice (const std::shared_ptr<Sample>& sample, int lane, int articulation,
                         int mic, float gainL, float gainR, double increment,
                         float envDecay, float toneCoeff);

        static int pieceNameToLane (const juce::String& piece);
        static int micNameToIndex (const juce::String& mic);
        static Placement placementFromFilename (const juce::String& name);

        /** Three mics per stroke and cymbals that ring for seconds mean a busy
            bar needs far more voices than it has notes; too few and the engine
            steals tails mid-ring, which reads as a one-shot cutting off. */
        static constexpr int kMaxVoices  = 384;
        static constexpr int kBleedDelay = 512;   // samples, ~11 ms at 48 kHz

        std::array<LaneSlot, NumLanes> lanes;
        std::array<Voice, kMaxVoices>  voices;
        juce::CriticalSection          voiceLock;
        /** Held while a kit is being swapped in, so a stroke arriving mid-load
            is dropped instead of reading a lane that is being rebuilt. */
        mutable juce::CriticalSection  layerLock;
        juce::AudioFormatManager       formats;
        double                         currentRate = 48000.0;

        /** Per-kit level match from the kit's own manifest, so swapping kits
            does not change how loud the instrument sits in a mix. */
        std::atomic<float> kitTrimDb { 0.0f };

        /** How the kit itself is tuned and damped, from its manifest, applied
            on top of the user's own lane controls. A sludge kit is the same
            shells dropped a tone and taped up, so a voicing is what makes it a
            different kit rather than a second copy of the recordings. */
        struct KitVoicing
        {
            std::atomic<float> tune { 0.0f };     // semitones
            std::atomic<float> damp { 0.0f };     // added to the lane's damp
            std::atomic<float> gainDb { 0.0f };
        };
        std::array<KitVoicing, NumLanes> kitVoicing {};
        void clearKitVoicing();

        std::atomic<float> micBlend { 0.35f };
        std::atomic<float> bleed    { 0.15f };
        std::atomic<float> crush    { 0.0f };

        std::atomic<float> roomSize    { 0.45f };
        std::atomic<float> roomDamping { 0.5f };
        std::atomic<float> roomMix     { 0.22f };

        juce::Reverb              room;
        juce::AudioBuffer<float>  laneBus;      // one lane at a time
        juce::AudioBuffer<float>  reverbBus;

        /** The room is the room the kit was played in, not an effect: the send
            is held back by a few milliseconds so the strike is heard dry first,
            and the return is rolled off, because a room does not give back the
            top end a close mic hears. */
        static constexpr int kRoomPreDelay = 2048;   // longest pre-delay, ~43 ms at 48 kHz
        std::array<float, kRoomPreDelay * 2> roomDelayLine {};
        int   roomDelayWrite = 0;
        float roomToneL = 0.0f;
        float roomToneR = 0.0f;
        float roomDuckEnv = 0.0f;

        std::atomic<int>   roomSpace { SpaceRoom };
        std::atomic<float> roomDuck  { 0.35f };

        std::atomic<int>   mixVoicing { MixModern };
        std::atomic<float> punch      { 0.5f };
        std::atomic<float> glue       { 0.35f };
        std::atomic<float> drive      { 0.2f };

        std::atomic<float> squeeze     { 0.0f };
        std::atomic<int>   squeezeGlow { GlowClean };

        // Industry Squeeze state, audio thread only. Three complementary
        // one-pole splits give four bands that sum back to the input, so at
        // unity gain the stage is transparent.
        static constexpr int kSqueezeBands = 4;
        float squeezeLp[3][2] {};
        float squeezeEnv[kSqueezeBands] {};
        float squeezeGain[kSqueezeBands] { 1.0f, 1.0f, 1.0f, 1.0f };

        // Kit bus state, audio thread only.
        float busCompEnv = 0.0f;
        float busTiltL   = 0.0f;
        float busTiltR   = 0.0f;
        float busCeiling = 1.0f;

        std::array<float, kBleedDelay> bleedLine {};
        int                            bleedWrite = 0;

        juce::String                   kitName;
        juce::String                   kitVersion { "1" };
        mutable juce::CriticalSection  kitNameLock;
    };
}
