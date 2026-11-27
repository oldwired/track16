// The pattern rain's glyph atlas: printable ASCII in JetBrains Mono Medium, then Bold, kGlyphW × kGlyphH texels a
// cell, 16 cells a row, at the size they are shown (each texel one pixel). Drawn with JUCE (no GL here: the display
// uploads it, the preview tool samples it).
#pragma once

#include "app/Screen.h"

#include <juce_graphics/juce_graphics.h>

namespace app
{
constexpr int kAtlasW = 256, kAtlasH = 256;
juce::Image glyphAtlas(); // white, the glyphs in the alpha channel (call after theme::installFonts)
// The atlas cell of a SceneGlyph's character (ch | kBold for bold): its top left texel.
inline juce::Point<int> glyphCell(char ch) noexcept
{
    const auto c = static_cast<unsigned char>(ch);
    const int i = (static_cast<int>(c & 0x7F) < 32 || static_cast<int>(c & 0x7F) > 126 ? 0 : static_cast<int>(c & 0x7F) - 32)
                + ((c & kBold) != 0 ? 95 : 0);
    return {(i % 16) * kGlyphW, (i / 16) * kGlyphH};
}
} // namespace app
