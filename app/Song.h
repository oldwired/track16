// A loaded module: the libopenmpt instances the audio thread plays, plus an immutable snapshot of everything the UI
// and the MIDI note output read (pattern cells, order list, names), so no thread but the audio thread ever calls into
// libopenmpt while it plays.
//
// Stems: with the analog filter loop present the song has a second instance of the same module ("wet"). Both play in
// lockstep with complementary channel masks; the wet one carries the channels that go through the filter.
//
// Loading (loadSong) runs on the message thread; the Song is then handed to the audio thread (Player::load) and
// handed back for deletion (Player::collect).
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

struct Song
{
    std::shared_ptr<const SongData> data;
    std::unique_ptr<openmpt::module_ext> dry, wet; // wet: only with stems
    openmpt::ext::interactive* dryI = nullptr;
    openmpt::ext::interactive* wetI = nullptr;
    openmpt::ext::interactive2* dryI2 = nullptr;
    Song();
    ~Song();
};

// Reads and parses a module file (message thread). `stems`: create the second ("wet") instance as well.
// Null + error on failure.
std::unique_ptr<Song> loadSong(const std::string& path, bool stems, int stereoSeparation, int interpolation, std::string& error);

// True for file names libopenmpt is likely to play (by extension; the loader decides).
bool looksLikeModule(const std::string& fileName);
} // namespace app
