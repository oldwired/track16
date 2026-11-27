#include "app/Screen.h"

#include "app/Log.h"
#include "app/Theme.h"

#include "ProjectIdentity.h"
#include "controls/ControlNames.h"

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace app
{
namespace
{
namespace c = theme::colour;
namespace L = theme::layout;
using theme::Align;
using theme::Text;

const juce::String kChevron = juce::String(juce::CharPointer_UTF8("\xE2\x80\xBA")); // ›
const juce::String kDot = juce::String(juce::CharPointer_UTF8("\xC2\xB7"));        // ·

// The body (canvas lpx) and the rain area (LCD px).
const juce::Rectangle<float> kBody {0, L::bodyY + 1, theme::kCanvasW, L::stripY - L::bodyY - 1};
constexpr float kRainX = 8, kRainY = 96, kRainW = 784, kRainH = 284; // inside the body: 152…606 lpx

std::uint64_t nowNs() noexcept
{
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

template <typename T>
bool assign(T& field, const T& v)
{
    if (field == v)
        return false;
    field = v;
    return true;
}

inline std::uint64_t bit(int ch) noexcept { return ch >= 0 && ch < 64 ? (1ull << ch) : 0ull; }

// ARGB with alpha 0…1 (the scene premultiplies).
inline std::uint32_t argb(std::uint32_t rgb, float alpha) noexcept
{
    const auto a = static_cast<std::uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    return (a << 24) | (rgb & 0xFFFFFF);
}
inline std::uint32_t mix(std::uint32_t a, std::uint32_t b, float t) noexcept // rgb a → b
{
    t = std::clamp(t, 0.0f, 1.0f);
    auto ch = [&](int s) {
        const float x = static_cast<float>((a >> s) & 255), y = static_cast<float>((b >> s) & 255);
        return static_cast<std::uint32_t>(x + (y - x) * t + 0.5f) << s;
    };
    return ch(16) | ch(8) | ch(0);
}

constexpr std::uint32_t kRain = 0x3CDC78, kMuted = 0x5A5F66, kFiltered = 0xFF7A3C, kWhite = 0xFFFFFF;
// The rain's fields (tracker style): note, note off, volume, effect, empty dots; the gutter's row numbers; separators
// and the playhead band (opaque: the background is #0B0D10).
constexpr std::uint32_t kNote = 0xE4FFEC, kNoteOff = 0x8A9A90, kVol = 0x5FD08C, kFx = 0xFFB65C, kEmpty = 0x34463C;
constexpr std::uint32_t kGutter = 0x56645C, kGutterBeat = 0x8FA898, kSeparator = 0x1A231E, kBand = 0x15291D;

std::uint32_t instrumentColour(int instrument1) // 1-based
{
    return instrument1 > 0 ? theme::kTrack[(instrument1 - 1) & 15].base & 0xFFFFFF : kRain;
}

juce::String noteName(int note) // 0…119, 60 = C-5
{
    static const char* const names[] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
    return juce::String(names[note % 12]) + juce::String(note / 12);
}

juce::String sizeText(long bytes)
{
    char t[32];
    if (bytes >= 1024 * 1024)
        std::snprintf(t, sizeof t, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else
        std::snprintf(t, sizeof t, "%ld KB", (bytes + 1023) / 1024);
    return t;
}
} // namespace

// ---- the canvas -----------------------------------------------------------------------------------------------------

struct Screen::Canvas final : juce::Component
{
    bool rainHole = false; // the player view: the GL scene draws the rain area
    void paint(juce::Graphics& g) override
    {
        if (rainHole)
            g.excludeClipRegion(juce::Rectangle<float>(kRainX, kRainY, kRainW, kRainH).transformedBy(
                                                         juce::AffineTransform::scale(1.0f / theme::kScale)).toNearestInt());
        g.fillAll(c::bg);
    }
};

// ---- status row -----------------------------------------------------------------------------------------------------

struct Screen::StatusRow final : juce::Component
{
    juce::String where, event, diag;
    bool playing = false, litIn = false, litOut = false, filter = false, notes = false;
    double midiInAt = -1e9;

    // Body size (15 lpx, ≈ 9 px on the panel): read from a standing position. Right to left: the play state, the
    // NOTES / FILTER / OUT / IN flags, the load; the last event in the room left between.
    void paint(juce::Graphics& g) override
    {
        const float cy = L::statusH / 2;
        const juce::String app = juce::String(identity::displayName).toUpperCase();
        theme::text(g, 16, cy, Align::Left, Text::BodyStrong, c::text2, app);
        float x = 16 + theme::textWidth(Text::BodyStrong, app) + 8;
        theme::text(g, x, cy, Align::Left, Text::BodyStrong, c::text3, kChevron);
        x += theme::textWidth(Text::BodyStrong, kChevron) + 8;
        theme::text(g, x, cy, Align::Left, Text::BodyStrong, c::text1, where);
        x += theme::textWidth(Text::BodyStrong, where) + 32;
        float ix = 1264;
        theme::glyph(g, theme::Glyph::Play, ix - 12, cy - 7, 14, playing ? c::play : c::text3);
        ix -= 32;
        const auto flagText = [&](const char* text, bool lit, juce::Colour ink) {
            theme::text(g, ix, cy, Align::Right, Text::BodyStrong, lit ? ink : c::text3, text);
            ix -= theme::textWidth(Text::BodyStrong, text) + 20;
        };
        flagText("NOTES", notes, c::rec);
        flagText("FILTER", filter, c::warn);
        flagText("OUT", litOut, c::accent);
        flagText("IN", litIn, c::accent);
        ix -= 12;
        theme::text(g, ix, cy, Align::Right, Text::Body, c::text3, diag);
        ix -= theme::textWidth(Text::Body, diag) + 32;
        juce::Graphics::ScopedSaveState s(g);
        g.reduceClipRegion(juce::Rectangle<float>(x, 0, std::max(0.0f, ix - x), L::statusH).toNearestInt());
        theme::text(g, std::max(x, 300.0f), cy, Align::Left, Text::Body, c::text1, event);
    }
    void set(juce::String& field, const juce::String& v)
    {
        if (assign(field, v))
            repaint();
    }
    template <typename T>
    void flag(T& field, const T& v)
    {
        if (assign(field, v))
            repaint();
    }
};

// ---- context bar ----------------------------------------------------------------------------------------------------

struct Screen::ContextBar final : juce::Component
{
    juce::String order, row, speed, bpm, title, info;
    std::array<juce::String, 4> modeName {"FILTER", "MUTE", "LOOP", "PLAY"}, modeSub;
    int mode = 1;
    bool queued = false, externalClock = false; // externalClock: BPM is the MIDI clock's (caption EXT BPM)

    void paint(juce::Graphics& g) override
    {
        g.addTransform(juce::AffineTransform::translation(0, -L::contextY));
        g.setColour(c::surface1);
        g.fillRect(0.0f, L::contextY, theme::kCanvasW, L::contextH);
        theme::hline(g, 0, theme::kCanvasW, L::contextY + L::contextH, c::border);
        g.setColour(c::surface2);
        g.fillRoundedRectangle(L::slotA, theme::radiusM);
        theme::glyph(g, theme::Glyph::Back, L::slotA.getX() + 34, L::slotA.getY() + 22, 24, c::text1);
        split(g, L::slotB, "ORDER", order, queued ? c::warn : c::text1, "ROW", row);
        split(g, L::slotC, "SPEED", speed, c::text1, externalClock ? "EXT BPM" : "BPM", bpm);
        // The readout: title and info, in the space before the mode buttons.
        {
            juce::Graphics::ScopedSaveState s(g);
            g.reduceClipRegion(juce::Rectangle<float>(L::readoutX, L::contextY, L::slotE.getX() - L::readoutX - 8, L::contextH).toNearestInt());
            theme::text(g, L::readoutX, L::captionY, Align::Left, Text::Caption, c::text2, info);
            theme::text(g, L::readoutX, L::valueY, Align::Left, Text::BodyStrong, c::text1, title);
        }
        for (int i = 0; i < 4; ++i)
        {
            const auto r = L::menuButton(i);
            const bool sel = i == std::min(mode, 3);
            g.setColour(sel ? c::accentWeak : c::surface2);
            g.fillRoundedRectangle(r, theme::radiusM);
            if (sel)
            {
                g.setColour(c::accent);
                g.fillRoundedRectangle(r.getX() + 16, r.getY(), r.getWidth() - 32, 3, 1);
            }
            theme::text(g, r.getCentreX(), r.getY() + 26, Align::Centre, Text::ValueS, sel ? c::accent : c::text1, modeName[static_cast<std::size_t>(i)]);
            theme::text(g, r.getCentreX(), r.getY() + 56, Align::Centre, Text::Caption, sel ? c::accent : c::text2, modeSub[static_cast<std::size_t>(i)]);
        }
    }
    static void split(juce::Graphics& g, juce::Rectangle<float> r, const char* capA, const juce::String& valA, juce::Colour inkA,
                      const char* capB, const juce::String& valB)
    {
        g.setColour(c::surface2);
        g.fillRoundedRectangle(r, theme::radiusM);
        theme::vline(g, r.getCentreX(), r.getY() + 12, r.getY() + 60, c::borderStrong);
        for (int i = 0; i < 2; ++i)
        {
            const float cx = r.getX() + r.getWidth() / 4 + r.getWidth() / 2 * static_cast<float>(i);
            theme::text(g, cx, L::captionY, Align::Centre, Text::Caption, c::text2, i == 0 ? capA : capB);
            theme::text(g, cx, L::valueY, Align::Centre, Text::Value, i == 0 ? inkA : c::text1, i == 0 ? valA : valB);
        }
    }
    template <typename T>
    void set(T& field, const T& v)
    {
        if (assign(field, v))
            repaint();
    }
};

// ---- the library browser --------------------------------------------------------------------------------------------

struct Screen::Browser final : juce::Component
{
    const Library* library = nullptr;
    int selected = 0, top = 0, playing = -1;
    std::vector<ModuleInfo> info; // per entry, as the prober finds it (channels 0: not yet)
    static constexpr float kRowH = 60, kHeadH = 52; // large: read on a 7" screen from a standing position
    juce::String root;

    int visibleRows() const { return static_cast<int>((static_cast<float>(getHeight()) - kHeadH - 8) / kRowH); }
    void select(int i)
    {
        const int n = library != nullptr ? static_cast<int>(library->entries().size()) : 0;
        if (n == 0)
            return;
        selected = std::clamp(i, 0, n - 1);
        const int rows = std::max(1, visibleRows());
        if (selected < top)
            top = selected;
        if (selected >= top + rows)
            top = selected - rows + 1;
        repaint();
    }
    int rowAt(juce::Point<float> p) const // local lpx → entry index, −1 = none
    {
        if (p.y < kHeadH)
            return -1;
        const int i = top + static_cast<int>((p.y - kHeadH) / kRowH);
        return library != nullptr && i < static_cast<int>(library->entries().size()) ? i : -1;
    }
    void paint(juce::Graphics& g) override
    {
        g.fillAll(c::bg);
        const auto& list = library->entries();
        const float w = static_cast<float>(getWidth());
        theme::text(g, 16, kHeadH / 2, Align::Left, Text::Title, c::text1, "LIBRARY");
        theme::text(g, 140, kHeadH / 2, Align::Left, Text::Body, c::text2,
                    juce::String(static_cast<int>(list.size())) + " MODULES  " + kDot + "  " + root);
        theme::text(g, w - 16, kHeadH / 2, Align::Right, Text::Label, c::text3,
                    "SELECT: CHOOSE  " + kDot + "  PUSH: LOAD  " + kDot + "  PLAY: START  " + kDot + "  BACK: PLAYER");
        if (list.empty())
        {
            theme::text(g, w / 2, 200, Align::Centre, Text::Body, c::text2, "No modules found on the stick.");
            theme::text(g, w / 2, 240, Align::Centre, Text::Caption, c::text3,
                        "Copy .mod, .s3m, .xm, .it (and the other libopenmpt formats) to the stick, in any folder.");
            return;
        }
        const int rows = visibleRows();
        for (int k = 0; k < rows && top + k < static_cast<int>(list.size()); ++k)
        {
            const int i = top + k;
            const auto& e = list[static_cast<std::size_t>(i)];
            const juce::Rectangle<float> r(8, kHeadH + kRowH * static_cast<float>(k), w - 16, kRowH - 4);
            if (! g.clipRegionIntersects(r.toNearestInt()))
                continue;
            if (i == selected)
            {
                g.setColour(c::accentWeak);
                g.fillRoundedRectangle(r, theme::radiusS);
                g.setColour(c::accent);
                g.fillRoundedRectangle(r.getX(), r.getY() + 8, 3, r.getHeight() - 16, 1);
            }
            else if (k % 2 == 1)
            {
                g.setColour(c::surface1);
                g.fillRoundedRectangle(r, theme::radiusS);
            }
            if (i == playing)
                theme::glyph(g, theme::Glyph::Play, r.getX() + 16, r.getCentreY() - 9, 18, c::play);
            float x = r.getX() + 48;
            if (! e.folder.empty())
            {
                const juce::String folder = juce::String::fromUTF8(e.folder.c_str()) + " /";
                theme::text(g, x, r.getCentreY(), Align::Left, Text::ValueS, c::text3, folder);
                x += theme::textWidth(Text::ValueS, folder) + 12;
            }
            const juce::String name = juce::String::fromUTF8(e.name.c_str());
            theme::text(g, x, r.getCentreY(), Align::Left, Text::Value, i == selected ? c::text1 : c::text2, name);
            x += theme::textWidth(Text::Value, name) + 16;
            // Right: format, channels, length (once probed), size; the title between, where it fits.
            float right = r.getRight() - 16;
            theme::text(g, right, r.getCentreY(), Align::Right, Text::Body, c::text3, sizeText(e.size));
            right -= theme::textWidth(Text::Body, sizeText(e.size)) + 24;
            const ModuleInfo* m = static_cast<std::size_t>(i) < info.size() && info[static_cast<std::size_t>(i)].channels > 0
                                      ? &info[static_cast<std::size_t>(i)] : nullptr;
            if (m != nullptr)
            {
                const int secs = static_cast<int>(m->seconds + 0.5);
                char facts[64];
                std::snprintf(facts, sizeof facts, "%s  %s  %d CH  %s  %d:%02d", juce::String(m->type).toUpperCase().toRawUTF8(),
                              kDot.toRawUTF8(), m->channels, kDot.toRawUTF8(), secs / 60, secs % 60);
                const juce::String f = juce::String::fromUTF8(facts);
                theme::text(g, right, r.getCentreY(), Align::Right, Text::BodyStrong, i == selected ? c::text1 : c::text2, f);
                right -= theme::textWidth(Text::BodyStrong, f) + 24;
                juce::String title = juce::String::fromUTF8(m->title.c_str()).trim();
                if (title.isNotEmpty() && ! title.equalsIgnoreCase(name) && ! title.equalsIgnoreCase(name.upToLastOccurrenceOf(".", false, false)))
                {
                    const juce::String ellipsis = juce::String(juce::CharPointer_UTF8("\xE2\x80\xA6"));
                    juce::String shown = title;
                    while (shown.length() > 3 && x + theme::textWidth(Text::Body, shown == title ? shown : shown + ellipsis) > right)
                        shown = shown.dropLastCharacters(1).trimEnd();
                    if (x + theme::textWidth(Text::Body, shown == title ? shown : shown + ellipsis) <= right)
                        theme::text(g, x, r.getCentreY(), Align::Left, Text::Body, c::text3, shown == title ? shown : shown + ellipsis);
                }
            }
        }
        // The scroll position.
        if (static_cast<int>(list.size()) > rows)
        {
            const float trackY = kHeadH, trackH = static_cast<float>(rows) * kRowH;
            const float h = std::max(24.0f, trackH * static_cast<float>(rows) / static_cast<float>(list.size()));
            const float y = trackY + (trackH - h) * static_cast<float>(top) / static_cast<float>(list.size() - static_cast<std::size_t>(rows));
            g.setColour(c::borderStrong);
            g.fillRoundedRectangle(w - 6, y, 3, h, 1.5f);
        }
    }
};

// ---- the knob strip -------------------------------------------------------------------------------------------------

struct Screen::KnobStrip final : juce::Component
{
    static constexpr const char* captions[6] = {"TEMPO", "PITCH", "STEREO", "INTERP", "GAIN", "VIEW"};
    std::array<float, 6> value {};
    std::array<juce::String, 6> text {};
    std::array<double, 6> touchedAt {-1e9, -1e9, -1e9, -1e9, -1e9, -1e9};
    std::array<bool, 6> underlined {};
    juce::String tab = "PLAYER";

    juce::Rectangle<int> column(int n) const
    {
        return juce::Rectangle<float>(L::columnX0 + L::columnW * static_cast<float>(n), L::tabH, L::columnW, L::stripH - L::tabH).toNearestInt();
    }
    void set(int n, float v, const juce::String& t)
    {
        const auto i = static_cast<std::size_t>(n);
        if (std::fabs(value[i] - v) < 1e-4f && text[i] == t)
            return;
        value[i] = v;
        text[i] = t;
        repaint(column(n));
    }
    void frame(double nowMs)
    {
        for (int n = 0; n < 6; ++n)
            if (assign(underlined[static_cast<std::size_t>(n)], nowMs - touchedAt[static_cast<std::size_t>(n)] < theme::recentlyTouchedMs))
                repaint(column(n));
    }
    void paint(juce::Graphics& g) override
    {
        g.addTransform(juce::AffineTransform::translation(0, -L::stripY));
        g.setColour(c::surface1);
        g.fillRect(0.0f, L::stripY, theme::kCanvasW, L::stripH);
        theme::hline(g, 0, theme::kCanvasW, L::stripY, c::border);
        juce::Path tabPath;
        tabPath.addQuadrilateral(0, L::stripY, L::tabTopW, L::stripY, L::tabW, L::stripY + L::tabH, 0, L::stripY + L::tabH);
        g.setColour(c::surface2);
        g.fillPath(tabPath);
        theme::text(g, 16, L::stripY + 13, Align::Left, Text::Label, c::text2, tab);
        for (int n = 0; n < 6; ++n)
        {
            const auto i = static_cast<std::size_t>(n);
            const float x = L::columnX0 + L::columnW * static_cast<float>(n), cx = x + L::columnW / 2;
            theme::text(g, cx, L::columnCaptionY, Align::Centre, Text::Title, c::text2, captions[n]);
            theme::text(g, cx, L::columnValueY, Align::Centre, Text::ValueL, c::text1, text[i]);
            if (underlined[i])
            {
                g.setColour(c::accent);
                g.fillRoundedRectangle(x + 56, L::columnUnderlineY, 96, 3, 1.5f);
            }
            g.setColour(c::borderStrong);
            g.fillRoundedRectangle(x + 32, L::columnTrackY, 144, 3, 1.5f);
            g.setColour(c::text2);
            g.fillRoundedRectangle(x + 32, L::columnTrackY, std::max(3.0f, 144.0f * std::clamp(value[i], 0.0f, 1.0f)), 3, 1.5f);
        }
    }
};

// ---- the OUTS page --------------------------------------------------------------------------------------------------
// In the OUTS pad mode the body shows where the pads' 16 channels play: MAIN, the jacks OUT 3–8 (a stereo pair as one
// bar over both jacks), +MAIN; above each jack what it carries (tap: CH → CLEAN → RETURN → CLICK) and its level.

namespace
{
constexpr std::uint32_t kGroupRgb[3] = {0x4C7DFF, 0x3CDC78, 0xFFB020}; // 3-4 blue, 5-6 green, 7-8 amber (the pads' colours)
juce::Colour groupColour(int group, float alpha = 1.0f) { return juce::Colour(0xFF000000 | kGroupRgb[group % 3]).withAlpha(alpha); }
} // namespace

struct Screen::OutsPage final : juce::Component
{
    static constexpr float kHeadH = 36, kJackY = 38, kJackH = 66, kRowY = 108, kRowH = 23; // as large as 16 rows allow
    static constexpr float kNameX = 16, kMainX = 312, kColW = 104, kAlsoX = 1168, kAlsoW = 96;
    static float jackX(int j) { return 440 + 112 * static_cast<float>(j) + 20 * static_cast<float>(j / 2); }

    const outs::Layout* layout = nullptr;
    std::shared_ptr<const SongData> song;
    int page = 0, stemsUsed = 0, stemsTotal = 0, toneJack = -1;
    std::uint16_t unserved = 0;
    std::uint64_t muted = 0, held = 0;
    std::array<float, outs::kJacks> level {};

    // A tap: the row (channel on this page, −1 = none) and the column (−1 MAIN, 0–5 a jack, 6 +MAIN, −2 none);
    // jackSource: a tap on a jack's header (−1 = none).
    void hit(juce::Point<float> p, int& row, int& column, int& jackSource) const
    {
        row = -1, column = -2, jackSource = -1;
        for (int j = 0; j < outs::kJacks; ++j)
            if (p.x >= jackX(j) && p.x < jackX(j) + kColW)
            {
                column = j;
                if (p.y >= kJackY && p.y < kJackY + kJackH)
                    jackSource = j;
            }
        if (p.x >= kMainX && p.x < kMainX + kColW + 16)
            column = -1;
        if (p.x >= kAlsoX && p.x < kAlsoX + kAlsoW)
            column = 6;
        if (p.y >= kRowY && p.y < kRowY + 16 * kRowH)
            row = static_cast<int>((p.y - kRowY) / kRowH);
    }
    void paint(juce::Graphics& g) override
    {
        g.fillAll(c::bg);
        const float w = static_cast<float>(getWidth());
        theme::text(g, 16, kHeadH / 2, Align::Left, Text::Title, c::text1, "OUTS");
        juce::String pool = "INSTANCES " + juce::String(stemsUsed) + "/" + juce::String(stemsTotal);
        if (unserved != 0)
            pool += "  " + kDot + "  POOL FULL: " + juce::String(__builtin_popcount(unserved)) + " NOT ROUTED (out_stems)";
        theme::text(g, 100, kHeadH / 2, Align::Left, Text::Body, unserved != 0 ? c::warn : c::text2, pool);
        theme::text(g, w - 16, kHeadH / 2, Align::Right, Text::Label, c::text3,
                    "PADS + STEP: ROUTE  " + kDot + "  SHIFT+PAD: +MAIN  " + kDot + "  STEP: SOURCE  " + kDot + "  SHIFT+STEP: TONE");
        if (layout == nullptr)
            return;
        // The jacks: name, source, level.
        theme::text(g, kMainX + kColW / 2, kJackY + kJackH / 2, Align::Centre, Text::Title, c::text2, "MAIN");
        theme::text(g, kAlsoX + kAlsoW / 2, kJackY + kJackH / 2, Align::Centre, Text::Title, c::text2, "+MAIN");
        for (int j = 0; j < outs::kJacks; ++j)
        {
            const float x = jackX(j);
            const auto src = layout->source[static_cast<std::size_t>(j)];
            const bool testing = toneJack == j;
            g.setColour(testing ? c::accentWeak : c::surface2);
            g.fillRoundedRectangle(x, kJackY, kColW, kJackH, theme::radiusM);
            theme::text(g, x + kColW / 2, kJackY + 15, Align::Centre, Text::Label, testing ? c::accent : c::text2,
                        "OUT " + juce::String(j + 3));
            theme::text(g, x + kColW / 2, kJackY + 39, Align::Centre, Text::Title,
                        src == outs::Source::Channels ? groupColour(j / 2) : c::text1, outs::sourceName(src));
            const float lv = std::clamp(std::sqrt(level[static_cast<std::size_t>(j)]), 0.0f, 1.0f);
            g.setColour(c::borderStrong);
            g.fillRoundedRectangle(x + 12, kJackY + kJackH - 8, kColW - 24, 3, 1.5f);
            g.setColour(lv > 0.95f ? c::warn : c::text1);
            g.fillRoundedRectangle(x + 12, kJackY + kJackH - 8, (kColW - 24) * lv, 3, 1.5f);
        }
        if (! song)
            return;
        const int channels = std::min(song->channels, kMaxChannels);
        for (int r = 0; r < 16; ++r)
        {
            const int ch = page * 16 + r;
            if (ch >= channels)
                break;
            const float y = kRowY + kRowH * static_cast<float>(r), cy = y + kRowH / 2;
            const bool isHeld = (held >> r) & 1;
            if (isHeld || r % 2 == 1)
            {
                g.setColour(isHeld ? c::accentWeak : c::surface1);
                g.fillRoundedRectangle(8, y + 1, w - 16, kRowH - 2, theme::radiusS);
            }
            const auto d = layout->dest[static_cast<std::size_t>(ch)];
            const bool also = (layout->alsoMain >> ch) & 1;
            const bool isMuted = (muted >> ch) & 1;
            juce::String name = juce::String(ch + 1).paddedLeft('0', 2);
            if (ch < static_cast<int>(song->channelNames.size()) && ! song->channelNames[static_cast<std::size_t>(ch)].empty())
                name += "  " + juce::String::fromUTF8(song->channelNames[static_cast<std::size_t>(ch)].c_str());
            {
                juce::Graphics::ScopedSaveState clip(g);
                g.reduceClipRegion(juce::Rectangle<float>(kNameX, y, kMainX - kNameX - 8, kRowH).toNearestInt());
                theme::text(g, kNameX + 8, cy, Align::Left, Text::Title, isMuted ? c::text3 : theme::track(r + 1), name);
            }
            // MAIN
            if (d == outs::kMain || also)
            {
                g.setColour(c::text1.withAlpha(isMuted ? 0.3f : 0.85f));
                g.fillRoundedRectangle(kMainX + 8, y + 5, kColW - 16, kRowH - 10, 4);
            }
            else
            {
                g.setColour(c::borderStrong);
                g.drawRoundedRectangle(kMainX + 8, y + 5, kColW - 16, kRowH - 10, 4, 1);
            }
            // The jacks: a free cell outlined, the destination filled (a pair as one bar); a jack with a utility
            // signal takes no channels (dimmed); not routed (pool full) in the warning colour.
            for (int j = 0; j < outs::kJacks; ++j)
                if (layout->source[static_cast<std::size_t>(j)] == outs::Source::Channels && ! (outs::jackMask(d) & (1u << j)))
                {
                    g.setColour(c::border);
                    g.drawRoundedRectangle(jackX(j) + 8, y + 5, kColW - 16, kRowH - 10, 4, 1);
                }
            if (d != outs::kMain)
            {
                const unsigned m = outs::jackMask(d);
                const int j0 = __builtin_ctz(m), j1 = 31 - __builtin_clz(m);
                const float x0 = jackX(j0) + 8, x1 = jackX(j1) + kColW - 8;
                const bool notRouted = (unserved >> d) & 1;
                const auto ink = notRouted ? c::warn : groupColour(outs::group(d), isMuted ? 0.3f : 0.9f);
                g.setColour(ink);
                if (notRouted)
                    g.drawRoundedRectangle(x0, y + 5, x1 - x0, kRowH - 10, 4, 2);
                else
                    g.fillRoundedRectangle(x0, y + 5, x1 - x0, kRowH - 10, 4);
                if (outs::isPair(d))
                    theme::text(g, (x0 + x1) / 2, cy, Align::Centre, Text::Label, notRouted ? c::warn : c::bg, "STEREO");
            }
            // +MAIN
            g.setColour(also ? c::text1 : c::border);
            if (also)
                g.fillEllipse(kAlsoX + kAlsoW / 2 - 6, cy - 6, 12, 12);
            else if (d != outs::kMain)
                g.drawEllipse(kAlsoX + kAlsoW / 2 - 6, cy - 6, 12, 12, 1);
        }
    }
};

// ---- the screen -----------------------------------------------------------------------------------------------------

Screen::Screen(Player& player, Midi& midi, ScreenConfig config) : player_(player), midi_(midi), config_(std::move(config))
{
    setOpaque(false);
    setLookAndFeel(&theme::lookAndFeel());
    canvas_ = std::make_unique<Canvas>();
    canvas_->setInterceptsMouseClicks(false, false);
    canvas_->setBounds(0, 0, static_cast<int>(theme::kCanvasW), static_cast<int>(theme::kCanvasH));
    canvas_->setTransform(juce::AffineTransform::scale(theme::kScale));
    addAndMakeVisible(*canvas_);
    auto add = [this](auto* part, juce::Rectangle<float> bounds) {
        part->setInterceptsMouseClicks(false, false);
        part->setBounds(bounds.toNearestInt());
        canvas_->addAndMakeVisible(part);
        return part;
    };
    status_ = add(new StatusRow(), {0, 0, theme::kCanvasW, L::statusH});
    context_ = add(new ContextBar(), {0, L::contextY, theme::kCanvasW, L::contextH + 1});
    browser_ = add(new Browser(), kBody);
    knobs_ = add(new KnobStrip(), {0, L::stripY, theme::kCanvasW, L::stripH});
    outsPage_ = add(new OutsPage(), kBody);
    outsPage_->layout = &layout_;
    outsPage_->setVisible(false);
    browser_->library = &library_;
    rects_.reserve(4096);
    overlay_.reserve(256);
    glyphs_.reserve(16384);
    padHitMs_.fill(-1e9);
    setView(View::Library);
    setSize(kWidth, kHeight);
    loader_ = std::thread([this] { loaderThread(); });
}

Screen::~Screen()
{
    {
        std::lock_guard<std::mutex> lock(loadMutex_);
        loadQuit_ = true;
    }
    loadWake_.notify_all();
    if (loader_.joinable())
        loader_.join();
    probeQuit_ = true;
    if (prober_.joinable())
        prober_.join();
    if (layoutDirty_)
        saveLayout();
    if (beatsDirty_)
        saveBeats();
    canvas_->deleteAllChildren();
    setLookAndFeel(nullptr);
}

void Screen::setStatus(const Status& s)
{
    statusBase_ = juce::String::fromUTF8((s.platform + " · " + s.renderer).c_str()).toUpperCase();
    status_->set(status_->diag, statusBase_);
}

void Screen::event(const juce::String& text) { status_->set(status_->event, text.toUpperCase()); }

void Screen::start()
{
    loadBeats();
    library_.scan(config_.roots);
    browser_->info.assign(library_.entries().size(), {});
    {
        std::vector<std::string> paths;
        for (const auto& e : library_.entries())
            paths.push_back(e.path);
        prober_ = std::thread([this, paths = std::move(paths)]() mutable { proberThread(std::move(paths)); });
    }
    browser_->root = juce::String::fromUTF8(library_.root().c_str());
    browser_->select(0);
    logLine("LIBRARY %d modules in %s", static_cast<int>(library_.entries().size()), library_.root().c_str());
    if (config_.autoplay && ! library_.entries().empty())
        loadEntry(0, true);
}

void Screen::setView(View v)
{
    view_ = v;
    browser_->setVisible(v == View::Library);
    canvas_->rainHole = v == View::Player;
    canvas_->repaint();
    status_->set(status_->where, v == View::Player ? juce::String("PLAYER") : juce::String("LIBRARY"));
}

void Screen::loadEntry(int index, bool play)
{
    const auto& list = library_.entries();
    if (index < 0 || index >= static_cast<int>(list.size()))
        return;
    {
        std::lock_guard<std::mutex> lock(loadMutex_);
        loadJob_ = {index, list[static_cast<std::size_t>(index)].path, play, player_.stems(), player_.sampleRate(), config_.outStems};
        loadWanted_ = true; // a newer request replaces one that has not started
    }
    loadWake_.notify_all();
    browser_->select(index);
    event("LOADING " + juce::String::fromUTF8(list[static_cast<std::size_t>(index)].name.c_str()));
}

void Screen::loaderThread()
{
    nameThisThread("loader");
    std::unique_lock<std::mutex> lock(loadMutex_);
    for (;;)
    {
        loadWake_.wait(lock, [this] { return loadQuit_ || loadWanted_; });
        if (loadQuit_)
            return;
        auto result = std::make_unique<Loaded>();
        result->job = loadJob_;
        loadWanted_ = false;
        lock.unlock();
        const double t0 = nowMs();
        result->song = loadSong(result->job.path, result->job.stems, result->job.outStems, result->job.sampleRate,
                                config_.stereoSeparation, config_.interpolation, result->error);
        result->ms = nowMs() - t0;
        lock.lock();
        loaded_ = std::move(result); // replaces a result frame() has not taken yet (a newer pick wins)
    }
}

// The loader's result (message thread, once per frame).
void Screen::takeLoaded()
{
    std::unique_ptr<Loaded> r;
    {
        std::lock_guard<std::mutex> lock(loadMutex_);
        r = std::move(loaded_);
    }
    if (! r)
        return;
    if (! r->song)
    {
        event("LOAD FAILED: " + juce::String::fromUTF8(r->error.c_str()));
        logLine("LOAD FAILED %s: %s", r->job.path.c_str(), r->error.c_str());
        return;
    }
    const auto& d = *r->song->data;
    logLine("LOAD %s \"%s\" %s, %d ch, %d orders, %d patterns, %d instruments, %.0f s, instances=%d (filter %d, outs %d) (%.0f ms)",
            d.path.c_str(), d.title.c_str(), d.type.c_str(), d.channels, d.numOrders, d.numPatterns, std::max(d.numInstruments, d.numSamples),
            d.durationSeconds, r->song->instances(), r->song->wet ? 1 : 0, static_cast<int>(r->song->outs.size()), r->ms);
    event("LOADED " + juce::String::fromUTF8(d.title.c_str()) + (r->job.play ? "" : "  " + kDot + "  PLAY STARTS IT"));
    r->song->playOnAdopt = r->job.play;
    pending_ = std::move(r->song); // replaces one the player has not taken yet
    playingIndex_ = r->job.index;
    browser_->playing = r->job.index;
    if (r->job.index >= 0 && static_cast<std::size_t>(r->job.index) < browser_->info.size())
        browser_->info[static_cast<std::size_t>(r->job.index)] = {d.type, d.title, d.channels, d.durationSeconds};
    mutes_ = solos_ = 0;
    player_.muteMask = 0;
    player_.soloMask = 0;
    player_.page = 0;
    player_.instrument = 0;
    orderPage_ = 0;
}

int Screen::pageCount() const
{
    if (! song_)
        return 1;
    const auto mode = static_cast<PadMode>(player_.padMode.load());
    const int n = mode == PadMode::Kit || mode == PadMode::Play ? std::max(song_->numInstruments, song_->numSamples) : song_->channels;
    return std::clamp((std::min(n, kMaxChannels) + 15) / 16, 1, 4);
}

void Screen::setPadMode(PadMode mode, bool samePressed)
{
    const auto now = static_cast<PadMode>(player_.padMode.load());
    if (now == mode && samePressed)
    {
        player_.page = (player_.page.load() + 1) % pageCount(); // the same mode again: the next page
        event("PADS PAGE " + juce::String(player_.page.load() + 1));
        return;
    }
    player_.padMode = static_cast<int>(mode);
    player_.page = 0;
    static const char* const names[] = {"FILTER", "MUTE", "LOOP", "PLAY", "KIT", "OUTS: HOLD PADS + STEP 1-8"};
    event(juce::String("PADS: ") + names[static_cast<int>(mode)]);
}

void Screen::padPress(int pad0, int velocity, bool fromReader, double nowMs)
{
    ++padHits_[static_cast<std::size_t>(pad0)]; // the player counted this hit in push() (the reader's, or ours below)
    padsHeld_ |= 1ull << pad0;
    padHitMs_[static_cast<std::size_t>(pad0)] = nowMs;
    if (! fromReader) // touch, scripts: no reader thread told the player
        player_.push({static_cast<std::uint8_t>(pad0), static_cast<std::uint8_t>(velocity), 60, static_cast<std::int8_t>(pad0)});
    const int index = player_.page.load() * 16 + pad0;
    switch (static_cast<PadMode>(player_.padMode.load()))
    {
    case PadMode::Mute:
        if (shift_)
            solos_ ^= bit(index);
        else
            mutes_ ^= bit(index);
        player_.muteMask = mutes_;
        player_.soloMask = solos_;
        break;
    case PadMode::Filter:
        if (shift_)
            player_.toggleFilterLatch(index);
        if (! player_.stems())
            event("FILTER NEEDS THE FILTER LOOP (filter_loop=1)");
        break;
    case PadMode::Loop:
        if (shift_)
            player_.toggleLoopLatch();
        break;
    case PadMode::Play:
        if (shift_)
        {
            player_.instrument = index;
            if (song_ && index < static_cast<int>(song_->instrumentNames.size()))
                event("INSTRUMENT " + juce::String(index + 1) + " " + juce::String::fromUTF8(song_->instrumentNames[static_cast<std::size_t>(index)].c_str()));
        }
        break;
    case PadMode::Kit:
        if (song_ && index < static_cast<int>(song_->instrumentNames.size()))
            event("KIT " + juce::String(index + 1) + " " + juce::String::fromUTF8(song_->instrumentNames[static_cast<std::size_t>(index)].c_str()));
        break;
    case PadMode::Outs:
        if (! song_ || index >= std::min(song_->channels, kMaxChannels))
            break;
        if (shift_)
        {
            if (layout_.dest[static_cast<std::size_t>(index)] == outs::kMain)
                event("CH " + juce::String(index + 1) + " IS ON MAIN: SEND IT TO A JACK FIRST (HOLD PAD + STEP 3-8)");
            else
            {
                layout_.alsoMain ^= bit(index);
                changeLayout("CH " + juce::String(index + 1) + ((layout_.alsoMain & bit(index)) ? " ALSO ON MAIN" : " OFF MAIN"), nowMs);
            }
        }
        else
            event("CH " + juce::String(index + 1) + " ON " + juce::String(outs::destName(layout_.dest[static_cast<std::size_t>(index)]))
                  + ((layout_.alsoMain & bit(index)) ? " +MAIN" : "") + kDot + " STEP 1 MAIN, 3-8 JACK, TWO KEYS = PAIR");
        break;
    }
}

// ---- the single outs ----

bool Screen::outsVisible() const { return view_ == View::Player && static_cast<PadMode>(player_.padMode.load()) == PadMode::Outs; }

// ---- rows per beat ----------------------------------------------------------------------------------------------------
// How many rows a MIDI beat (24 clock pulses) is: the clock in and out, the beat quantize, the BPM shown. Auto: 4 rows,
// unless that puts the module's typical row (SongData::rowSeconds) above 200 BPM (then 8, 12) or below 60 (then 2, 1),
// so fast speed-3 modules come out at the tempo they sound like instead of twice it.
namespace
{
juce::String ratioText(int rpb)
{
    switch (rpb)
    {
    case 1: return juce::String::fromUTF8("\u00D7\u00BC");
    case 2: return juce::String::fromUTF8("\u00D7\u00BD");
    case 3: return juce::String::fromUTF8("\u00D7\u00BE");
    case 6: return juce::String::fromUTF8("\u00D71.5");
    default: return juce::String::fromUTF8("\u00D7") + juce::String(rpb / 4);
    }
}
} // namespace

int Screen::beatAuto(const SongData& d) const
{
    if (config_.rowsPerBeat > 0)
        return config_.rowsPerBeat;
    if (d.rowSeconds <= 0)
        return 4;
    const auto bpm = [&](int rpb) { return 60.0 / (d.rowSeconds * rpb); };
    int rpb = 4;
    for (int up : {8, 12})
        if (bpm(rpb) > 200)
            rpb = up;
    for (int down : {2, 1})
        if (bpm(rpb) < 60)
            rpb = down;
    return rpb;
}

void Screen::applyBeat(const SongData& d)
{
    const auto it = beats_.find(d.fileName);
    const int automatic = beatAuto(d), rpb = it != beats_.end() ? it->second : automatic;
    player_.rowsPerBeat = rpb;
    logLine("BEAT %s: %d rows per beat (%s), typical row %.1f BPM", d.fileName.c_str(), rpb,
            it != beats_.end() ? "saved" : config_.rowsPerBeat > 0 ? "rows_per_beat" : "auto",
            d.rowSeconds > 0 ? 60.0 / (d.rowSeconds * rpb) : 0.0);
}

void Screen::stepBeat(int dir, bool backToAuto)
{
    if (! song_)
        return;
    const int automatic = beatAuto(*song_);
    int rpb = player_.rowsPerBeat.load();
    if (backToAuto)
        rpb = automatic;
    else
    {
        const int n = static_cast<int>(std::size(Player::kRowsPerBeat));
        int i = 0;
        while (i < n - 1 && Player::kRowsPerBeat[i] < rpb)
            ++i;
        rpb = Player::kRowsPerBeat[std::clamp(i + (dir > 0 ? 1 : -1), 0, n - 1)];
    }
    player_.rowsPerBeat = rpb;
    if (rpb == automatic)
        beats_.erase(song_->fileName);
    else
        beats_[song_->fileName] = rpb;
    beatsDirty_ = true;
    beatsSaveMs_ = lastFrameMs_ + 1500; // written once the turns pause
    event("CLOCK " + ratioText(rpb) + ": " + juce::String(rpb) + (rpb == 1 ? " ROW" : " ROWS") + " PER BEAT"
          + (rpb == automatic ? " (AUTO)" : ""));
}

bool Screen::beatKnob() const
{
    return player_.syncState() != Player::SyncInternal || (player_.midiSync.load() && player_.clockBpm() > 0);
}

void Screen::loadBeats()
{
    beats_.clear();
    if (config_.dataDir.empty())
        return;
    std::ifstream in(config_.dataDir + "/rows_per_beat.txt");
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        const auto space = line.find(' ');
        if (space == std::string::npos)
            continue;
        const int rpb = std::atoi(line.substr(0, space).c_str());
        if (rpb == Player::validRowsPerBeat(rpb))
            beats_[line.substr(space + 1)] = rpb;
    }
}

void Screen::saveBeats()
{
    beatsDirty_ = false;
    if (config_.dataDir.empty())
        return;
    ::mkdir(config_.dataDir.c_str(), 0755);
    const std::string file = config_.dataDir + "/rows_per_beat.txt";
    std::ofstream out(file, std::ios::trunc);
    out << "# Track16: rows per MIDI beat chosen with TEMPO, per module (rows, a space, the file name), see the README\n";
    for (const auto& [name, rpb] : beats_)
        out << rpb << ' ' << name << '\n';
    if (! out)
        logLine("BEAT cannot write %s", file.c_str());
}

// The library's facts (format, channels, length, title) for every entry, one by one at the lowest priority, so the
// list fills in while it is shown; the screen takes them each frame (takeProbed).
void Screen::proberThread(std::vector<std::string> paths)
{
    ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 19);
    for (std::size_t i = 0; i < paths.size() && ! probeQuit_.load(); ++i)
    {
        ModuleInfo info;
        if (! probeModule(paths[i], info) || info.channels <= 0)
            continue;
        std::lock_guard<std::mutex> lock(probeMutex_);
        probed_.emplace_back(static_cast<int>(i), std::move(info));
    }
}

void Screen::takeProbed()
{
    std::vector<std::pair<int, ModuleInfo>> done;
    {
        std::lock_guard<std::mutex> lock(probeMutex_);
        done.swap(probed_);
    }
    for (auto& [i, info] : done)
        if (static_cast<std::size_t>(i) < browser_->info.size() && browser_->info[static_cast<std::size_t>(i)].channels == 0)
            browser_->info[static_cast<std::size_t>(i)] = std::move(info);
    if (! done.empty() && view_ == View::Library)
        browser_->repaint();
}

// SHIFT + HOME: the next frame, as the panel shows it, to the first free <dataDir>/screenshots/track16-NNN.png (on
// the stick: track16-data/track16/screenshots/).
void Screen::screenshot()
{
    if (! onScreenshot || config_.dataDir.empty())
    {
        event("NO SCREENSHOTS HERE");
        return;
    }
    const std::string dir = config_.dataDir + "/screenshots";
    ::mkdir(config_.dataDir.c_str(), 0755);
    ::mkdir(dir.c_str(), 0755);
    for (int n = 1; n < 1000; ++n)
    {
        char name[32];
        std::snprintf(name, sizeof name, "track16-%03d.png", n);
        struct stat st {};
        if (::stat((dir + "/" + name).c_str(), &st) == 0)
            continue;
        onScreenshot(dir + "/" + name);
        event("SCREENSHOT " + juce::String(name));
        return;
    }
    event("SCREENSHOTS FULL (999)");
}

// The module's own layout (<dataDir>/outs/<file name>.txt), or the default one.
void Screen::loadLayout()
{
    layoutFile_.clear();
    layout_ = outs::parse(config_.outsDefault);
    if (song_ && ! config_.dataDir.empty())
    {
        std::string name = song_->fileName;
        for (char& ch : name)
            if (ch == '/' || ch == '\\' || static_cast<unsigned char>(ch) < 32)
                ch = '_';
        layoutFile_ = config_.dataDir + "/outs/" + name + ".txt";
        if (std::ifstream in(layoutFile_); in)
        {
            std::string line, all;
            while (std::getline(in, line))
                if (! line.empty() && line[0] != '#')
                    all += line + " ";
            layout_ = outs::parse(all);
            logLine("OUTS %s: %s", layoutFile_.c_str(), outs::format(layout_).c_str());
        }
    }
    layoutDirty_ = false;
    player_.setOuts(layout_);
}

void Screen::saveLayout()
{
    layoutDirty_ = false;
    if (layoutFile_.empty())
        return;
    ::mkdir(config_.dataDir.c_str(), 0755);
    ::mkdir((config_.dataDir + "/outs").c_str(), 0755);
    std::ofstream out(layoutFile_, std::ios::trunc);
    out << "# Track16 single outs (" << (song_ ? song_->fileName : std::string()) << "), see the README\n" << outs::format(layout_) << "\n";
    if (! out)
        logLine("OUTS cannot write %s", layoutFile_.c_str());
}

void Screen::changeLayout(const juce::String& what, double nowMs)
{
    player_.setOuts(layout_);
    layoutDirty_ = true;
    layoutSaveMs_ = nowMs + 1500; // written once the edits pause
    event(what);
}

void Screen::setChannelDest(int channel, std::uint8_t dest)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    layout_.dest[static_cast<std::size_t>(channel)] = dest;
    if (dest == outs::kMain)
        layout_.alsoMain &= ~bit(channel);
}

// STEP keys in the OUTS pad mode. With pads held: their channels to MAIN (STEP 1–2) or a jack (STEP 3–8; with the pair's
// other STEP key held as well: the pair). Alone: STEP 3–8 cycle what the jack carries. SHIFT + STEP 1–10: a 3 s test
// tone on MAIN L, MAIN R, OUT 3 … OUT 8, PHONES L, PHONES R alone.
void Screen::outsStep(int s, double nowMs)
{
    const std::uint64_t pads = padsHeld_ & 0xFFFF;
    if (pads != 0 && song_)
    {
        if (s > 7)
            return;
        const int j = s - 2;
        const std::uint8_t dest = s < 2 ? outs::kMain : outs::stepJack(j, (stepsHeld_ >> ((j ^ 1) + 2)) & 1);
        int count = 0;
        for (int p = 0; p < 16; ++p)
            if ((pads >> p) & 1)
            {
                const int ch = player_.page.load() * 16 + p;
                if (ch < std::min(song_->channels, kMaxChannels))
                    setChannelDest(ch, dest), ++count;
            }
        if (count > 0)
            changeLayout(juce::String(count) + (count == 1 ? " CHANNEL" : " CHANNELS") + " TO "
                             + (dest == outs::kMain ? juce::String("MAIN") : "OUT " + juce::String(outs::destName(dest))),
                         nowMs);
        return;
    }
    if (shift_) // the output test
    {
        if (s >= Player::kToneOutputs)
            return;
        static const char* const names[] = {"MAIN L", "MAIN R", "OUT 3", "OUT 4", "OUT 5", "OUT 6", "OUT 7", "OUT 8", "PHONES L", "PHONES R"};
        const bool stop = player_.toneOutput() == s;
        player_.testTone(s);
        event(stop ? juce::String("TEST TONE OFF") : juce::String("TEST TONE: ") + names[s] + " ONLY, 3 S (440 HZ)");
        logLine("OUTS test tone %s %s", stop ? "off" : "on", names[s]);
        return;
    }
    if (s >= 2 && s <= 7)
    {
        auto& src = layout_.source[static_cast<std::size_t>(s - 2)];
        src = outs::nextSource(src);
        changeLayout("OUT " + juce::String(s + 1) + ": " + outs::sourceLongName(src), nowMs);
    }
}

void Screen::outsTouch(juce::Point<float> at, double nowMs)
{
    int row = -1, column = -2, jackSource = -1;
    outsPage_->hit(at - kBody.getPosition(), row, column, jackSource);
    if (jackSource >= 0)
    {
        auto& src = layout_.source[static_cast<std::size_t>(jackSource)];
        src = outs::nextSource(src);
        changeLayout("OUT " + juce::String(jackSource + 3) + ": " + outs::sourceLongName(src), nowMs);
        return;
    }
    const int ch = player_.page.load() * 16 + row;
    if (row < 0 || ! song_ || ch >= std::min(song_->channels, kMaxChannels))
        return;
    auto& d = layout_.dest[static_cast<std::size_t>(ch)];
    if (column == -1)
        setChannelDest(ch, outs::kMain);
    else if (column >= 0 && column < outs::kJacks)
    {
        if (layout_.source[static_cast<std::size_t>(column)] != outs::Source::Channels)
        {
            event("OUT " + juce::String(column + 3) + " CARRIES " + outs::sourceLongName(layout_.source[static_cast<std::size_t>(column)])
                  + ": TAP ITS NAME FOR CHANNELS");
            return;
        }
        setChannelDest(ch, outs::tapJack(d, column));
    }
    else if (column == 6 && d != outs::kMain)
        layout_.alsoMain ^= bit(ch);
    else
        return;
    changeLayout("CH " + juce::String(ch + 1) + " ON " + juce::String(outs::destName(d)) + ((layout_.alsoMain & bit(ch)) ? " +MAIN" : ""), nowMs);
}

void Screen::padRelease(int pad0, double)
{
    padsHeld_ &= ~(1ull << pad0);
    player_.padRelease(pad0, padHits_[static_cast<std::size_t>(pad0)]);
}

bool Screen::handle(const io::ControlEvent& e, double nowMs)
{
    using K = io::ControlEvent::Kind;
    using C = io::Control;
    if (io::isPad(e.control))
    {
        const int pad0 = io::indexOf(e.control) - 1;
        const bool fromReader = e.source == io::kSourcePanel || e.source == io::kSourceKeyboard;
        if (e.kind == K::Press)
            padPress(pad0, std::clamp<int>(e.value, 1, 127), fromReader, nowMs);
        else if (e.kind == K::Release)
            padRelease(pad0, nowMs);
        return true;
    }
    if (e.control == C::Shift)
    {
        shift_ = e.kind == K::Press;
        return true;
    }
    if (e.control == C::MainVolume && e.kind == K::Value)
    {
        player_.mainVolume = std::clamp(static_cast<float>(e.value) / 1023.0f, 0.0f, 1.0f);
        return true;
    }
    if (e.control == C::AfsDrive && e.kind == K::Value)
    {
        player_.drive = std::clamp<int>(e.value, 0, 1023);
        if (driveSeen_) // the panel reports the pot once at start: no message for that
        {
            const float db = 20.0f * std::log10(Player::driveGain(e.value));
            event("DRIVE +" + juce::String(db, 1) + " DB INTO THE ANALOG FILTER");
        }
        driveSeen_ = true;
        return true;
    }
    if (e.control == C::Strip)
    {
        if (e.source != io::kSourceStrip) // the strip's reader already told the player
            player_.push(platform::stripTrigger(e.value, e.kind != K::Release));
        return true;
    }
    // LCD touch, in 800 × 480 points → canvas lpx.
    if (e.control == C::Screen && e.kind == K::Touch)
    {
        const juce::Point<float> at(static_cast<float>(e.x) / theme::kScale, static_cast<float>(e.y) / theme::kScale);
        if (e.value == 1)
        {
            if (L::slotA.contains(at))
                setView(view_ == View::Player ? View::Library : View::Player);
            const auto now = static_cast<PadMode>(player_.padMode.load());
            for (int i = 0; i < 4; ++i)
                if (L::menuButton(i).contains(at))
                    setPadMode(i == 3 && now == PadMode::Play ? PadMode::Kit
                               : i == 0 && now == PadMode::Filter ? PadMode::Outs
                                                                  : static_cast<PadMode>(i),
                               false);
            if (outsVisible() && kBody.contains(at))
                outsTouch(at, nowMs);
            if (view_ == View::Library && kBody.contains(at))
            {
                const int row = browser_->rowAt(at - kBody.getPosition());
                if (row >= 0 && row == browser_->selected)
                {
                    loadEntry(row, false); // loads; PLAY starts it
                    setView(View::Player);
                }
                else if (row >= 0)
                    browser_->select(row);
            }
        }
        return true;
    }
    if (e.kind == K::Turn && io::isKnob(e.control))
    {
        const int d = e.value, n = io::indexOf(e.control);
        knobs_->touchedAt[static_cast<std::size_t>(n - 1)] = nowMs;
        switch (n)
        {
        case 1:
            if (beatKnob()) // with a MIDI clock: rows per beat (SHIFT: auto)
                stepBeat(d, shift_);
            else
                player_.tempoFactor = shift_ ? 1.0f : std::clamp(player_.tempoFactor.load() + 0.01f * static_cast<float>(d), 0.5f, 2.0f);
            break;
        case 2: player_.pitchSemitones = shift_ ? 0.0f : std::clamp(player_.pitchSemitones.load() + static_cast<float>(d), -12.0f, 12.0f); break;
        case 3: player_.stereoSeparation = std::clamp(player_.stereoSeparation.load() + 5 * d, 0, 200); break;
        case 4:
        {
            static constexpr int steps[] = {1, 2, 4, 8};
            int i = 0;
            while (i < 3 && steps[i] < player_.interpolation.load())
                ++i;
            player_.interpolation = steps[std::clamp(i + (d > 0 ? 1 : -1), 0, 3)];
            break;
        }
        case 5: player_.gainDb = shift_ ? 0.0f : std::clamp(player_.gainDb.load() + 0.5f * static_cast<float>(d), -24.0f, 12.0f); break;
        case 6: density_ = static_cast<Density>(std::clamp(static_cast<int>(density_) + (d > 0 ? 1 : -1), 0, 5)); break;
        default: break;
        }
        return true;
    }
    if (e.control == C::Selector && e.kind == K::Turn)
    {
        if (view_ == View::Library)
            browser_->select(browser_->selected + (shift_ ? 10 : 1) * e.value);
        else if (song_)
        {
            const int count = std::max(1, std::max(song_->numInstruments, song_->numSamples));
            player_.instrument = (player_.instrument.load() + e.value + count) % count;
            const int i = player_.instrument.load();
            event("INSTRUMENT " + juce::String(i + 1) + " "
                  + (i < static_cast<int>(song_->instrumentNames.size()) ? juce::String::fromUTF8(song_->instrumentNames[static_cast<std::size_t>(i)].c_str()) : juce::String()));
        }
        return true;
    }
    if (io::isStep(e.control) && (e.kind == K::Press || e.kind == K::Release))
    {
        const std::uint32_t b = 1u << (io::indexOf(e.control) - 1);
        stepsHeld_ = e.kind == K::Press ? (stepsHeld_ | b) : (stepsHeld_ & ~b);
        if (e.kind == K::Press && static_cast<PadMode>(player_.padMode.load()) == PadMode::Outs)
        {
            outsStep(io::indexOf(e.control) - 1, nowMs);
            return true;
        }
    }
    if (e.kind == K::Release && e.control == C::Back)
        return true;
    if (e.kind != K::Press)
        return true;
    switch (e.control)
    {
    case C::Back:
        if (shift_)
        {
            if (onQuit)
                onQuit();
        }
        else
            setView(view_ == View::Player ? View::Library : View::Player);
        break;
    case C::Home:
        if (shift_)
            screenshot();
        else
            setView(View::Library);
        break;
    case C::StripMode:
    {
        const int m = stripMode_ = (stripMode_ + 1) % Player::StripModes;
        player_.setStripMode(m);
        static const char* const names[] = {"STRIP: PITCH BEND", "STRIP: REPEAT (TOP = SHORT LOOPS)", "STRIP: TEMPO BEND (USER 1)", "STRIP: VOLUME (USER 2)"};
        event(names[m]);
        break;
    }
    case C::StripHold:
        stripHold_ = ! stripHold_;
        player_.setStripHold(stripHold_);
        event(stripHold_ ? "STRIP HOLD ON: THE LAST VALUE STAYS" : "STRIP HOLD OFF");
        break;
    case C::SelectorPush:
        if (view_ == View::Library)
        {
            loadEntry(browser_->selected, false); // loads; PLAY starts it
            setView(View::Player);
        }
        else
            setView(View::Library);
        break;
    case C::Play: player_.togglePlay(); break;
    case C::Stop:
        player_.stop();
        if (shift_) // everything back: mutes, solos
        {
            mutes_ = solos_ = 0;
            player_.muteMask = 0;
            player_.soloMask = 0;
            player_.masterFilter = false;
            event("RESET MUTES AND FILTER");
        }
        break;
    case C::Rec:
        player_.midiNotes = ! player_.midiNotes.load();
        event(player_.midiNotes.load() ? "MIDI NOTES ON (CHANNELS 1-16)" : "MIDI NOTES OFF");
        break;
    case C::Scene:
    case C::Pattern:
    {
        const int dir = e.control == C::Scene ? -1 : 1;
        if (shift_) // the Play mode's octave
        {
            player_.playRoot = std::clamp(player_.playRoot.load() + 12 * dir, 12, 96);
            event("PLAY ROOT " + noteName(player_.playRoot.load()));
        }
        else if (! library_.entries().empty())
        {
            const int n = static_cast<int>(library_.entries().size());
            loadEntry(((playingIndex_ < 0 ? 0 : playingIndex_) + dir + n) % n, true);
        }
        break;
    }
    case C::PadModeTrack: setPadMode(shift_ ? PadMode::Outs : PadMode::Filter, true); break;
    case C::PadModeMute: setPadMode(PadMode::Mute, true); break;
    case C::PadModeSlice: setPadMode(PadMode::Loop, true); break;
    case C::PadModeScale: setPadMode(shift_ ? PadMode::Kit : PadMode::Play, true); break;
    case C::Bar1:
    case C::Bar2:
    case C::Bar3:
    case C::Bar4:
        if (shift_) // the pads' page: channels / instruments 1–16, 17–32, 33–48, 49–64
        {
            const int page = static_cast<int>(e.control) - static_cast<int>(C::Bar1);
            if (page < pageCount())
            {
                player_.page = page;
                event("PADS PAGE " + juce::String(page + 1) + " (" + juce::String(page * 16 + 1) + "-" + juce::String(page * 16 + 16) + ")");
            }
            else
                event("NO PADS PAGE " + juce::String(page + 1) + ": " + juce::String(pageCount()) + (pageCount() == 1 ? " PAGE" : " PAGES"));
            break;
        }
        if (e.control == C::Bar4)
        {
            player_.patternLoop = ! player_.patternLoop.load();
            event(player_.patternLoop.load() ? "PATTERN LOOP ON" : "PATTERN LOOP OFF");
            break;
        }
        player_.quantize = static_cast<int>(e.control) - static_cast<int>(C::Bar1);
        {
            static const char* const names[] = {"JUMPS AT PATTERN END", "JUMPS ON THE BEAT", "JUMPS ON THE NEXT ROW"};
            event(names[player_.quantize.load()]);
        }
        break;
    case C::AfsActive:
        if (player_.stems())
        {
            player_.masterFilter = ! player_.masterFilter.load();
            event(player_.masterFilter.load() ? "WHOLE MIX THROUGH THE ANALOG FILTER" : "ANALOG FILTER: PADS ONLY");
        }
        else
            event("ANALOG FILTER LOOP NOT AVAILABLE");
        break;
    default:
        if (io::isStep(e.control) && song_)
        {
            const int s = io::indexOf(e.control) - 1;
            if (shift_)
            {
                if (s * 16 < song_->numOrders)
                {
                    orderPage_ = s;
                    event("ORDERS " + juce::String(s * 16) + "-" + juce::String(s * 16 + 15));
                }
            }
            else
            {
                const int order = orderPage_ * 16 + s;
                if (order < song_->numOrders && song_->orderPattern(order) >= 0)
                {
                    player_.jump(order, static_cast<Quantize>(player_.quantize.load()));
                    event("JUMP TO ORDER " + juce::String(order));
                }
            }
        }
        break;
    }
    return true;
}

// MIDI in for the UI (the player already has the notes, bends, the clock and the transport): the transport's events,
// Song Select (loads that module of the library), and the control channel: notes 36–51 are pads 1–16 (in the pad
// mode the panel is in), program change n jumps to order n + 1 (quantized as the BAR keys say), and the controllers
// of midiControl().
void Screen::midiIn(const MidiMessageIn& m, double nowMs)
{
    status_->midiInAt = nowMs;
    const int status = m.bytes[0], type = status & 0xF0, channel = status & 0x0F;
    if (status >= 0xF0)
    {
        if (! player_.midiSync.load())
            return;
        if (status == 0xFA)
            event("MIDI START: FOLLOWING THE CLOCK");
        else if (status == 0xFB)
            event("MIDI CONTINUE: FOLLOWING THE CLOCK");
        else if (status == 0xFC)
            event("MIDI STOP");
        else if (status == 0xF3 && m.bytes[1] < library_.entries().size())
            loadEntry(m.bytes[1], player_.playing());
        return;
    }
    const bool control = channel == player_.midiControlChannel.load() - 1;
    const bool on = type == 0x90 && m.bytes[2] > 0, off = type == 0x80 || (type == 0x90 && m.bytes[2] == 0);
    if (control && (on || off))
    {
        if (const int pad = m.bytes[1] - 36; pad >= 0 && pad < 16)
        {
            io::ControlEvent e {on ? io::ControlEvent::Kind::Press : io::ControlEvent::Kind::Release, io::pad(pad + 1),
                                static_cast<std::int16_t>(m.bytes[2]), static_cast<std::uint8_t>(io::kSourceMidi + m.port), nowMs};
            if (on || (padsHeld_ & (1ull << pad)))
                handle(e, nowMs);
        }
        return;
    }
    if (control && type == 0xB0)
        midiControl(m.bytes[1], m.bytes[2], nowMs);
    else if (control && type == 0xC0)
    {
        player_.jump(m.bytes[1], static_cast<Quantize>(player_.quantize.load()));
        event("MIDI: ORDER " + juce::String(m.bytes[1] + 1));
    }
    else if (on)
        event("MIDI IN CH " + juce::String(channel + 1) + " " + noteName(m.bytes[1]) + " " + juce::String(m.bytes[2]));
}

// The control channel's controllers: 7 volume; 70–75 the knobs TEMPO, PITCH, STEREO, INTERP, GAIN, VIEW (64 = the
// middle where it has one); 76 DRIVE; 80 ACTIVE, 81 pattern loop, 82 REC (≥ 64 = on); 85 the pad mode (0 FILTER,
// 1 MUTE, 2 LOOP, 3 PLAY, 4 KIT, 5 OUTS); 102–117 mute channel 1–16 (≥ 64 = muted).
void Screen::midiControl(int cc, int v, double nowMs)
{
    const float x = static_cast<float>(v) / 127.0f;
    const auto knob = [&](int n) { knobs_->touchedAt[static_cast<std::size_t>(n)] = nowMs; };
    switch (cc)
    {
    case 7: player_.midiVolume = x; break;
    case 70: player_.tempoFactor = v < 64 ? 0.5f + 0.5f * static_cast<float>(v) / 64.0f : 1.0f + static_cast<float>(v - 64) / 63.0f; knob(0); break;
    case 71: player_.pitchSemitones = std::clamp(std::round(static_cast<float>(v - 64) / 64.0f * 12.0f), -12.0f, 12.0f); knob(1); break;
    case 72: player_.stereoSeparation = static_cast<int>(std::lround(x * 200.0f)); knob(2); break;
    case 73:
    {
        static constexpr int steps[] = {1, 2, 4, 8};
        player_.interpolation = steps[std::clamp(v / 32, 0, 3)];
        knob(3);
        break;
    }
    case 74: player_.gainDb = v < 64 ? -24.0f + 24.0f * static_cast<float>(v) / 64.0f : 12.0f * static_cast<float>(v - 64) / 63.0f; knob(4); break;
    case 75: density_ = static_cast<Density>(std::clamp(v * 6 / 128, 0, 5)); knob(5); break;
    case 76: player_.drive = v * 1023 / 127; break;
    case 80:
        if (player_.stems() && player_.masterFilter.load() != (v >= 64))
        {
            player_.masterFilter = v >= 64;
            event(v >= 64 ? "MIDI: WHOLE MIX THROUGH THE ANALOG FILTER" : "MIDI: ANALOG FILTER PADS ONLY");
        }
        break;
    case 81: player_.patternLoop = v >= 64; break;
    case 82: player_.midiNotes = v >= 64; break;
    case 85: setPadMode(static_cast<PadMode>(std::clamp(v, 0, 5)), false); break;
    default:
        if (cc >= 102 && cc <= 117)
        {
            const std::uint64_t b = bit(cc - 102);
            mutes_ = v >= 64 ? (mutes_ | b) : (mutes_ & ~b);
            player_.muteMask = mutes_;
        }
        break;
    }
}

// ---- per frame ------------------------------------------------------------------------------------------------------

void Screen::frame(double nowMs, float fps)
{
    lastFrameMs_ = nowMs;
    player_.collect();
    takeLoaded();
    takeProbed();
    if (const int m = player_.measurements(); m != lastMeasurements_)
    {
        lastMeasurements_ = m;
        const int frames = player_.latencyFrames();
        const bool ok = player_.latencyMeasured();
        logLine("FILTER LOOP latency %d frames (%.1f ms) %s", frames, 1000.0 * frames / std::max(1, player_.sampleRate()),
                ok ? "measured" : "FALLBACK: no click came back");
        event(ok ? "FILTER LOOP " + juce::String(frames) + " FRAMES"
                 : "FILTER LOOP: NO RETURN, GUESSING " + juce::String(frames) + " FRAMES (LP CUTOFF CLOSED?)");
    }
    if (pending_)
    {
        std::shared_ptr<const SongData> data = pending_->data;
        const int beatBefore = player_.rowsPerBeat.load();
        applyBeat(*data); // before the player starts it
        if (! player_.load(std::move(pending_))) // the player starts it itself when it should play (Song::playOnAdopt)
            player_.rowsPerBeat = beatBefore;
        else
        {
            if (layoutDirty_)
                saveLayout(); // the last module's
            song_ = std::move(data);
            loadLayout();
            context_->set(context_->title, juce::String::fromUTF8(song_->title.c_str()));
            char info[160];
            std::snprintf(info, sizeof info, "%s · %d CH · %d:%02d", juce::String(song_->type).toUpperCase().toRawUTF8(),
                          song_->channels, static_cast<int>(std::lround(song_->durationSeconds)) / 60,
                          static_cast<int>(std::lround(song_->durationSeconds)) % 60);
            context_->set(context_->info, juce::String::fromUTF8(info));
        }
    }
    knobs_->frame(nowMs);

    // The OUTS page instead of the rain in the OUTS pad mode; the layout is written once the edits pause.
    {
        const bool outsOn = outsVisible();
        if (outsPage_->isVisible() != outsOn || canvas_->rainHole != (view_ == View::Player && ! outsOn))
        {
            outsPage_->setVisible(outsOn);
            canvas_->rainHole = view_ == View::Player && ! outsOn;
            canvas_->repaint();
        }
        if (layoutDirty_ && nowMs >= layoutSaveMs_)
            saveLayout();
        if (beatsDirty_ && nowMs >= beatsSaveMs_)
            saveBeats();
        auto& p = *outsPage_;
        bool changed = false;
        changed |= assign(p.song, song_);
        changed |= assign(p.page, player_.page.load());
        changed |= assign(p.stemsUsed, player_.outStemsUsed());
        changed |= assign(p.stemsTotal, player_.outStemsTotal());
        changed |= assign(p.unserved, player_.outsUnserved());
        changed |= assign(p.toneJack, player_.toneOutput() - 2);
        changed |= assign(p.muted, player_.muteMask.load() | (player_.soloMask.load() != 0 ? ~player_.soloMask.load() : std::uint64_t {0}));
        changed |= assign(p.held, padsHeld_ & 0xFFFF);
        changed |= assign(outsShown_, layout_);
        if (static_cast<long>(nowMs / 66.0) != static_cast<long>(outsMeterMs_ / 66.0)) // the meters at 15 Hz
        {
            outsMeterMs_ = nowMs;
            for (int j = 0; j < outs::kJacks; ++j)
            {
                const float v = player_.takeJackPeak(j);
                auto& l = p.level[static_cast<std::size_t>(j)];
                const float next = std::max(v, l * 0.7f);
                if (std::fabs(next - l) > 0.01f)
                    changed = true;
                l = next;
            }
        }
        if (changed && outsOn)
            p.repaint();
    }

    // Status row.
    const bool halfSecond = static_cast<long>(nowMs / 500.0) != static_cast<long>((nowMs - 17) / 500.0);
    status_->flag(status_->playing, player_.playing());
    status_->flag(status_->litIn, nowMs - status_->midiInAt < theme::midiActivityMs);
    status_->flag(status_->litOut, midi_.outputCount() > 0 && (player_.midiClock.load() || player_.midiNotes.load()) && player_.playing());
    status_->flag(status_->filter, player_.route() != FilterRoute::Off);
    status_->flag(status_->notes, player_.midiNotes.load());
    if (halfSecond)
    {
        char tail[96];
        std::snprintf(tail, sizeof tail, "%.0f FPS · DSP %.0f%%", static_cast<double>(fps), static_cast<double>(player_.loadAverage() * 100.0f));
        status_->set(status_->diag, juce::String::fromUTF8(tail)); // the platform and renderer are in the log
    }

    // Context bar.
    const auto pos = player_.position();
    char t[48];
    if (song_)
    {
        std::snprintf(t, sizeof t, "%02d/%02d", pos.order, std::max(0, song_->numOrders - 1));
        const int queued = player_.queuedOrder();
        context_->set(context_->queued, queued >= 0);
        if (queued >= 0)
            std::snprintf(t, sizeof t, "%02d>%02d", pos.order, queued);
        context_->set(context_->order, juce::String(t));
        std::snprintf(t, sizeof t, "%02d", pos.row);
        context_->set(context_->row, juce::String(t));
        context_->set(context_->speed, juce::String(player_.speed()));
        // Following an external clock: its tempo (caption EXT BPM); else the module's.
        const bool ext = player_.syncState() != Player::SyncInternal && player_.clockBpm() > 0;
        if (ext != context_->externalClock)
        {
            context_->externalClock = ext;
            context_->repaint();
        }
        if (ext)
            std::snprintf(t, sizeof t, "%.1f", static_cast<double>(player_.clockBpm()));
        else
            std::snprintf(t, sizeof t, "%.1f", static_cast<double>(player_.bpm() * player_.tempoFactor.load()));
        context_->set(context_->bpm, juce::String(t));
    }
    else
    {
        context_->set(context_->order, juce::String("--"));
        context_->set(context_->row, juce::String("--"));
        context_->set(context_->speed, juce::String("--"));
        context_->set(context_->bpm, juce::String("--"));
        context_->set(context_->title, juce::String("NO MODULE"));
        context_->set(context_->info, juce::String("PUSH SELECT TO LOAD"));
    }
    const auto mode = static_cast<PadMode>(player_.padMode.load());
    context_->set(context_->mode, mode == PadMode::Outs ? 0 : std::min(static_cast<int>(mode), 3));
    std::array<juce::String, 4> names {mode == PadMode::Outs ? "OUTS" : "FILTER", "MUTE", "LOOP", mode == PadMode::Kit ? "KIT" : "PLAY"};
    std::array<juce::String, 4> subs;
    const juce::String pageText = "PAGE " + juce::String(player_.page.load() + 1) + "/" + juce::String(pageCount());
    const int filtered = __builtin_popcountll(player_.filterChannels());
    subs[0] = ! player_.stems() ? juce::String("NO LOOP") : player_.masterFilter.load() ? juce::String("WHOLE MIX") : juce::String(filtered) + " CH";
    subs[1] = solos_ != 0 ? juce::String(__builtin_popcountll(solos_)) + " SOLO" : juce::String(__builtin_popcountll(mutes_)) + " MUTED";
    subs[2] = player_.loopActive() ? juce::String(player_.loopLength()) + " ROWS" + (player_.loopLatched() ? " HOLD" : "")
                                   : (player_.patternLoop.load() ? juce::String("PATTERN") : juce::String("OFF"));
    subs[3] = mode == PadMode::Kit ? pageText : "INST " + juce::String(player_.instrument.load() + 1);
    if (mode == PadMode::Outs)
    {
        int routed = 0;
        for (int c = 0; c < (song_ ? std::min(song_->channels, kMaxChannels) : 0); ++c)
            routed += layout_.dest[static_cast<std::size_t>(c)] != outs::kMain ? 1 : 0;
        subs[0] = player_.outsUnserved() != 0 ? juce::String("POOL FULL") : juce::String(routed) + " CH OUT";
    }
    if ((mode == PadMode::Filter || mode == PadMode::Mute || mode == PadMode::Outs) && pageCount() > 1)
    {
        const std::size_t i = mode == PadMode::Outs ? 0 : static_cast<std::size_t>(mode);
        subs[i] = subs[i] + " " + kDot + " P" + juce::String(player_.page.load() + 1);
    }
    for (int i = 0; i < 4; ++i)
    {
        context_->set(context_->modeName[static_cast<std::size_t>(i)], names[static_cast<std::size_t>(i)]);
        context_->set(context_->modeSub[static_cast<std::size_t>(i)], subs[static_cast<std::size_t>(i)]);
    }

    // Knob strip.
    // TEMPO: with a MIDI clock the rows per beat, as a ratio to 4 (EXT ×2: twice the rows per beat); else the factor.
    if (beatKnob())
    {
        static const char* const syncText[] = {"CLOCK", "WAIT", "EXT", "NO CLOCK"};
        const int sync = std::clamp(player_.syncState(), 0, 3), rpb = player_.rowsPerBeat.load();
        int i = 0;
        while (i < 6 && Player::kRowsPerBeat[i] < rpb)
            ++i;
        knobs_->set(0, static_cast<float>(i) / 6.0f,
                    sync == Player::SyncLost ? juce::String(syncText[sync]) : juce::String(syncText[sync]) + " " + ratioText(rpb));
    }
    else
    {
        std::snprintf(t, sizeof t, "%.0f%%", static_cast<double>(player_.tempoFactor.load() * 100.0f));
        knobs_->set(0, (player_.tempoFactor.load() - 0.5f) / 1.5f, t);
    }
    std::snprintf(t, sizeof t, "%+.0f ST", static_cast<double>(player_.pitchSemitones.load()));
    knobs_->set(1, (player_.pitchSemitones.load() + 12.0f) / 24.0f, t);
    std::snprintf(t, sizeof t, "%d%%", player_.stereoSeparation.load());
    knobs_->set(2, static_cast<float>(player_.stereoSeparation.load()) / 200.0f, t);
    static const char* const interp[] = {"NONE", "LINEAR", "CUBIC", "SINC"};
    const int ip = player_.interpolation.load();
    knobs_->set(3, static_cast<float>(ip) / 8.0f, interp[ip >= 8 ? 3 : ip >= 4 ? 2 : ip >= 2 ? 1 : 0]);
    std::snprintf(t, sizeof t, "%+.1f DB", static_cast<double>(player_.gainDb.load()));
    knobs_->set(4, (player_.gainDb.load() + 24.0f) / 36.0f, t);
    static const char* const views[] = {"AUTO", "FULL", "WIDE", "MEDIUM", "COMPACT", "RAIN"};
    knobs_->set(5, static_cast<float>(density_) / 5.0f, views[static_cast<int>(density_)]);

    if (view_ == View::Player)
        buildRain(nowMs);
}

// The pattern rain: the rows fall from the top onto the playhead line, flare in their instrument's colour as they cross
// it, and fade into a trail below. Columns are the module's channels (from the pads' page on, when they do not all fit).
void Screen::buildRain(double nowMs)
{
    rects_.clear();
    glyphs_.clear();
    overlay_.clear();
    if (! song_)
        return;
    const SongData& d = *song_;
    const auto pos = player_.position();
    const bool playing = player_.playing();
    double frac = 0;
    if (playing)
    {
        const std::uint64_t start = player_.rowStartNs(), now = nowNs();
        if (now > start)
            frac = std::clamp(static_cast<double>(now - start) / 1e9 / std::max(0.01, player_.rowSeconds()), 0.0, 1.0);
    }

    // Geometry (LCD px): the glyphs at 1:1 on whole pixels; a row-number gutter on the left, the channel columns
    // (centred), a header per column (number, name, level with peak hold).
    constexpr float kCharW = kGlyphW, kRowH = kGlyphH, kHeadH = 26, kGutterW = 3 * kCharW + 8;
    // Per density: full (note instrument volume effect), wide (note instrument effect), medium (note instrument),
    // compact (note), rain (one character).
    static constexpr int widthOf[] = {0, 14, 10, 6, 3, 1}; // characters per column
    static constexpr float gapOf[] = {0, 8, 8, 8, 5, 4};   // and the space between columns
    const auto columnW = [&](int density) { return static_cast<float>(widthOf[density]) * kCharW + gapOf[density]; };
    const float areaW = kRainW - kGutterW;
    int density = static_cast<int>(density_);
    if (density_ == Density::Auto)
    {
        density = 5;
        for (int k = 1; k <= 5; ++k)
            if (static_cast<float>(d.channels) * columnW(k) <= areaW)
            {
                density = k;
                break;
            }
    }
    const float colW = columnW(density);
    const int visible = std::max(1, static_cast<int>(areaW / colW));
    const auto mode = static_cast<PadMode>(player_.padMode.load());
    const int padFirst = (mode == PadMode::Filter || mode == PadMode::Mute || mode == PadMode::Outs) ? player_.page.load() * 16 : 0;
    int first = 0;
    if (d.channels > visible)
        first = std::clamp(padFirst, 0, d.channels - visible);
    const int last = std::min(d.channels, first + visible);
    // The gutter and the columns as one block, centred.
    const float gutterX = std::round(kRainX + std::max(0.0f, (areaW - colW * static_cast<float>(last - first)) / 2.0f));
    const float x0 = gutterX + kGutterW;
    const auto columnX = [&](int ch) { return x0 + std::round(colW * static_cast<float>(ch - first)); };

    const std::uint64_t solo = player_.soloMask.load(), mutes = player_.muteMask.load();
    const std::uint64_t filtered = player_.filterChannels();
    const int rpb = std::max(1, player_.rowsPerBeat.load());

    // The playhead at 62 % of the rows area; future rows fall from above, the trail below.
    const float top = kRainY + kHeadH, bottom = kRainY + kRainH;
    const float playY = std::round(top + (bottom - top) * 0.62f);
    const int above = static_cast<int>((playY - top) / kRowH) + 2, below = static_cast<int>((bottom - playY) / kRowH) + 1;

    // The playhead band (under the glyphs): where each row is when it plays.
    rects_.push_back({kRainX, playY, kRainW, kRowH, argb(kBand, 1.0f)});

    // Row k (0 = the current row, > 0 = future): its pattern and row, walking the order list.
    auto locate = [&](int k, int& pattern, int& row) -> bool {
        int o = pos.order, r = pos.row + k;
        for (int guard = 0; guard < 8; ++guard)
        {
            const int p = d.orderPattern(o);
            if (p < 0)
                return false;
            const int rows = d.patternRows(p);
            if (r >= 0 && r < rows)
            {
                pattern = p;
                row = r;
                return true;
            }
            if (r >= rows)
            {
                r -= rows;
                int n = o + 1;
                while (n < d.numOrders && d.orderPattern(n) == -1)
                    ++n;
                if (n >= d.numOrders || d.orderPattern(n) < 0)
                    return false;
                o = n;
            }
            else
            {
                int n = o - 1;
                while (n >= 0 && d.orderPattern(n) == -1)
                    --n;
                if (n < 0)
                    return false;
                o = n;
                r += d.patternRows(d.orderPattern(o));
            }
        }
        return false;
    };

    // Column headers: number and name in the channel's ink (the pad colour while on the pads; orange through the filter;
    // grey muted; white solo), a level bar with a peak tick that holds for a moment, and faint separators.
    const double dt = lastRainMs_ > 0 ? std::clamp((nowMs - lastRainMs_) / 1000.0, 0.0, 0.2) : 0.0;
    lastRainMs_ = nowMs;
    for (int ch = first; ch < last; ++ch)
    {
        const float x = columnX(ch);
        const float level = std::sqrt(std::clamp(player_.channelLevel(ch), 0.0f, 1.0f));
        const bool muted = (mutes & bit(ch)) || (solo != 0 && ! (solo & bit(ch)));
        const bool soloed = solo & bit(ch);
        const bool filt = filtered & bit(ch);
        const bool onPads = ch >= padFirst && ch < padFirst + 16 && (mode == PadMode::Filter || mode == PadMode::Mute);
        const std::uint32_t ink = muted ? kMuted : soloed ? kWhite : filt ? kFiltered : onPads ? (theme::kTrack[(ch - padFirst) & 15].base & 0xFFFFFF) : kRain;
        if (ch > first)
            rects_.push_back({x, kRainY, 1, kRainH, argb(kSeparator, 1.0f)});
        if (! muted && level > 0.02f)
            rects_.push_back({x + 1, top, colW - 2, bottom - top, argb(filt ? kFiltered : kRain, 0.035f * level)});
        // The peak: holds 0.6 s, then falls at 1.2 per second.
        auto& peak = peakHold_[static_cast<std::size_t>(ch)];
        auto& held = peakHeldMs_[static_cast<std::size_t>(ch)];
        if (level >= peak)
            peak = level, held = nowMs;
        else if (nowMs - held > 600)
            peak = std::max(level, peak - static_cast<float>(1.2 * dt));
        const float barX = x + 3, barW = colW - 6, barY = kRainY + kGlyphH + 4;
        rects_.push_back({barX, barY, barW, 3, argb(kSeparator, 1.0f)});
        rects_.push_back({barX, barY, std::round(barW * level), 3, argb(ink, muted ? 0.35f : 0.9f)});
        if (peak > 0.03f && ! muted)
            rects_.push_back({barX + std::round((barW - 2) * peak), barY - 1, 2, 5, argb(kWhite, 0.8f)});
        // "01 Name", as much as the column holds (the number alone in narrow columns).
        char text[24];
        const int chars = widthOf[density];
        std::snprintf(text, sizeof text, "%02d", (ch + 1) % 100);
        std::string label = density == 5 ? std::string(text + 1) : std::string(text);
        if (chars >= 6 && static_cast<std::size_t>(ch) < d.channelNames.size())
        {
            std::string name;
            for (char v : d.channelNames[static_cast<std::size_t>(ch)])
                name += (v >= 32 && v < 127) ? v : '?';
            while (! name.empty() && name.back() == ' ')
                name.pop_back();
            if (! name.empty())
                label += " " + name;
        }
        if (static_cast<int>(label.size()) > chars)
            label.resize(static_cast<std::size_t>(chars));
        const float tx = density >= 4 ? std::round(x + (colW - kCharW * static_cast<float>(label.size())) / 2.0f) : x + std::round(gapOf[density] / 2);
        for (std::size_t i = 0; i < label.size(); ++i)
            glyphs_.push_back({tx + kCharW * static_cast<float>(i), kRainY, kCharW, kRowH,
                               soloed || (onPads && i < 2) ? static_cast<char>(label[i] | kBold) : label[i],
                               argb(ink, muted ? 0.6f : onPads || soloed ? 1.0f : 0.8f)});
    }
    rects_.push_back({kRainX, top - 3, kRainW, 1, argb(kSeparator, 1.0f)});

    // Rows: the row number in the gutter, a line at each beat (stronger at each bar of 4 beats), the cells by field:
    // note (bold), instrument (its colour), volume, effect; dots faint. Rows coming are full strength for 8 rows,
    // then fade to 45 %; at the playhead they flare in their instrument's colour; the trail fades to 35 %.
    for (int k = -below; k <= above; ++k)
    {
        int pattern = 0, row = 0;
        if (! locate(k, pattern, row))
            continue;
        const float dist = static_cast<float>(k) - static_cast<float>(frac); // > 0 future, < 0 past
        const float y = std::round(playY - dist * kRowH);
        if (y < top - 1 || y + kRowH > bottom + 1)
            continue;
        const bool beat = row % rpb == 0, bar = row % (rpb * 4) == 0;
        if (beat)
            rects_.push_back({kRainX, y, kRainW, 1, argb(kRain, bar ? 0.22f : 0.10f)});
        float fade;
        float glow = 0;
        if (dist > 0)
            fade = dist <= 8 ? 1.0f : std::max(0.45f, 1.0f - 0.05f * (dist - 8));
        else
        {
            const float t = -dist;
            glow = std::exp(-t / 2.2f);
            fade = std::max(0.35f, std::exp(-t / 6.0f));
        }
        // The gutter: decimal row numbers, brighter on beats, white at the playhead.
        {
            char num[8];
            std::snprintf(num, sizeof num, "%3d", row);
            const bool current = k == 0;
            const std::uint32_t ink = current ? kWhite : bar ? kRain : beat ? kGutterBeat : kGutter;
            for (int i = 0; i < 3; ++i)
                if (num[i] != ' ')
                    glyphs_.push_back({gutterX + kCharW * static_cast<float>(i), y, kCharW, kRowH,
                                       current ? static_cast<char>(num[i] | kBold) : num[i], argb(ink, current ? 1.0f : fade)});
        }
        for (int ch = first; ch < last; ++ch)
        {
            const Cell* cell = d.cell(pattern, row, ch);
            if (cell == nullptr)
                continue;
            const float x = columnX(ch) + std::round(gapOf[density] / 2);
            const bool muted = (mutes & bit(ch)) || (solo != 0 && ! (solo & bit(ch)));
            const bool filt = filtered & bit(ch);
            const bool hasNote = cell->noteValue != 0;
            const bool noteOff = cell->noteValue > 120;
            std::uint32_t noteInk = noteOff ? kNoteOff : kNote;
            std::uint32_t instInk = mix(instrumentColour(cell->instrument), kWhite, 0.3f);
            std::uint32_t volInk = kVol, fxInk = kFx;
            if (glow > 0.05f && hasNote && ! noteOff) // the flare
            {
                noteInk = mix(noteInk, mix(instrumentColour(cell->instrument), kWhite, 0.45f), glow);
                if (-dist < 1.0f && ! muted)
                    rects_.push_back({columnX(ch) + 1, y, colW - 2, kRowH, argb(instrumentColour(cell->instrument), 0.30f * (1.0f + dist))});
            }
            if (filt)
                noteInk = mix(noteInk, kFiltered, 0.6f), instInk = mix(instInk, kFiltered, 0.6f);
            float a = fade;
            if (muted)
                noteInk = instInk = volInk = fxInk = kMuted, a *= 0.5f;
            auto put = [&](float gx, char v, std::uint32_t ink, bool bold) {
                if (v == ' ')
                    return;
                const bool dot = v == '.';
                glyphs_.push_back({gx, y, kCharW, kRowH, bold && ! dot ? static_cast<char>(v | kBold) : v,
                                   argb(dot ? kEmpty : ink, a)});
            };
            if (density == 5)
            {
                put(x, hasNote ? (noteOff ? '|' : cell->note[0]) : (cell->fx[0] != '.' ? ':' : '.'), hasNote ? noteInk : fxInk, hasNote);
                continue;
            }
            float gx = x;
            for (char v : cell->note)
                put(gx, v, noteInk, true), gx += kCharW;
            if (density <= 3)
            {
                gx += kCharW;
                for (char v : cell->inst)
                    put(gx, v, instInk, false), gx += kCharW;
            }
            if (density == 1)
            {
                gx += kCharW;
                for (char v : cell->vol)
                    put(gx, v, volInk, false), gx += kCharW;
            }
            if (density <= 2)
            {
                gx += kCharW;
                for (char v : cell->fx)
                    put(gx, v, fxInk, false), gx += kCharW;
            }
        }
    }
    // The playhead's edges.
    overlay_.push_back({kRainX, playY - 1, kRainW, 1, argb(kRain, 0.55f)});
    overlay_.push_back({kRainX, playY + kRowH, kRainW, 1, argb(kRain, 0.55f)});
    if (! playing)
        overlay_.push_back({kRainX, top, kRainW, bottom - top, argb(0x0B0D10, 0.35f)}); // paused: dimmed
}

SceneInput Screen::scene(double nowMs) const
{
    SceneInput s;
    s.nowMs = nowMs;
    s.background = c::bg;
    if (view_ == View::Player)
    {
        s.rects = &rects_;
        s.glyphs = &glyphs_;
        s.overlay = &overlay_;
    }
    return s;
}

// ---- lamps ----------------------------------------------------------------------------------------------------------

void Screen::lamps(io::LampFrame& f, double) const
{
    using C = io::Control;
    using io::LampMode;
    using io::LampState;
    const auto mode = static_cast<PadMode>(player_.padMode.load());
    const int page = player_.page.load();
    const int channels = song_ ? song_->channels : 0;
    const int instruments = song_ ? std::max(song_->numInstruments, song_->numSamples) : 0;
    const std::uint64_t filtered = player_.filterChannels(), latched = player_.filterLatched();
    const std::uint64_t held = player_.padHeld() | padsHeld_;
    for (int i = 0; i < 16; ++i)
    {
        const int index = page * 16 + i;
        const auto led = theme::kTrack[index & 15].led;
        const bool down = (held >> i) & 1;
        LampState s {};
        switch (mode)
        {
        case PadMode::Mute:
        case PadMode::Filter:
            if (index >= channels)
                break;
            {
                const float level = player_.channelLevel(index);
                const bool muted = (mutes_ & bit(index)) || (solos_ != 0 && ! (solos_ & bit(index)));
                if (mode == PadMode::Filter && (filtered & bit(index))) // red: filtered; white: kept clean under ACTIVE
                    s = {(latched & bit(index)) && ! down ? LampMode::BlinkSlow : LampMode::On,
                         player_.masterFilter.load() ? io::lampcolour::white : io::lampcolour::red, 0};
                else if (mode == PadMode::Mute && (solos_ & bit(index)))
                    s = {LampMode::On, io::lampcolour::white, 0};
                else if (muted)
                    s = {LampMode::Faint, led, 0};
                else
                    s = {level > 0.08f ? LampMode::On : LampMode::Dim, led, 0};
            }
            break;
        case PadMode::Loop:
            if (Player::kLoopLengths[i] > 0)
            {
                const bool active = player_.loopActive() && player_.loopLength() == Player::kLoopLengths[i];
                s = active ? LampState {player_.loopLatched() ? LampMode::BlinkSlow : LampMode::On, io::lampcolour::amber, 0}
                           : LampState {LampMode::Dim, i < 8 ? io::lampcolour::amber : io::lampcolour::blue, 0};
            }
            break;
        case PadMode::Play:
        {
            const int note = player_.playRoot.load() + i;
            s = down ? LampState {LampMode::Flash, io::lampcolour::white, padHitMs_[static_cast<std::size_t>(i)]}
                     : LampState {note % 12 == 0 ? LampMode::On : LampMode::Dim, note % 12 == 0 ? io::lampcolour::blue : theme::kTrack[10].led, 0};
            break;
        }
        case PadMode::Kit:
            if (index < instruments)
                s = down ? LampState {LampMode::Flash, led, padHitMs_[static_cast<std::size_t>(i)]} : LampState {LampMode::Dim, led, 0};
            break;
        case PadMode::Outs:
            // MAIN: faint in the channel's colour; a pair lit, a single jack dim, in the jack group's colour; +MAIN
            // blinks; not routed (pool full) blinks red; held: white.
            if (index < channels && index < kMaxChannels)
            {
                const auto d = layout_.dest[static_cast<std::size_t>(index)];
                if (down)
                    s = {LampMode::On, io::lampcolour::white, 0};
                else if (d == outs::kMain)
                    s = {LampMode::Faint, led, 0};
                else if ((player_.outsUnserved() >> d) & 1)
                    s = {LampMode::BlinkFast, io::lampcolour::red, 0};
                else
                {
                    static constexpr std::uint32_t groups[3] = {io::lampcolour::blue, io::lampcolour::green, io::lampcolour::amber};
                    const bool also = (layout_.alsoMain >> index) & 1;
                    s = {also ? LampMode::BlinkSlow : outs::isPair(d) ? LampMode::On : LampMode::Dim, groups[outs::group(d)], 0};
                }
            }
            break;
        }
        f[io::pad(i + 1)] = s;
    }
    // Step keys: the order page; the playing order white, a queued one blinking, the others dim. In the OUTS pad mode:
    // STEP 1-2 MAIN, 3-8 the jacks (lit: where the held pad's channel goes; otherwise what each jack carries).
    const auto pos = player_.position();
    const int queued = player_.queuedOrder();
    if (mode == PadMode::Outs)
    {
        static constexpr std::uint32_t groups[3] = {io::lampcolour::blue, io::lampcolour::green, io::lampcolour::amber};
        int heldChannel = -1;
        for (int i = 0; i < 16 && heldChannel < 0; ++i)
            if ((padsHeld_ >> i) & 1)
                heldChannel = page * 16 + i;
        const bool showHeld = heldChannel >= 0 && heldChannel < std::min(channels, kMaxChannels);
        const auto d = showHeld ? layout_.dest[static_cast<std::size_t>(heldChannel)] : outs::kMain;
        const bool onMain = showHeld && (d == outs::kMain || ((layout_.alsoMain >> heldChannel) & 1));
        std::uint32_t usedJacks = 0;
        for (int c = 0; c < std::min(channels, kMaxChannels); ++c)
            usedJacks |= outs::jackMask(layout_.dest[static_cast<std::size_t>(c)]);
        for (int i = 0; i < 16; ++i)
        {
            LampState s {};
            if (i < 2)
                s = showHeld ? (onMain ? LampState {LampMode::On, io::lampcolour::white, 0} : LampState {})
                             : LampState {LampMode::Dim, io::lampcolour::white, 0};
            else if (i < 8)
            {
                const int j = i - 2;
                const auto src = layout_.source[static_cast<std::size_t>(j)];
                if (showHeld)
                    s = (outs::jackMask(d) >> j) & 1 ? LampState {LampMode::On, groups[j / 2], 0} : LampState {};
                else if (src == outs::Source::Channels)
                    s = {(usedJacks >> j) & 1 ? LampMode::On : LampMode::Faint, groups[j / 2], 0};
                else
                    s = {src == outs::Source::Click ? LampMode::BlinkSlow : LampMode::Dim,
                         src == outs::Source::Return ? io::lampcolour::red : io::lampcolour::white, 0};
            }
            // SHIFT: the output test's ten keys; the one playing blinks.
            if (shift_ && ! showHeld)
                s = i < Player::kToneOutputs ? LampState {LampMode::Dim, io::lampcolour::white, 0} : LampState {};
            if (player_.toneOutput() == i)
                s = {LampMode::BlinkFast, io::lampcolour::white, 0};
            f[io::step(i + 1)] = s;
        }
    }
    for (int i = 0; i < 16 && mode != PadMode::Outs; ++i)
    {
        const int order = orderPage_ * 16 + i;
        LampState s {};
        if (song_ && order < song_->numOrders && song_->orderPattern(order) >= 0)
        {
            if (order == queued)
                s = {LampMode::BlinkFast, io::lampcolour::amber, 0};
            else if (order == pos.order)
                s = {LampMode::On, io::lampcolour::white, 0};
            else
                s = {LampMode::Dim, theme::kTrack[(song_->orderPattern(order)) & 15].led, 0};
        }
        f[io::step(i + 1)] = s;
    }
    f[C::Play] = player_.playing() ? LampState {LampMode::On, io::lampcolour::green, 0} : LampState {};
    f[C::Stop] = {};
    f[C::Rec] = player_.midiNotes.load() ? LampState {LampMode::On, io::lampcolour::red, 0} : LampState {};
    const C modes[] = {C::PadModeTrack, C::PadModeMute, C::PadModeSlice, C::PadModeScale};
    const int modeButton = mode == PadMode::Outs ? 0 : std::min(static_cast<int>(mode), 3);
    for (int i = 0; i < 4; ++i)
        f[modes[i]] = i == modeButton ? LampState {mode == PadMode::Kit || mode == PadMode::Outs ? LampMode::BlinkSlow : LampMode::On,
                                                   io::lampcolour::white, 0}
                                      : LampState {};
    if (shift_) // SHIFT: the pads' pages; the shown one lit, the others there blinking slowly
    {
        const int pages = pageCount();
        for (int i = 0; i < 4; ++i)
            f[static_cast<C>(static_cast<int>(C::Bar1) + i)] = i == page    ? LampState {LampMode::On, io::lampcolour::white, 0}
                                                               : i < pages ? LampState {LampMode::BlinkSlow, io::lampcolour::white, 0}
                                                                           : LampState {};
    }
    else
    {
        const int q = player_.quantize.load();
        for (int i = 0; i < 3; ++i)
            f[static_cast<C>(static_cast<int>(C::Bar1) + i)] = i == q ? LampState {LampMode::On, io::lampcolour::white, 0} : LampState {};
        f[C::Bar4] = player_.patternLoop.load() ? LampState {LampMode::On, io::lampcolour::white, 0} : LampState {};
    }
    f[C::Scene] = f[C::Pattern] = {};
    // ANALOG FILTER ACTIVE: lit while audio goes through the filter loop (on the unit the lamp also drives the filter
    // board's select line, hw/imx6/AnalogBoard.h) and while the loop's latency is measured.
    const bool filterLit = player_.route() != FilterRoute::Off || player_.measuring();
    f[C::AfsActive] = filterLit ? LampState {LampMode::On, io::lampcolour::red, 0} : LampState {};
    // The strip: its LEDs show the position it acts with, the MODE lamps its mode, HOLD whether it keeps its value.
    const int stripMode = player_.stripMode();
    io::stripLamps(player_.stripPosition(), player_.stripTouched(), player_.stripLatched(), static_cast<io::StripMode>(stripMode), f);
    const C stripModes[] = {C::StripModePitch, C::StripModeRepeat, C::StripModeUser1, C::StripModeUser2};
    for (int i = 0; i < 4; ++i)
        f[stripModes[i]] = i == stripMode ? LampState {LampMode::On, io::lampcolour::white, 0} : LampState {};
    f[C::StripHold] = player_.stripHold() ? LampState {LampMode::On, io::lampcolour::white, 0} : LampState {};
    f[C::AfsOver] = player_.over() ? LampState {LampMode::On, io::lampcolour::red, 0} : LampState {};
}
} // namespace app
