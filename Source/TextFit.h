#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <unordered_map>

// Words always fit: shrink the font (down to 7 px) before squeezing, never cut a word off with "..."
namespace hhs
{
inline juce::Font fitFont (juce::Font f, const juce::String& text, float width, int lines = 1)
{
    if (text.isEmpty() || width <= 1.0f) return f;
    // measuring glyphs every paint was the top UI-thread cost (mixer strips): remember the fitted height
    thread_local std::unordered_map<juce::String, float> fitted;
    const auto key = text + "\x1f" + f.toString() + "\x1f" + juce::String (f.getHorizontalScale()) + "\x1f" + juce::String (f.getExtraKerningFactor())
                     + "\x1f" + juce::String (width) + "\x1f" + juce::String (lines);
    if (auto it = fitted.find (key); it != fitted.end())
        return it->second == f.getHeight() ? f : f.withHeight (it->second);
    if (fitted.size() > 8192) fitted.clear();
    const float room = width * (float) juce::jmax (1, lines) * 0.97f;
    juce::String longest;   // a single word can't wrap: it alone must fit one line
    for (auto& w : juce::StringArray::fromTokens (text, " ", {})) if (w.length() > longest.length()) longest = w;
    auto tooWide = [&] { return juce::GlyphArrangement::getStringWidth (f, text) > room || juce::GlyphArrangement::getStringWidth (f, longest) > width * 0.97f; };
    for (int i = 0; i < 30 && f.getHeight() > 6.0f && tooWide(); ++i)
        f = f.withHeight (juce::jmax (6.0f, f.getHeight() * 0.92f));
    fitted[key] = f.getHeight();
    return f;
}
template <typename T>
inline void fitText (juce::Graphics& g, const juce::String& text, juce::Rectangle<T> r, juce::Justification j, bool = true)
{
    const auto a = r.toNearestInt(); const auto old = g.getCurrentFont();
    g.setFont (fitFont (old, text, (float) a.getWidth()));
    g.drawFittedText (text, a, j, 1, 0.6f);
    g.setFont (old);
}
inline void fitText (juce::Graphics& g, const juce::String& text, double x, double y, double w, double h, juce::Justification j, bool e = true)
{
    fitText (g, text, juce::Rectangle<int> ((int) x, (int) y, (int) w, (int) h), j, e);
}
}
