#include "app/GlyphAtlas.h"

#include "app/Theme.h"

namespace app
{
juce::Image glyphAtlas()
{
    constexpr float kMonoEm = 13.3333f;  // JetBrains Mono advances 0.6 em: 8 px
    constexpr int kMonoBaseline = 12;    // cap height 0.73 em ≈ 9.7 px, centred in the 15 px cell
    juce::Image img(juce::Image::ARGB, kAtlasW, kAtlasH, true, juce::SoftwareImageType());
    juce::Graphics g(img);
    g.setColour(juce::Colours::white);
    for (int face = 0; face < 2; ++face)
    {
        g.setFont(theme::mono(face == 1, kMonoEm));
        for (int c = 32; c < 127; ++c)
        {
            const auto cell = glyphCell(static_cast<char>(face == 1 ? (c | kBold) : c));
            g.saveState();
            g.reduceClipRegion(cell.x, cell.y, kGlyphW, kGlyphH);
            g.drawSingleLineText(juce::String::charToString(static_cast<juce::juce_wchar>(c)), cell.x, cell.y + kMonoBaseline);
            g.restoreState();
        }
    }
    return img;
}
} // namespace app
