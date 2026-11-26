#include "app/Song.h"

#include "app/Log.h"

#include <libopenmpt/libopenmpt.hpp>
#include <libopenmpt/libopenmpt_ext.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

namespace app
{
namespace
{
// libopenmpt's log, line by line into ours (warnings while loading).
class LogBuffer final : public std::stringbuf
{
public:
    int sync() override
    {
        std::string text = str();
        str({});
        for (std::size_t a = 0, b; a < text.size(); a = b + 1)
        {
            b = text.find('\n', a);
            if (b == std::string::npos)
                b = text.size();
            if (b > a)
                logLine("OPENMPT %s", text.substr(a, b - a).c_str());
        }
        return 0;
    }
};

void copyText(char* out, std::size_t n, const std::string& s, char fill)
{
    for (std::size_t i = 0; i < n; ++i)
        out[i] = i < s.size() && s[i] != ' ' ? s[i] : fill;
}

std::unique_ptr<openmpt::module_ext> open(const std::vector<char>& bytes, std::ostream& log, int stereo, int interp)
{
    std::map<std::string, std::string> ctls {{"play.at_end", "continue"}};
    auto m = std::make_unique<openmpt::module_ext>(bytes, log, ctls);
    m->set_repeat_count(-1); // loop forever; the player decides when to move on
    m->set_render_param(openmpt::module::RENDER_STEREOSEPARATION_PERCENT, stereo);
    m->set_render_param(openmpt::module::RENDER_INTERPOLATIONFILTER_LENGTH, interp);
    return m;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
} // namespace

Song::Song() = default;
Song::~Song() = default;

bool looksLikeModule(const std::string& fileName)
{
    const auto dot = fileName.rfind('.');
    if (dot == std::string::npos || dot + 1 >= fileName.size() || fileName[0] == '.')
        return false;
    static const std::vector<std::string> extensions = [] {
        auto list = openmpt::get_supported_extensions();
        for (auto& e : list)
            e = lower(e);
        return list;
    }();
    const auto ext = lower(fileName.substr(dot + 1));
    if (std::find(extensions.begin(), extensions.end(), ext) != extensions.end())
        return true;
    // Amiga style "mod.name" prefixes.
    const auto prefix = lower(fileName.substr(0, std::min<std::size_t>(fileName.find('.'), 8)));
    return prefix == "mod" || prefix == "xm" || prefix == "s3m" || prefix == "it";
}

std::unique_ptr<Song> loadSong(const std::string& path, bool stems, int sampleRate, int stereoSeparation, int interpolation, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (! in)
        return error = "cannot open " + path, nullptr;
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty())
        return error = "empty file", nullptr;

    static LogBuffer logBuffer;
    static std::ostream log(&logBuffer);
    auto song = std::make_unique<Song>();
    try
    {
        song->dry = open(bytes, log, stereoSeparation, interpolation);
        if (stems)
            song->wet = open(bytes, log, stereoSeparation, interpolation);
    }
    catch (const std::exception& e)
    {
        log.flush();
        return error = e.what(), nullptr;
    }
    log.flush();
    song->dryI = static_cast<openmpt::ext::interactive*>(song->dry->get_interface(openmpt::ext::interactive_id));
    song->dryI2 = static_cast<openmpt::ext::interactive2*>(song->dry->get_interface(openmpt::ext::interactive2_id));
    if (song->wet)
        song->wetI = static_cast<openmpt::ext::interactive*>(song->wet->get_interface(openmpt::ext::interactive_id));
    song->dryP = static_cast<openmpt::ext::track16_position*>(song->dry->get_interface(openmpt::ext::track16_position_id));
    if (song->wet)
        song->wetP = static_cast<openmpt::ext::track16_position*>(song->wet->get_interface(openmpt::ext::track16_position_id));
    if (song->dryI == nullptr || song->dryP == nullptr || (song->wet && (song->wetI == nullptr || song->wetP == nullptr)))
        return error = "libopenmpt without the interactive or track16_position extension", nullptr;
    try
    {
        // The mixer for the device's rate now (a read of 0 frames only sets it up), not on the audio thread.
        float none[1] = {};
        song->dry->read(sampleRate, 0, none, none);
        if (song->wet)
        {
            song->wet->read(sampleRate, 0, none, none);
            // The wet instance starts with every channel muted (the player unmutes the routed ones); channels past
            // kMaxChannels are never routed, so they only ever play on the dry one.
            for (int c = 0; c < song->wet->get_num_channels(); ++c)
                song->wetI->set_channel_mute_status(c, true);
        }
    }
    catch (const std::exception& e)
    {
        return error = e.what(), nullptr;
    }

    // The snapshot.
    auto& m = *song->dry;
    auto d = std::make_shared<SongData>();
    d->path = path;
    const auto slash = path.rfind('/');
    d->fileName = slash == std::string::npos ? path : path.substr(slash + 1);
    d->title = m.get_metadata("title");
    d->artist = m.get_metadata("artist");
    d->type = m.get_metadata("type");
    d->typeLong = m.get_metadata("type_long");
    d->tracker = m.get_metadata("tracker");
    d->message = m.get_metadata("message");
    d->channels = m.get_num_channels();
    d->numOrders = m.get_num_orders();
    d->numPatterns = m.get_num_patterns();
    d->numInstruments = m.get_num_instruments();
    d->numSamples = m.get_num_samples();
    d->durationSeconds = m.get_duration_seconds();
    d->instrumentNames = d->numInstruments > 0 ? m.get_instrument_names() : m.get_sample_names();
    d->channelNames = m.get_channel_names();
    if (d->title.empty())
        d->title = d->fileName;
    d->orders.resize(static_cast<std::size_t>(d->numOrders));
    for (int o = 0; o < d->numOrders; ++o)
        d->orders[static_cast<std::size_t>(o)] = m.is_order_skip_entry(o) ? -1 : m.is_order_stop_entry(o) ? -2 : m.get_order_pattern(o);
    d->patterns.resize(static_cast<std::size_t>(d->numPatterns));
    const int ch = d->channels;
    for (int p = 0; p < d->numPatterns; ++p)
    {
        auto& pd = d->patterns[static_cast<std::size_t>(p)];
        pd.rows = m.get_pattern_num_rows(p);
        pd.cells.resize(static_cast<std::size_t>(pd.rows * ch));
        for (int r = 0; r < pd.rows; ++r)
            for (int c = 0; c < ch; ++c)
            {
                Cell& cell = pd.cells[static_cast<std::size_t>(r * ch + c)];
                using M = openmpt::module;
                cell.noteValue = m.get_pattern_row_channel_command(p, r, c, M::command_note);
                cell.instrument = m.get_pattern_row_channel_command(p, r, c, M::command_instrument);
                cell.volCommand = m.get_pattern_row_channel_command(p, r, c, M::command_volumeffect);
                cell.volValue = m.get_pattern_row_channel_command(p, r, c, M::command_volume);
                const auto effect = m.get_pattern_row_channel_command(p, r, c, M::command_effect);
                const auto param = m.get_pattern_row_channel_command(p, r, c, M::command_parameter);
                if (cell.noteValue != 0)
                    copyText(cell.note, 3, m.format_pattern_row_channel_command(p, r, c, M::command_note), '.');
                if (cell.instrument != 0)
                {
                    char hex[4];
                    std::snprintf(hex, sizeof hex, "%02X", cell.instrument);
                    copyText(cell.inst, 2, hex, '.');
                }
                if (cell.volCommand != 0)
                    copyText(cell.vol, 3,
                             m.format_pattern_row_channel_command(p, r, c, M::command_volumeffect)
                                 + m.format_pattern_row_channel_command(p, r, c, M::command_volume),
                             '.');
                if (effect != 0 || param != 0)
                    copyText(cell.fx, 3,
                             m.format_pattern_row_channel_command(p, r, c, M::command_effect)
                                 + m.format_pattern_row_channel_command(p, r, c, M::command_parameter),
                             '.');
            }
    }
    song->data = std::move(d);
    return song;
}
} // namespace app
