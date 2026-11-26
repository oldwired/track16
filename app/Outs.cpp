#include "app/Outs.h"

#include <cstdlib>

namespace app::outs
{
namespace
{
constexpr const char* kShort[] = {"ch", "clean", "ret", "click"};
constexpr const char* kNames[] = {"CH", "CLEAN", "RETURN", "CLICK"};
constexpr const char* kLong[] = {"CHANNELS", "CLEAN MIX", "FILTER RETURN", "CLICK"};

// "3".."8" → jack 0–5, "3-4" "5-6" "7-8" → the pairs; anything else (also "main") → MAIN.
std::uint8_t parseDest(std::string_view s)
{
    if (s.size() == 1 && s[0] >= '3' && s[0] <= '8')
        return mono(s[0] - '3');
    if (s == "3-4" || s == "5-6" || s == "7-8")
        return pair((s[0] - '3') / 2);
    return kMain;
}
} // namespace

std::string destName(std::uint8_t d)
{
    if (isMono(d))
        return std::string(1, static_cast<char>('3' + monoJack(d)));
    if (isPair(d))
    {
        const char a = static_cast<char>('3' + 2 * pairIndex(d));
        return std::string {a, '-', static_cast<char>(a + 1)};
    }
    return "MAIN";
}

const char* sourceName(Source s) { return kNames[static_cast<int>(s) % static_cast<int>(Source::Count)]; }
const char* sourceLongName(Source s) { return kLong[static_cast<int>(s) % static_cast<int>(Source::Count)]; }
Source nextSource(Source s) noexcept { return static_cast<Source>((static_cast<int>(s) + 1) % static_cast<int>(Source::Count)); }

std::string format(const Layout& l)
{
    std::string t = "src=";
    for (int j = 0; j < kJacks; ++j)
        t += std::string(j ? "," : "") + kShort[static_cast<int>(l.source[static_cast<std::size_t>(j)])];
    for (int c = 0; c < kMaxChannels; ++c)
    {
        const auto d = l.dest[static_cast<std::size_t>(c)];
        if (d == kMain || d >= kDests)
            continue;
        std::string name = destName(d);
        t += " " + std::to_string(c + 1) + "=" + name + ((l.alsoMain >> c) & 1 ? "+" : "");
    }
    return t;
}

Layout parse(std::string_view text)
{
    Layout l;
    while (! text.empty())
    {
        const auto space = text.find_first_of(" \t;");
        const std::string_view field = text.substr(0, space);
        text = space == std::string_view::npos ? std::string_view() : text.substr(space + 1);
        const auto eq = field.find('=');
        if (eq == std::string_view::npos)
            continue;
        const std::string_view key = field.substr(0, eq);
        std::string_view value = field.substr(eq + 1);
        if (key == "src")
        {
            for (int j = 0; j < kJacks && ! value.empty(); ++j)
            {
                const auto comma = value.find(',');
                const std::string_view s = value.substr(0, comma);
                for (int k = 0; k < static_cast<int>(Source::Count); ++k)
                    if (s == kShort[k])
                        l.source[static_cast<std::size_t>(j)] = static_cast<Source>(k);
                value = comma == std::string_view::npos ? std::string_view() : value.substr(comma + 1);
            }
            continue;
        }
        const int channel = std::atoi(std::string(key).c_str()) - 1;
        if (channel < 0 || channel >= kMaxChannels)
            continue;
        const bool plus = ! value.empty() && value.back() == '+';
        if (plus)
            value.remove_suffix(1);
        l.dest[static_cast<std::size_t>(channel)] = parseDest(value);
        if (plus && l.dest[static_cast<std::size_t>(channel)] != kMain)
            l.alsoMain |= 1ull << channel;
    }
    return l;
}

std::uint8_t tapJack(std::uint8_t dest, int jack0) noexcept
{
    if (jack0 < 0 || jack0 >= kJacks)
        return dest;
    const int partner = jack0 ^ 1;
    if (isMono(dest) && monoJack(dest) == jack0)
        return kMain;
    if (isMono(dest) && monoJack(dest) == partner)
        return pair(jack0 / 2);
    if (isPair(dest) && pairIndex(dest) == jack0 / 2)
        return mono(partner);
    return mono(jack0);
}
} // namespace app::outs
