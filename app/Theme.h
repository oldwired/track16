// freesp16's look: its colour tokens (dark palette), the 16 track colours, the Inter type scale, the layout grid and a
// few drawing helpers. Screens are drawn on freesp16's 1280 × 768 "lpx" canvas, scaled to the 800 × 480 LCD (× 0.625),
// so every size and position here is freesp16's own number.
//
// The fonts are compiled into the binary (the unit has no font files): call installFonts() once after JUCE starts and
// give your root component lookAndFeel() (Screen does), so every JUCE widget inside it draws with Inter.
// Not juce::LookAndFeel::setDefaultLookAndFeel(): that creates juce::Desktop, which opens JUCE's own X11 windowing, and
// that crashes on the unit.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>

namespace app::theme
{
inline constexpr float kCanvasW = 1280, kCanvasH = 768, kScale = 0.625f; // canvas lpx → LCD pixels

// ---- colour (tokens.json color.*, dark) ----
namespace colour
{
inline const juce::Colour bg {0xFF0B0D10}, surface1 {0xFF13161B}, surface2 {0xFF1A1E25}, surface3 {0xFF232833},
    overlay {0xB80B0D10}, border {0xFF2A303B}, borderStrong {0xFF3A4250}, text1 {0xFFE8ECF2}, text2 {0xFFA3ACBA},
    text3 {0xFF6B7482}, accent {0xFF569DFF}, accentWeak {0x29569DFF}, onAccent {0xFF0B0D10}, mod {0xFFFE6270},
    play {0xFF4ADD8C}, rec {0xFFF53C41}, warn {0xFFF7B83D}, playhead {0xFFFFFFFF}, meterLow {0xFF4ADD8C},
    meterMid {0xFFF7B83D}, meterHigh {0xFFF53C41};
} // namespace colour

// Track colours 1–16 (base, dim), and the saturated LED colour the panel shows for each (freesp16 ui/theme/Theme.h).
struct TrackColour
{
    std::uint32_t base, dim, led;
};
inline constexpr TrackColour kTrack[16] = {
    {0xFFFF7E72, 0xFF7A342E, 0xFF0000}, {0xFFFD8358, 0xFF79371E, 0xFF3000}, {0xFFF68C36, 0xFF753B07, 0xFF7000},
    {0xFFEA9602, 0xFF6F4100, 0xFFC000}, {0xFFD9A100, 0xFF664700, 0x80FF00}, {0xFFC3AC00, 0xFF5A4D00, 0x00FF00},
    {0xFF94BB3B, 0xFF41560A, 0x00FF50}, {0xFF51C672, 0xFF195C2E, 0x00FFB0}, {0xFF00CA9B, 0xFF005E45, 0x00C0FF},
    {0xFF00C7D2, 0xFF005C62, 0x0070FF}, {0xFF3FB3FF, 0xFF0B517F, 0x0000FF}, {0xFF88A2FF, 0xFF3A4883, 0x5000FF},
    {0xFFB394FF, 0xFF52407D, 0xA000FF}, {0xFFD787E9, 0xFF65396F, 0xFF00FF}, {0xFFF07EC2, 0xFF723459, 0xFF0080},
    {0xFFFE7B94, 0xFF793240, 0xFF80A0}};
inline juce::Colour track(int track1Based, bool dim = false)
{
    const auto& t = kTrack[(track1Based - 1) & 15];
    return juce::Colour(dim ? t.dim : t.base);
}

// ---- type (tokens.json typography.*): Inter; caps styles are written in upper case by the caller ----
enum class Text { Caption, Label, Body, BodyStrong, Title, ValueS, Value, ValueL };
juce::Font font(Text);
juce::Font mono(bool bold, float em); // JetBrains Mono (Medium / Bold): the pattern rain's glyph atlas
float lineHeight(Text);
void installFonts();               // after JUCE's GUI is initialised
void releaseFonts();               // after every component that uses them
juce::LookAndFeel& lookAndFeel();  // Inter for every widget of the component it is set on

// ---- geometry (tokens.json radius / space, Layout.h) ----
inline constexpr float radiusXs = 4, radiusS = 6, radiusM = 8, radiusL = 12;
inline constexpr float pad = 16, gap = 8;
namespace layout
{
inline constexpr float statusH = 40, contextY = 40, contextH = 96, stripH = 96, stripY = kCanvasH - stripH;
inline constexpr float bodyY = contextY + contextH, contentTop = bodyY + pad, contentBottom = stripY - pad; // 152–656
inline constexpr float ctlY = contextY + 12, ctlH = 72, captionY = 72, valueY = 104;
inline const juce::Rectangle<float> slotA {16, ctlY, 96, ctlH}, slotB {120, ctlY, 256, ctlH}, slotC {384, ctlY, 256, ctlH},
    slotD {648, ctlY, 160, ctlH}, slotE {824, ctlY, 440, ctlH};
inline constexpr float readoutX = 664, menuButtonW = 104;
inline juce::Rectangle<float> menuButton(int i) { return {slotE.getX() + (menuButtonW + gap) * static_cast<float>(i), ctlY, menuButtonW, ctlH}; }
inline constexpr float columnX0 = 16, columnW = 208;
inline constexpr float columnCaptionY = stripY + 21, columnValueY = stripY + 50, columnUnderlineY = stripY + 67,
                       columnTrackY = stripY + 79;
inline constexpr float meterColumnX = 1176;
} // namespace layout

// ---- motion (tokens.json motion.*, ui/shell/Timing.h) ----
inline constexpr double padFlashMs = 1000, recentlyTouchedMs = 600, midiActivityMs = 100, blinkSlowMs = 1000,
                        peakHoldMs = 500, meterReleaseMs = 300;

// ---- drawing helpers, in canvas lpx ----
enum class Align { Left, Centre, Right };
// Text centred vertically on `cy` (freesp16's DrawList::text convention).
void text(juce::Graphics&, float x, float cy, Align, Text, juce::Colour, const juce::String&);
float textWidth(Text, const juce::String&);
// A caption badge 22 lpx tall (track tiles, the context bar); w = 0 sizes it to the text + 16. Returns its right edge.
float badge(juce::Graphics&, float x, float y, const juce::String&, juce::Colour fill, juce::Colour ink, float w = 0);
void hline(juce::Graphics&, float x0, float x1, float y, juce::Colour);
void vline(juce::Graphics&, float x, float y0, float y1, juce::Colour);

// The vector icons of freesp16's ui/theme/Glyphs.cpp (24-unit grid, 2-unit strokes).
enum class Glyph { Back, Project, Tracks, Mixer, Arranger, Utility, Play, Dot };
void glyph(juce::Graphics&, Glyph, float x, float y, float size, juce::Colour);

// A stereo meter column with freesp16's dB scale (ui/widgets/Meter.cpp): bars from `top` to `bottom`, +3 … −50 dB.
float meterY(float db, float top, float bottom);
void meterBars(juce::Graphics&, float x, float top, float bottom, float pitch, const float db[2], const float peakDb[2],
               bool dim);
} // namespace app::theme
