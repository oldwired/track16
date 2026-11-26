// The single outputs OUT 3–8: where each module channel plays, and what each jack carries. Pure data and rules (no
// audio, no UI), shared by the player, the screen and the tests.
//
// Jacks 0–5 = OUT 3–8, each its own DAC channel. A channel's destination is MAIN (the default), one jack in mono, or a
// jack pair in stereo (3-4, 5-6, 7-8); "+MAIN" keeps a channel that goes to a single out on MAIN as well. Each jack
// carries the channels sent to it (Source::Channels) or one of the utility signals: the clean mix (MAIN's channels
// before the analog filter and DRIVE), the filter's return, or a metronome click. A jack pair whose two jacks carry
// the same utility signal plays it in stereo; a single jack gets its mono sum.
//
// Text form (track16.conf `outs=`, the per-module files): space-separated fields, channels 1-based,
//   src=ch,ch,clean,clean,ret,click   the six jacks' sources (missing ones: ch)
//   1=3-4  2=5  7=7-8+                channel → destination; "+" = also on MAIN
#pragma once

#include "app/Song.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace app::outs
{
constexpr int kJacks = 6;    // OUT 3 … OUT 8
constexpr int kMaxStems = 6; // libopenmpt instances for the single outs, at most (one per destination in use)

// Destinations: 0 = MAIN, 1–6 = jack 3–8 in mono, 7–9 = the pairs 3-4, 5-6, 7-8.
constexpr std::uint8_t kMain = 0, kDests = 10;
constexpr std::uint8_t mono(int jack0) noexcept { return static_cast<std::uint8_t>(1 + jack0); }
constexpr std::uint8_t pair(int pair0) noexcept { return static_cast<std::uint8_t>(7 + pair0); }
constexpr bool isMono(std::uint8_t d) noexcept { return d >= 1 && d <= 6; }
constexpr bool isPair(std::uint8_t d) noexcept { return d >= 7 && d <= 9; }
constexpr int monoJack(std::uint8_t d) noexcept { return d - 1; } // isMono(d)
constexpr int pairIndex(std::uint8_t d) noexcept { return d - 7; } // isPair(d)
// The jacks a destination plays on, as bits 0–5.
constexpr unsigned jackMask(std::uint8_t d) noexcept
{
    return isMono(d) ? 1u << monoJack(d) : isPair(d) ? 3u << (2 * pairIndex(d)) : 0u;
}
constexpr int group(std::uint8_t d) noexcept { return isMono(d) ? monoJack(d) / 2 : isPair(d) ? pairIndex(d) : -1; } // 0 = 3-4 …

enum class Source : std::uint8_t { Channels, Clean, Return, Click, Count };

struct Layout
{
    std::array<std::uint8_t, kMaxChannels> dest {};
    std::uint64_t alsoMain = 0;
    std::array<Source, kJacks> source {};
    bool operator==(const Layout&) const = default;
};

std::string destName(std::uint8_t d);   // "MAIN", "3", "3-4"
const char* sourceName(Source s);       // "CH", "CLEAN", "RETURN", "CLICK"
const char* sourceLongName(Source s);   // "CHANNELS", "CLEAN MIX", "FILTER RETURN", "CLICK"
Source nextSource(Source s) noexcept;   // the cycle CH → CLEAN → RETURN → CLICK → CH

std::string format(const Layout&);
Layout parse(std::string_view text); // unknown fields are skipped

// The touchscreen's rule for a tap on jack `jack0` of a channel now at `dest`: the same mono jack again → MAIN; the
// other jack of its pair → the pair; one jack of the pair it is on → the other jack alone; anything else → that jack.
std::uint8_t tapJack(std::uint8_t dest, int jack0) noexcept;
// The panel's rule (a pad held, STEP 3–8 pressed): that jack, or its pair while the pair's other STEP key is held.
constexpr std::uint8_t stepJack(int jack0, bool partnerHeld) noexcept { return partnerHeld ? pair(jack0 / 2) : mono(jack0); }
} // namespace app::outs
