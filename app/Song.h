// A loaded module: the libopenmpt instances the audio thread plays, plus an immutable snapshot of everything the UI
// and the MIDI note output read (pattern cells, order list, names), so no thread but the audio thread ever calls into
// libopenmpt while it plays.
//
// Stems: the song plays as several instances of the same module, in lockstep, each with its own channel mute mask, so
// that each channel is mixed once, in the instance whose output it belongs to. "dry" feeds MAIN; with the analog
// filter loop, "wet" carries the channels that go through the filter; the "outs" pool carries channels sent to the
// single outputs OUT 3–8 (one instance per destination in use, app/Outs.h). An instance leaves the channels it does
// not carry out of its mix with the vendored libopenmpt's set_channel_silent, not with mutes: a silent channel plays
// on in every other respect, so all instances stay identical and a channel can move between them mid-note. (A muted
// channel stands still: its sample position, NNA voices and, in S3M, its effects.)
//
// Loading (loadSong) runs on the screen's loader thread; the Song is then handed to the audio thread (Player::load)
// and handed back for deletion (Player::collect) on the message thread.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace openmpt
{
class module_ext;
namespace ext
{
class interactive;
class interactive2;
class track16_position;
} // namespace ext
} // namespace openmpt

namespace app
{
constexpr int kMaxChannels = 64; // channels the player tracks (mutes, filter, VU, rain); more are played but not addressable

// One pattern cell, pre-formatted for the rain (fixed-width text) plus the raw values the MIDI note output needs.
struct Cell
{
    char note[3] {'.', '.', '.'}; // "C-5", "===", "^^^", "..."
    char inst[2] {'.', '.'};      // "01" … "FF" (hex) or ".."
    char vol[3] {'.', '.', '.'};  // volume column: "v64", "p32", …
    char fx[3] {'.', '.', '.'};   // effect column: "A0F", …
    std::uint8_t noteValue = 0;   // 0 none, 1–120 notes (C-0 … B-9, 61 = C-5), 253 fade, 254 cut, 255 key off
    std::uint8_t instrument = 0;  // 0 none, 1-based
    std::uint8_t volCommand = 0;  // libopenmpt's volume command (1 = set volume)
    std::uint8_t volValue = 0;
};

struct PatternData
{
    int rows = 0;
    std::vector<Cell> cells; // rows × channels, row-major
};

struct SongData
{
    std::string path, fileName, title, artist, type, typeLong, tracker, message;
    int channels = 0, numOrders = 0, numPatterns = 0, numInstruments = 0, numSamples = 0;
    double durationSeconds = 0;
    double rowSeconds = 0;              // a typical row's length at tempo factor 1 (the first pattern's average)
    std::vector<int> orders;            // pattern per order; −1 = skip ("+++"), −2 = end ("---")
    std::vector<PatternData> patterns;
    std::vector<std::string> instrumentNames; // instruments, or samples when the module has no instruments
    std::vector<std::string> channelNames;

    const Cell* cell(int pattern, int row, int channel) const noexcept
    {
        if (pattern < 0 || pattern >= static_cast<int>(patterns.size()) || channel < 0 || channel >= channels)
            return nullptr;
        const auto& p = patterns[static_cast<std::size_t>(pattern)];
        if (row < 0 || row >= p.rows)
            return nullptr;
        return &p.cells[static_cast<std::size_t>(row * channels + channel)];
    }
    int patternRows(int pattern) const noexcept
    {
        return pattern >= 0 && pattern < static_cast<int>(patterns.size()) ? patterns[static_cast<std::size_t>(pattern)].rows : 0;
    }
    int orderPattern(int order) const noexcept
    {
        return order >= 0 && order < static_cast<int>(orders.size()) ? orders[static_cast<std::size_t>(order)] : -2;
    }
};

// One libopenmpt instance and its extension interfaces.
struct Stem
{
    std::unique_ptr<openmpt::module_ext> m;
    openmpt::ext::interactive* i = nullptr;
    openmpt::ext::interactive2* i2 = nullptr;
    openmpt::ext::track16_position* p = nullptr; // seeks, silence (third_party/libopenmpt)
    explicit operator bool() const noexcept { return m != nullptr; }
    Stem();
    Stem(Stem&&) noexcept;
    Stem& operator=(Stem&&) noexcept;
    ~Stem();
};

struct Song
{
    std::shared_ptr<const SongData> data;
    Stem dry, wet;          // wet: only with the filter loop
    std::vector<Stem> outs; // the single outputs' pool
    bool playOnAdopt = false; // the player starts it when it takes it over
    Song();
    ~Song();
    template <typename F>
    void each(F&& f) // every instance (they all move together)
    {
        f(dry);
        if (wet)
            f(wet);
        for (auto& s : outs)
            f(s);
    }
    int instances() const noexcept { return 1 + (wet ? 1 : 0) + static_cast<int>(outs.size()); }
};

// Reads and parses a module file (any thread but the audio thread). `stems`: create the filter's "wet" instance as
// well; `outStems`: that many instances for the single outputs (0–6). `sampleRate`: the mixer is set up for it here,
// so the audio thread's first read does no setup work. Every instance but "dry" starts with all channels silent
// (track16_position::set_channel_silent: they play on, unmixed).
// Null + error on failure.
std::unique_ptr<Song> loadSong(const std::string& path, bool stems, int outStems, int sampleRate, int stereoSeparation, int interpolation,
                               std::string& error);

// True for file names libopenmpt is likely to play (by extension; the loader decides).
bool looksLikeModule(const std::string& fileName);
} // namespace app
