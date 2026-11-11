#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// Words always fit: shrink the font (down to 7 px) before squeezing, never cut a word off with "..."
namespace hhs
{
inline juce::Font fitFont (juce::Font f, const juce::String& text, float width, int lines = 1)
{
    if (text.isEmpty() || width <= 1.0f) return f;
    const float room = width * (float) juce::jmax (1, lines) * 0.97f;
    juce::String longest;   // a single word can't wrap: it alone must fit one line
    for (auto& w : juce::StringArray::fromTokens (text, " ", {})) if (w.length() > longest.length()) longest = w;
    auto tooWide = [&] { return juce::GlyphArrangement::getStringWidth (f, text) > room || juce::GlyphArrangement::getStringWidth (f, longest) > width * 0.97f; };
    for (int i = 0; i < 30 && f.getHeight() > 6.0f && tooWide(); ++i)
        f = f.withHeight (juce::jmax (6.0f, f.getHeight() * 0.92f));
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
