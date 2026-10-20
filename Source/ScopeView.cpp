#include "UI.h"

namespace hhs
{
namespace
{
// Ableton-style scope: oscilloscope, scrolling waveform history, spectrum analyzer
class ScopeView : public juce::Component, private juce::Timer
{
public:
    ScopeView (std::function<ScopeTap*()> src, juce::String t) : source (std::move (src)), title (std::move (t))
    {
        const char* names[] { "OSC", "WAVE", "SPECTRUM" };
        const char* tips[] { "Oscilloscope: the live wave shape", "Waveform: scrolling history of the last few seconds",
                             "Spectrum: frequency analyzer, 20 Hz - 20 kHz" };
        for (int i = 0; i < 3; ++i)
        {
            auto* b = modeBtns.add (new juce::TextButton());
            setupButton (*b, names[i], tips[i], true);
            b->setRadioGroupId (5101);
            b->onClick = [this, i] { mode = i; repaint(); };
            addAndMakeVisible (b);
        }
        modeBtns[0]->setToggleState (true, juce::dontSendNotification);
        setupButton (freezeBtn, "FREEZE", "Freeze the display", true);
        addAndMakeVisible (freezeBtn);
        setSize (760, 380);
        startTimerHz (30);
    }
    void resized() override
    {
        auto t = getLocalBounds().reduced (8).removeFromTop (26);
        t.removeFromLeft (150);
        for (auto* b : modeBtns) { b->setBounds (t.removeFromLeft (b == modeBtns[2] ? 90 : 60)); t.removeFromLeft (4); }
        freezeBtn.setBounds (t.removeFromRight (70));
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (col::bg);
        g.setColour (col::text); g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText (title, 12, 8, 140, 26, juce::Justification::centredLeft);
        auto area = getLocalBounds().reduced (10).withTrimmedTop (34).toFloat();
        g.setColour (col::panel); g.fillRoundedRectangle (area, 6);
        auto a = area.reduced (8);
        g.setColour (col::grid);
        for (int i = 1; i < 8; ++i) g.drawVerticalLine ((int) (a.getX() + a.getWidth() * (float) i / 8), a.getY(), a.getBottom());
        for (int i = 1; i < 4; ++i) g.drawHorizontalLine ((int) (a.getY() + a.getHeight() * (float) i / 4), a.getX(), a.getRight());
        if (mode == 0) drawOsc (g, a); else if (mode == 1) drawHist (g, a); else drawSpec (g, a);
    }

private:
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder, histMax = 4096, chunk = 128;
    std::function<ScopeTap*()> source;
    juce::String title;
    juce::OwnedArray<juce::TextButton> modeBtns;
    juce::TextButton freezeBtn;
    int mode = 0, histPos = 0, lastPos = -1;
    double rate = 44100.0;
    std::vector<float> snap = std::vector<float> ((size_t) fftSize, 0.0f);
    std::vector<std::pair<float, float>> hist = std::vector<std::pair<float, float>> ((size_t) histMax, { 0.0f, 0.0f });
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> fftBuf = std::vector<float> ((size_t) (2 * fftSize), 0.0f);
    std::vector<float> spec = std::vector<float> ((size_t) (fftSize / 2), -100.0f);

    void timerCallback() override
    {
        if (freezeBtn.getToggleState()) return;
        auto* tap = source ? source() : nullptr;
        if (tap == nullptr) { std::fill (snap.begin(), snap.end(), 0.0f); repaint(); return; }
        rate = tap->rate.load();
        const int w = tap->pos.load();
        tap->read (snap.data(), fftSize, w);
        if (lastPos < 0) lastPos = w;
        int raw = (w - lastPos + ScopeTap::size) % ScopeTap::size;
        if (raw > fftSize) { lastPos = (w - fftSize + ScopeTap::size) % ScopeTap::size; raw = fftSize; }
        const int fresh = raw - raw % chunk;   // whole chunks; the rest waits for the next tick
        for (int c = 0; c < fresh / chunk; ++c)
        {
            float lo = 0, hi = 0;
            for (int i = 0; i < chunk; ++i)
            {
                const float v = snap[(size_t) (fftSize - raw + c * chunk + i)];
                lo = juce::jmin (lo, v); hi = juce::jmax (hi, v);
            }
            hist[(size_t) histPos] = { lo, hi };
            histPos = (histPos + 1) % histMax;
        }
        lastPos = (lastPos + fresh) % ScopeTap::size;

        std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);
        std::copy (snap.begin(), snap.end(), fftBuf.begin());
        window.multiplyWithWindowingTable (fftBuf.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (fftBuf.data());
        for (int i = 0; i < fftSize / 2; ++i)
        {
            const float db = juce::Decibels::gainToDecibels (fftBuf[(size_t) i] * 4.0f / (float) fftSize, -100.0f);
            auto& sp = spec[(size_t) i];
            sp = db > sp ? db : sp * 0.85f + db * 0.15f;
        }
        repaint();
    }
    void drawOsc (juce::Graphics& g, juce::Rectangle<float> a)
    {
        const int span = juce::jlimit (256, fftSize / 2, (int) (rate * 0.02));   // ~20 ms window
        int start = fftSize - span - 1;
        for (int i = fftSize - span - 1; i > fftSize / 2 - span && i > 0; --i)
            if (snap[(size_t) (i - 1)] < 0.0f && snap[(size_t) i] >= 0.0f) { start = i; break; }   // rising-edge trigger
        juce::Path fill, line;
        const float mid = a.getCentreY(), hh = a.getHeight() * 0.48f;
        const int wpx = juce::jmax (1, (int) a.getWidth());
        fill.startNewSubPath (a.getX(), mid);
        for (int x = 0; x < wpx; ++x)
        {
            const float v = juce::jlimit (-1.0f, 1.0f, snap[(size_t) (start + x * span / wpx)]);
            const float px = a.getX() + (float) x, py = mid - v * hh;
            fill.lineTo (px, py);
            if (x == 0) line.startNewSubPath (px, py); else line.lineTo (px, py);
        }
        fill.lineTo (a.getRight(), mid); fill.closeSubPath();
        const float top = mid - hh, bot = mid + hh;
        g.setGradientFill (col::mirrorGradient (0, top, bot, 0.22f)); g.fillPath (fill);
        g.setGradientFill (col::mirrorGradient (0, top, bot, 0.25f));
        g.strokePath (line, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setGradientFill (col::mirrorGradient (0, top, bot));
        g.strokePath (line, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    void drawHist (juce::Graphics& g, juce::Rectangle<float> a)
    {
        const int cols = juce::jmin (histMax, (int) a.getWidth());
        const float mid = a.getCentreY(), hh = a.getHeight() * 0.48f;
        g.setGradientFill (col::mirrorGradient (0, mid - hh, mid + hh, 0.9f));
        for (int x = 0; x < cols; ++x)
        {
            const auto [lo, hi] = hist[(size_t) ((histPos - cols + x + histMax) % histMax)];
            const float px = a.getRight() - (float) cols + (float) x;
            g.drawVerticalLine ((int) px, mid - juce::jmin (1.0f, hi) * hh, mid - juce::jmax (-1.0f, lo) * hh + 1.0f);
        }
        g.setColour (col::dim); g.drawHorizontalLine ((int) mid, a.getX(), a.getRight());
    }
    void drawSpec (juce::Graphics& g, juce::Rectangle<float> a)
    {
        auto xFor = [&] (double hz) { return a.getX() + a.getWidth() * (float) (std::log10 (hz / 20.0) / 3.0); };
        auto yFor = [&] (float db) { return a.getBottom() - a.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 90.0f) / 90.0f); };
        g.setFont (juce::FontOptions (10.0f));
        for (double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const float x = xFor (hz);
            g.setColour (col::grid); g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
            g.setColour (col::dim);
            g.drawText (hz >= 1000.0 ? juce::String ((int) (hz / 1000.0)) + "k" : juce::String ((int) hz), (int) x + 2, (int) a.getBottom() - 14, 40, 12, juce::Justification::left);
        }
        juce::Path p;
        p.startNewSubPath (a.getX(), a.getBottom());
        const double binHz = rate / fftSize;
        const int wpx = juce::jmax (1, (int) a.getWidth());
        for (int x = 0; x <= wpx; ++x)
        {
            const double hz = 20.0 * std::pow (1000.0, x / (double) wpx);
            const int bin = juce::jlimit (1, fftSize / 2 - 1, (int) (hz / binHz));
            p.lineTo (a.getX() + (float) x, yFor (spec[(size_t) bin]));
        }
        p.lineTo (a.getRight(), a.getBottom()); p.closeSubPath();
        g.setGradientFill (col::levelGradient (0, a.getY(), a.getBottom(), 0.45f));
        g.fillPath (p);
        g.setGradientFill (col::levelGradient (0, a.getY(), a.getBottom())); g.strokePath (p, juce::PathStrokeType (1.4f));
    }
};
}

juce::Component* createScopeView (std::function<ScopeTap*()> source, const juce::String& title)
{
    return new ScopeView (std::move (source), title);
}
}
