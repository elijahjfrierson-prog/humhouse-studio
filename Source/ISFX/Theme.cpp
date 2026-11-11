#include "App.h"
#include "ISFXData.h"

namespace isfx
{
ISFXLookAndFeel::ISFXLookAndFeel()
{
    using namespace theme;
    if (auto face = logoTypeface()) setDefaultSansSerifTypeface (face);   // whole UI in the logo's wide face
    setColour (juce::ResizableWindow::backgroundColourId, bg);
    setColour (juce::TextButton::buttonColourId, panel2);
    setColour (juce::TextButton::buttonOnColourId, accent);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    setColour (juce::ComboBox::backgroundColourId, panel2);
    setColour (juce::ComboBox::outlineColourId, line);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::arrowColourId, dim);
    setColour (juce::PopupMenu::backgroundColourId, panel);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.8f));
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::Label::textColourId, text);
    setColour (juce::Slider::thumbColourId, accent);
    setColour (juce::Slider::trackColourId, accent.withAlpha (0.6f));
    setColour (juce::Slider::backgroundColourId, line);
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, panel2);
    setColour (juce::ListBox::backgroundColourId, panel);
    setColour (juce::ScrollBar::thumbColourId, line.brighter (0.3f));
    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xf0222632));
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, accent2.withAlpha (0.5f));
    setColour (juce::AlertWindow::backgroundColourId, panel);
    setColour (juce::AlertWindow::textColourId, text);
    setColour (juce::TextEditor::backgroundColourId, panel2);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::outlineColourId, line);
    setColour (juce::ToggleButton::textColourId, text);
    setColour (juce::ToggleButton::tickColourId, accent);
    setColour (juce::ProgressBar::foregroundColourId, accent);
    setColour (juce::ProgressBar::backgroundColourId, panel2);
}

void ISFXLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& c, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    auto col = b.getToggleState() ? theme::accent : c;
    if (over) col = col.brighter (0.12f);
    if (down) col = col.darker (0.2f);
    g.setColour (col);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (b.getToggleState() ? theme::accent.brighter (0.3f) : theme::line);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
}

void drawLogo (juce::Graphics& g, juce::Rectangle<float> r)
{
    static const juce::Image mark = juce::ImageCache::getFromMemory (ISFXData::insequencemark_png, ISFXData::insequencemark_pngSize);
    if (! mark.isValid()) return;
    float h = std::min (r.getHeight() * 0.86f, r.getWidth());
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (mark, { r.getX(), r.getCentreY() - h * 0.5f, h, h });
}

juce::Typeface::Ptr logoTypeface()
{
    static const juce::Typeface::Ptr face = juce::Typeface::createSystemTypefaceFor (ISFXData::MichromaRegular_ttf, ISFXData::MichromaRegular_ttfSize);
    return face;
}

juce::Font headerFont (float height)
{
    auto face = logoTypeface();
    return face != nullptr ? juce::Font (juce::FontOptions (face).withHeight (height)) : juce::Font (juce::FontOptions (height, juce::Font::bold));
}
}
