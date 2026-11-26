#include "app/Screen.h"

#include "app/Log.h"
#include "app/Theme.h"

#include "ProjectIdentity.h"
#include "controls/ControlNames.h"

#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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

constexpr std::uint32_t kRain = 0x3CDC78, kRainDim = 0x1E6E3C, kMuted = 0x5A5F66, kFiltered = 0xFF7A3C, kWhite = 0xFFFFFF;

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

    void paint(juce::Graphics& g) override
    {
        const float cy = L::statusH / 2;
        const juce::String app = juce::String(identity::displayName).toUpperCase();
        theme::text(g, 16, cy, Align::Left, Text::Label, c::text2, app);
        float x = 16 + theme::textWidth(Text::Label, app) + 8;
        theme::text(g, x, cy, Align::Left, Text::Label, c::text3, kChevron);
        x += theme::textWidth(Text::Label, kChevron) + 8;
        theme::text(g, x, cy, Align::Left, Text::Label, c::text1, where);
        theme::text(g, 300, cy, Align::Left, Text::Caption, c::text2, event);
        float ix = 1264;
        theme::glyph(g, theme::Glyph::Play, ix - 9, cy - 5, 10, playing ? c::play : c::text3);
        ix -= 44;
        theme::text(g, ix, cy, Align::Right, Text::Caption, notes ? c::rec : c::text3, "NOTES");
        ix -= 64;
        theme::text(g, ix, cy, Align::Right, Text::Caption, filter ? c::warn : c::text3, "FILTER");
        ix -= 72;
        theme::text(g, ix, cy, Align::Right, Text::Caption, litOut ? c::accent : c::text3, "OUT");
        ix -= 40;
        theme::text(g, ix, cy, Align::Right, Text::Caption, litIn ? c::accent : c::text3, "IN");
        theme::text(g, ix - 48, cy, Align::Right, Text::Caption, c::text3, diag);
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
            repaint(900, 0, 380, static_cast<int>(L::statusH));
    }
};

// ---- context bar ----------------------------------------------------------------------------------------------------

struct Screen::ContextBar final : juce::Component
{
    juce::String order, row, speed, bpm, title, info;
    std::array<juce::String, 4> modeName {"FILTER", "MUTE", "LOOP", "PLAY"}, modeSub;
    int mode = 1;
    bool queued = false;

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
        split(g, L::slotC, "SPEED", speed, c::text1, "BPM", bpm);
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
    static constexpr float kRowH = 44, kHeadH = 44;
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
        theme::text(g, 16, kHeadH / 2, Align::Left, Text::Label, c::text1, "LIBRARY");
        theme::text(g, 120, kHeadH / 2, Align::Left, Text::Caption, c::text2,
                    juce::String(static_cast<int>(list.size())) + " MODULES  " + kDot + "  " + root);
        theme::text(g, w - 16, kHeadH / 2, Align::Right, Text::Caption, c::text3,
                    "SELECT: CHOOSE  " + kDot + "  PUSH: PLAY  " + kDot + "  BACK: PLAYER");
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
                theme::glyph(g, theme::Glyph::Play, r.getX() + 14, r.getCentreY() - 6, 12, c::play);
            float x = r.getX() + 36;
            if (! e.folder.empty())
            {
                const juce::String folder = juce::String::fromUTF8(e.folder.c_str()) + " /";
                theme::text(g, x, r.getCentreY(), Align::Left, Text::Caption, c::text3, folder);
                x += theme::textWidth(Text::Caption, folder) + 10;
            }
            theme::text(g, x, r.getCentreY(), Align::Left, Text::BodyStrong, i == selected ? c::text1 : c::text2,
                        juce::String::fromUTF8(e.name.c_str()));
            theme::text(g, r.getRight() - 16, r.getCentreY(), Align::Right, Text::Caption, c::text3, sizeText(e.size));
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
        theme::text(g, 16, L::stripY + 13, Align::Left, Text::Caption, c::text2, tab);
        for (int n = 0; n < 6; ++n)
        {
            const auto i = static_cast<std::size_t>(n);
            const float x = L::columnX0 + L::columnW * static_cast<float>(n), cx = x + L::columnW / 2;
            theme::text(g, cx, L::columnCaptionY, Align::Centre, Text::Caption, c::text2, captions[n]);
            theme::text(g, cx, L::columnValueY, Align::Centre, Text::Value, c::text1, text[i]);
            if (underlined[i])
            {
                g.setColour(c::accent);
                g.fillRoundedRectangle(x + 64, L::columnUnderlineY, 80, 2, 1);
            }
            g.setColour(c::borderStrong);
            g.fillRoundedRectangle(x + 40, L::columnTrackY, 128, 2, 1);
            g.setColour(c::text2);
            g.fillRoundedRectangle(x + 40, L::columnTrackY, std::max(2.0f, 128.0f * std::clamp(value[i], 0.0f, 1.0f)), 2, 1);
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
    library_.scan(config_.roots);
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
        loadJob_ = {index, list[static_cast<std::size_t>(index)].path, play, player_.stems(), player_.sampleRate()};
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
        result->song = loadSong(result->job.path, result->job.stems, result->job.sampleRate, config_.stereoSeparation,
                                config_.interpolation, result->error);
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
    logLine("LOAD %s \"%s\" %s, %d ch, %d orders, %d patterns, %d instruments, %.0f s, stems=%d (%.0f ms)", d.path.c_str(),
            d.title.c_str(), d.type.c_str(), d.channels, d.numOrders, d.numPatterns, std::max(d.numInstruments, d.numSamples),
            d.durationSeconds, r->song->wet ? 1 : 0, r->ms);
    event("LOADED " + juce::String::fromUTF8(d.title.c_str()));
    r->song->playOnAdopt = r->job.play;
    pending_ = std::move(r->song); // replaces one the player has not taken yet
    playingIndex_ = r->job.index;
    browser_->playing = r->job.index;
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
    static const char* const names[] = {"FILTER", "MUTE", "LOOP", "PLAY", "KIT"};
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
    }
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
            for (int i = 0; i < 4; ++i)
                if (L::menuButton(i).contains(at))
                    setPadMode(i == 3 && static_cast<PadMode>(player_.padMode.load()) == PadMode::Play ? PadMode::Kit : static_cast<PadMode>(i), false);
            if (view_ == View::Library && kBody.contains(at))
            {
                const int row = browser_->rowAt(at - kBody.getPosition());
                if (row >= 0 && row == browser_->selected)
                {
                    loadEntry(row, true);
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
        case 1: player_.tempoFactor = shift_ ? 1.0f : std::clamp(player_.tempoFactor.load() + 0.01f * static_cast<float>(d), 0.5f, 2.0f); break;
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
        case 6: density_ = static_cast<Density>(std::clamp(static_cast<int>(density_) + (d > 0 ? 1 : -1), 0, 4)); break;
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
    case C::Home: setView(View::Library); break;
    case C::SelectorPush:
        if (view_ == View::Library)
        {
            loadEntry(browser_->selected, true);
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
    case C::PadModeTrack: setPadMode(PadMode::Filter, true); break;
    case C::PadModeMute: setPadMode(PadMode::Mute, true); break;
    case C::PadModeSlice: setPadMode(PadMode::Loop, true); break;
    case C::PadModeScale: setPadMode(shift_ ? PadMode::Kit : PadMode::Play, true); break;
    case C::Bar1:
    case C::Bar2:
    case C::Bar3:
    {
        const int q = static_cast<int>(e.control) - static_cast<int>(C::Bar1);
        player_.quantize = q;
        static const char* const names[] = {"JUMPS AT PATTERN END", "JUMPS ON THE BEAT", "JUMPS ON THE NEXT ROW"};
        event(names[q]);
        break;
    }
    case C::Bar4:
        player_.patternLoop = ! player_.patternLoop.load();
        event(player_.patternLoop.load() ? "PATTERN LOOP ON" : "PATTERN LOOP OFF");
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

void Screen::midiIn(const MidiMessageIn& m, double nowMs)
{
    status_->midiInAt = nowMs;
    const int type = m.bytes[0] & 0xF0;
    if (type == 0x90 && m.bytes[2] > 0)
        event("MIDI IN NOTE " + noteName(m.bytes[1]) + " " + juce::String(m.bytes[2]));
}

// ---- per frame ------------------------------------------------------------------------------------------------------

void Screen::frame(double nowMs, float fps)
{
    lastFrameMs_ = nowMs;
    player_.collect();
    takeLoaded();
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
        if (player_.load(std::move(pending_))) // the player starts it itself when it should play (Song::playOnAdopt)
        {
            song_ = std::move(data);
            context_->set(context_->title, juce::String::fromUTF8(song_->title.c_str()));
            char info[160];
            std::snprintf(info, sizeof info, "%s · %d CH · %d ORD · %d:%02d", juce::String(song_->type).toUpperCase().toRawUTF8(),
                          song_->channels, song_->numOrders, static_cast<int>(song_->durationSeconds) / 60,
                          static_cast<int>(song_->durationSeconds) % 60);
            context_->set(context_->info, juce::String::fromUTF8(info));
        }
    }
    knobs_->frame(nowMs);

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
        std::snprintf(tail, sizeof tail, " · %.0f FPS · DSP %.1f%%", static_cast<double>(fps), static_cast<double>(player_.loadAverage() * 100.0f));
        status_->set(status_->diag, statusBase_ + juce::String::fromUTF8(tail));
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
        context_->set(context_->info, juce::String("PUSH SELECT IN THE LIBRARY"));
    }
    const auto mode = static_cast<PadMode>(player_.padMode.load());
    context_->set(context_->mode, std::min(static_cast<int>(mode), 3));
    std::array<juce::String, 4> names {"FILTER", "MUTE", "LOOP", mode == PadMode::Kit ? "KIT" : "PLAY"};
    std::array<juce::String, 4> subs;
    const juce::String pageText = "PAGE " + juce::String(player_.page.load() + 1) + "/" + juce::String(pageCount());
    const int filtered = __builtin_popcountll(player_.filterChannels());
    subs[0] = ! player_.stems() ? juce::String("NO LOOP") : player_.masterFilter.load() ? juce::String("WHOLE MIX") : juce::String(filtered) + " CH";
    subs[1] = juce::String(__builtin_popcountll(mutes_)) + " MUTED" + (solos_ != 0 ? " +SOLO" : "");
    subs[2] = player_.loopActive() ? juce::String(player_.loopLength()) + " ROWS" + (player_.loopLatched() ? " HOLD" : "")
                                   : (player_.patternLoop.load() ? juce::String("PATTERN") : juce::String("OFF"));
    subs[3] = mode == PadMode::Kit ? pageText : "INST " + juce::String(player_.instrument.load() + 1);
    if (mode == PadMode::Filter || mode == PadMode::Mute)
        subs[static_cast<std::size_t>(mode)] = subs[static_cast<std::size_t>(mode)] + " " + kDot + " P" + juce::String(player_.page.load() + 1);
    for (int i = 0; i < 4; ++i)
    {
        context_->set(context_->modeName[static_cast<std::size_t>(i)], names[static_cast<std::size_t>(i)]);
        context_->set(context_->modeSub[static_cast<std::size_t>(i)], subs[static_cast<std::size_t>(i)]);
    }

    // Knob strip.
    std::snprintf(t, sizeof t, "%.0f%%", static_cast<double>(player_.tempoFactor.load() * 100.0f));
    knobs_->set(0, (player_.tempoFactor.load() - 0.5f) / 1.5f, t);
    std::snprintf(t, sizeof t, "%+.0f ST", static_cast<double>(player_.pitchSemitones.load()));
    knobs_->set(1, (player_.pitchSemitones.load() + 12.0f) / 24.0f, t);
    std::snprintf(t, sizeof t, "%d%%", player_.stereoSeparation.load());
    knobs_->set(2, static_cast<float>(player_.stereoSeparation.load()) / 200.0f, t);
    static const char* const interp[] = {"NONE", "LINEAR", "CUBIC", "SINC"};
    const int ip = player_.interpolation.load();
    knobs_->set(3, static_cast<float>(ip) / 8.0f, interp[ip >= 8 ? 3 : ip >= 4 ? 2 : ip >= 2 ? 1 : 0]);
    std::snprintf(t, sizeof t, "%+.1f DB", static_cast<double>(player_.gainDb.load()));
    knobs_->set(4, (player_.gainDb.load() + 24.0f) / 36.0f, t);
    static const char* const views[] = {"AUTO", "FULL", "MEDIUM", "COMPACT", "RAIN"};
    knobs_->set(5, static_cast<float>(density_) / 4.0f, views[static_cast<int>(density_)]);

    if (view_ == View::Player)
        buildRain(nowMs);
}

// The pattern rain: the rows fall from the top onto the playhead line, flare in their instrument's colour as they cross
// it, and fade into a trail below. Columns are the module's channels (from the pads' page on, when they do not all fit).
void Screen::buildRain(double)
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

    // Density: characters per channel column.
    constexpr float kCharW = 7, kRowH = 14, kGlyphW = 8, kHeadH = 18;
    static constexpr int widthOf[] = {0, 14, 6, 3, 1};
    const auto columnW = [&](int density) { return density == 4 ? 12.0f : static_cast<float>(widthOf[density]) * kCharW + 7.0f; };
    int density = static_cast<int>(density_);
    if (density_ == Density::Auto)
    {
        density = 4;
        for (int k = 1; k <= 4; ++k)
            if (static_cast<float>(d.channels) * columnW(k) <= kRainW)
            {
                density = k;
                break;
            }
    }
    const float colW = columnW(density);
    const int visible = std::max(1, static_cast<int>(kRainW / colW));
    const auto mode = static_cast<PadMode>(player_.padMode.load());
    const int padFirst = (mode == PadMode::Filter || mode == PadMode::Mute) ? player_.page.load() * 16 : 0;
    int first = 0;
    if (d.channels > visible)
        first = std::clamp(padFirst, 0, d.channels - visible);
    const int last = std::min(d.channels, first + visible);
    const float x0 = kRainX + std::max(0.0f, (kRainW - colW * static_cast<float>(last - first)) / 2.0f);

    const std::uint64_t solo = player_.soloMask.load(), mutes = player_.muteMask.load();
    const std::uint64_t filtered = player_.filterChannels();
    const int rpb = std::max(1, player_.rowsPerBeat.load());

    // Geometry: the playhead at 62 % of the rows area; future rows above, the trail below.
    const float top = kRainY + kHeadH, bottom = kRainY + kRainH;
    const float playY = top + (bottom - top) * 0.62f;
    const int above = static_cast<int>((playY - top) / kRowH) + 2, below = static_cast<int>((bottom - playY) / kRowH) + 1;

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

    // Columns: header (channel number, level) and a faint glow with the level.
    for (int ch = first; ch < last; ++ch)
    {
        const float x = x0 + colW * static_cast<float>(ch - first);
        const float level = std::sqrt(std::clamp(player_.channelLevel(ch), 0.0f, 1.0f));
        const bool muted = (mutes & bit(ch)) || (solo != 0 && ! (solo & bit(ch)));
        const bool filt = filtered & bit(ch);
        const bool onPads = ch >= padFirst && ch < padFirst + 16 && (mode == PadMode::Filter || mode == PadMode::Mute);
        const std::uint32_t ink = muted ? kMuted : filt ? kFiltered : onPads ? (theme::kTrack[(ch - padFirst) & 15].base & 0xFFFFFF) : kRain;
        if (! muted && level > 0.02f)
            rects_.push_back({x + 1, top, colW - 2, bottom - top, argb(filt ? kFiltered : kRain, 0.06f * level)});
        rects_.push_back({x + 2, kRainY + 13, (colW - 4) * level, 3, argb(ink, muted ? 0.3f : 0.9f)});
        char num[16];
        std::snprintf(num, sizeof num, "%02d", (ch + 1) % 100);
        const int nChars = density == 4 ? 1 : 2;
        const char* text = density == 4 ? num + 1 : num;
        const float tx = x + (colW - kCharW * static_cast<float>(nChars)) / 2.0f;
        for (int i = 0; i < nChars; ++i)
            glyphs_.push_back({tx + kCharW * static_cast<float>(i) - 0.5f, kRainY, kGlyphW, 12, text[i], argb(ink, onPads ? 1.0f : 0.6f)});
    }

    // Rows.
    for (int k = -below; k <= above; ++k)
    {
        int pattern = 0, row = 0;
        if (! locate(k, pattern, row))
            continue;
        const float dist = static_cast<float>(k) - static_cast<float>(frac); // > 0 future, < 0 past
        const float y = playY - dist * kRowH;
        if (y < top - 1 || y + kRowH > bottom + 1)
            continue;
        if (row % rpb == 0)
            rects_.push_back({kRainX, y, kRainW, 1, argb(kRain, row % (rpb * 4) == 0 ? 0.16f : 0.07f)});
        for (int ch = first; ch < last; ++ch)
        {
            const Cell* cell = d.cell(pattern, row, ch);
            if (cell == nullptr)
                continue;
            const float x = x0 + colW * static_cast<float>(ch - first) + 4;
            const bool muted = (mutes & bit(ch)) || (solo != 0 && ! (solo & bit(ch)));
            const bool filt = filtered & bit(ch);
            const bool hasNote = cell->noteValue != 0;
            // Colour and strength by distance from the playhead.
            std::uint32_t noteInk = kRain, restInk = kRainDim;
            float noteA, restA;
            if (dist > 0) // falling
            {
                noteA = std::max(0.18f, 0.75f - 0.045f * dist);
                restA = std::max(0.10f, 0.40f - 0.03f * dist);
            }
            else // the flare at the playhead, then the trail
            {
                const float t = -dist;
                const float glow = std::exp(-t / 2.2f);
                noteInk = mix(kRain, mix(instrumentColour(cell->instrument), kWhite, 0.35f * glow), glow);
                noteA = 0.25f + 0.75f * glow;
                restA = 0.18f + 0.4f * glow;
                restInk = mix(kRainDim, kRain, glow);
                if (hasNote && t < 1.0f && ! muted)
                    rects_.push_back({x - 3, y, colW - 2, kRowH, argb(noteInk, 0.22f * (1.0f - t))});
            }
            if (filt)
                noteInk = mix(noteInk, kFiltered, 0.7f), restInk = mix(restInk, kFiltered, 0.5f);
            if (muted)
                noteInk = restInk = kMuted, noteA *= 0.45f, restA *= 0.45f;
            auto put = [&](float gx, char ch2, bool strong) {
                if (ch2 == ' ')
                    return;
                const bool dot = ch2 == '.';
                glyphs_.push_back({gx - 0.5f, y, kGlyphW, kRowH, ch2, argb(strong ? noteInk : restInk, dot ? restA * 0.5f : strong ? noteA : restA)});
            };
            if (density == 4)
            {
                put(x, hasNote ? (cell->noteValue <= 120 ? cell->note[0] : '|') : (cell->fx[0] != '.' ? ':' : '.'), hasNote);
                continue;
            }
            char text[16];
            int n = 0;
            for (char v : cell->note)
                text[n++] = v;
            if (density <= 2)
            {
                text[n++] = ' ';
                text[n++] = cell->inst[0];
                text[n++] = cell->inst[1];
            }
            if (density == 1)
            {
                text[n++] = ' ';
                for (char v : cell->vol)
                    text[n++] = v;
                text[n++] = ' ';
                for (char v : cell->fx)
                    text[n++] = v;
            }
            for (int i = 0; i < n; ++i)
                put(x + kCharW * static_cast<float>(i), text[i], i < 3 && hasNote);
        }
    }
    // The playhead: a thin line under the current row's band.
    overlay_.push_back({kRainX, playY + kRowH, kRainW, 1, argb(kWhite, 0.35f)});
    overlay_.push_back({kRainX, playY, kRainW, kRowH, argb(kWhite, 0.04f)});
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
                if (mode == PadMode::Filter && (filtered & bit(index)))
                    s = {(latched & bit(index)) && ! down ? LampMode::BlinkSlow : LampMode::On, io::lampcolour::red, 0};
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
        }
        f[io::pad(i + 1)] = s;
    }
    // Step keys: the order page; the playing order white, a queued one blinking, the others dim.
    const auto pos = player_.position();
    const int queued = player_.queuedOrder();
    for (int i = 0; i < 16; ++i)
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
    for (int i = 0; i < 4; ++i)
        f[modes[i]] = i == std::min(static_cast<int>(mode), 3)
                        ? LampState {mode == PadMode::Kit ? LampMode::BlinkSlow : LampMode::On, io::lampcolour::white, 0}
                        : LampState {};
    const int q = player_.quantize.load();
    for (int i = 0; i < 3; ++i)
        f[static_cast<C>(static_cast<int>(C::Bar1) + i)] = i == q ? LampState {LampMode::On, io::lampcolour::white, 0} : LampState {};
    f[C::Bar4] = player_.patternLoop.load() ? LampState {LampMode::On, io::lampcolour::white, 0} : LampState {};
    f[C::Scene] = f[C::Pattern] = {};
    // ANALOG FILTER ACTIVE: lit while audio goes through the filter loop (on the unit the lamp also drives the filter
    // board's select line, hw/imx6/AnalogBoard.h) and while the loop's latency is measured.
    const bool filterLit = player_.route() != FilterRoute::Off || player_.measuring();
    f[C::AfsActive] = filterLit ? LampState {LampMode::On, io::lampcolour::red, 0} : LampState {};
    io::stripLamps(0, false, false, io::StripMode::Pitch, f);
    f[C::StripModePitch] = {LampMode::On, io::lampcolour::white, 0};
}
} // namespace app
