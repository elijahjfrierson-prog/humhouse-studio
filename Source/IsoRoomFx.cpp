// HumHouse Iso Room as a built-in plug-in: the zero-latency vocal isolation engine from the hum-house-isoroom app
// (band-wise room suppression + voice-keyed gate), same controls as the VST3
#include "Effects.h"
#include "IsoRoom/IsoRoomEngine.h"

namespace hhs
{
class IsoRoomFx : public BuiltinEffect
{
public:
    IsoRoomFx() { initDefaults(); }
    const char* typeId() const override { return "isoroom"; }
    const std::vector<ParamDef>& params() const override
    {
        static const std::vector<ParamDef> ps {
            { "threshold", "Threshold (dB)", -90, 0, -40 }, { "attack", "Attack (ms)", 0.05f, 20, 0.5f }, { "hold", "Hold (ms)", 0, 500, 80 },
            { "release", "Release (ms)", 5, 1000, 120 }, { "range", "Gate Range (dB)", -80, 0, -80 },
            { "depth", "Iso Depth (dB)", 0, 60, 24 }, { "sens", "Sensitivity %", 0, 100, 50 }, { "smooth", "Smoothing (ms)", 1, 60, 8 },
            { "auto", "Auto Room", 0, 1, 1, { "Off", "On" } }, { "learn", "Learn Room", 0, 1, 0, { "Off", "Learning" } },
            { "lowcut", "Low Cut (Hz)", 20, 400, 80 }, { "highcut", "High Cut (Hz)", 2000, 20000, 16000 },
            { "keylow", "Voice Low (Hz)", 60, 1000, 180 }, { "keyhigh", "Voice High (Hz)", 1000, 12000, 5000 },
            { "mix", "Mix %", 0, 100, 100 }, { "out", "Output (dB)", -24, 24, 0 }, { "residual", "Listen to what's removed", 0, 1, 0, { "Off", "On" } } };
        return ps;
    }
    void prepare (double s, int maxBlock) override { engine.prepare (s, maxBlock); push(); }
    void reset() override { engine.reset(); }
    void process (juce::AudioBuffer<float>& b, int n, const PlayContext&) override
    {
        push();
        float* ch[2] { b.getWritePointer (0), b.getNumChannels() > 1 ? b.getWritePointer (1) : b.getWritePointer (0) };
        engine.process (ch, b.getNumChannels() > 1 ? 2 : 1, n);
    }
    float meter (int i) const override { return i == 0 ? engine.gateGainDb() : engine.suppressionDb(); }
private:
    hhir::IsoRoomEngine engine;
    std::array<float, 17> last {}; bool pushed = false;
    void push()
    {
        std::array<float, 17> now {};
        for (int i = 0; i < 17; ++i) now[(size_t) i] = p (i);
        if (pushed && now == last) return;   // filters are only recomputed when a knob moved
        last = now; pushed = true;
        hhir::IsoParameters q;
        q.thresholdDb = p (0); q.attackMs = p (1); q.holdMs = p (2); q.releaseMs = p (3); q.rangeDb = p (4);
        q.depthDb = p (5); q.sensitivity = p (6) / 100.0f; q.smoothingMs = p (7); q.autoTrack = p (8) > 0.5f; q.learning = p (9) > 0.5f;
        q.lowCutHz = p (10); q.highCutHz = p (11); q.keyLowHz = p (12); q.keyHighHz = p (13);
        q.mix = p (14) / 100.0f; q.outputDb = p (15); q.listenResidual = p (16) > 0.5f;
        engine.setParameters (q);
    }
};
std::unique_ptr<BuiltinEffect> createIsoRoom() { return std::make_unique<IsoRoomFx>(); }
}
