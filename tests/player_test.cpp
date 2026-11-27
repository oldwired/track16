// Host test of the player engine: every module in the given folder through app::Player, with a simulated analog
// filter loop (the send comes back on inputs 2/3 after kLoopDelay frames) and a recording MIDI scheduler.
// Checks playback, the loop latency measurement (and a re-measurement), quantized jumps, the stutter loop and its
// return in time, the filter routes, stale pad releases, mutes, the Kit pads, stop, and the MIDI clock and notes.
// Exit code 1 when a check fails.
#include "app/Library.h"
#include "app/Player.h"
#include "app/Song.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace app;

namespace
{
constexpr int kPeriod = 128, kLoopDelay = 300, kRate = 44100;
int failures = 0;

#define CHECK(cond, ...)                                                                                                 \
    do                                                                                                                   \
    {                                                                                                                    \
        if (! (cond))                                                                                                    \
        {                                                                                                                \
            ++failures;                                                                                                  \
            std::printf("  FAIL %s: ", #cond);                                                                           \
            std::printf(__VA_ARGS__);                                                                                    \
            std::printf("\n");                                                                                           \
        }                                                                                                                \
    } while (0)

struct Recorder final : MidiScheduler
{
    struct Message
    {
        std::uint64_t ns;
        std::uint8_t b[3];
        int size;
    };
    std::vector<Message> messages;
    bool schedule(std::uint64_t dueNs, const std::uint8_t* bytes, int size) noexcept override
    {
        Message m {dueNs, {0, 0, 0}, size};
        for (int i = 0; i < size && i < 3; ++i)
            m.b[i] = bytes[i];
        messages.push_back(m);
        return true;
    }
    int count(std::uint8_t status, std::uint8_t mask = 0xFF) const
    {
        int n = 0;
        for (const auto& m : messages)
            n += (m.b[0] & mask) == status ? 1 : 0;
        return n;
    }
};

struct Rig
{
    Player player;
    Recorder midi;
    bool loop, jacks;
    std::vector<std::vector<float>> out;
    std::vector<float> in2, in3, ringL, ringR;
    float* outPtr[12] {};
    const float* inPtr[4] {};
    std::uint64_t t = 0, ns = 1000000000ull;
    double energy = 0, sendEnergy = 0, time = 0;
    std::array<double, 6> jackEnergy {}; // OUT 3–8
    std::array<double, 12> portEnergy {}; // every output buffer
    double diff34 = 0;                   // the largest |MAIN − OUT 3-4| sample difference (compare34)
    bool compare34 = false;
    long periods = 0, rowsSeen = 0;
    int lastOrder = -1, lastRow = -1;
    bool finite = true;

    explicit Rig(bool withLoop, bool withJacks = false)
        : loop(withLoop), jacks(withJacks), out(12, std::vector<float>(kPeriod)), in2(kPeriod), in3(kPeriod), ringL(1 << 16), ringR(1 << 16)
    {
        player.setMidi(&midi);
        player.prepare(kRate, kPeriod);
        player.armMeasure(); // no lamps here
        for (int i = 0; i < 12; ++i)
            outPtr[i] = out[static_cast<std::size_t>(i)].data();
        // OUT 3-8 only when asked; the send only with the loop.
        for (int p = 1; p <= 3 && ! jacks; ++p)
            outPtr[2 * p] = outPtr[2 * p + 1] = nullptr;
        if (! loop)
            outPtr[10] = outPtr[11] = nullptr;
        inPtr[2] = loop ? in2.data() : nullptr;
        inPtr[3] = loop ? in3.data() : nullptr;
    }
    void run(double seconds)
    {
        const long n = static_cast<long>(seconds * kRate / kPeriod);
        for (long k = 0; k < n; ++k)
        {
            for (int i = 0; i < kPeriod; ++i) // the loop: what was sent kLoopDelay frames ago
            {
                const auto at = (t + static_cast<std::uint64_t>(i) - kLoopDelay) & 0xFFFF;
                in2[static_cast<std::size_t>(i)] = ringL[at] * 0.5f; // the send DAC's −6 dB, a unity analog path
                in3[static_cast<std::size_t>(i)] = ringR[at] * 0.5f;
            }
            for (auto& b : out)
                std::fill(b.begin(), b.end(), 0.0f);
            platform::AudioIO io {outPtr, 6, inPtr, 4, kPeriod, ns};
            const auto t0 = std::chrono::steady_clock::now();
            player.process(io);
            time += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            ++periods;
            if (const auto p = player.position(); p.order != lastOrder || p.row != lastRow)
            {
                rowsSeen += player.playing() ? 1 : 0;
                lastOrder = p.order;
                lastRow = p.row;
            }
            for (int i = 0; i < kPeriod; ++i)
            {
                const float l = out[0][static_cast<std::size_t>(i)], r = out[1][static_cast<std::size_t>(i)];
                finite = finite && std::isfinite(l) && std::isfinite(r);
                energy += static_cast<double>(l * l + r * r);
                if (jacks)
                    for (int j = 0; j < 6; ++j)
                    {
                        const float v = out[static_cast<std::size_t>(2 + j)][static_cast<std::size_t>(i)];
                        finite = finite && std::isfinite(v);
                        jackEnergy[static_cast<std::size_t>(j)] += static_cast<double>(v * v);
                    }
                for (std::size_t k = 0; k < 12; ++k)
                    portEnergy[k] += static_cast<double>(out[k][static_cast<std::size_t>(i)] * out[k][static_cast<std::size_t>(i)]);
                if (compare34)
                    diff34 = std::max({diff34, static_cast<double>(std::fabs(l - out[2][static_cast<std::size_t>(i)])),
                                       static_cast<double>(std::fabs(r - out[3][static_cast<std::size_t>(i)]))});
                if (loop)
                {
                    sendEnergy += static_cast<double>(out[10][static_cast<std::size_t>(i)] * out[10][static_cast<std::size_t>(i)]);
                    ringL[(t + static_cast<std::uint64_t>(i)) & 0xFFFF] = out[10][static_cast<std::size_t>(i)];
                    ringR[(t + static_cast<std::uint64_t>(i)) & 0xFFFF] = out[11][static_cast<std::size_t>(i)];
                }
            }
            t += kPeriod;
            ns += static_cast<std::uint64_t>(1e9 * kPeriod / kRate);
        }
    }
    double rms(double seconds)
    {
        energy = 0;
        run(seconds);
        return std::sqrt(energy / (2.0 * seconds * kRate));
    }
    std::array<std::uint16_t, 16> hits {}; // as the screen counts them
    void pad(int pad0, int velocity = 100)
    {
        ++hits[static_cast<std::size_t>(pad0)];
        player.push({static_cast<std::uint8_t>(pad0), static_cast<std::uint8_t>(velocity), 60, static_cast<std::int8_t>(pad0)});
    }
    void release(int pad0) { player.padRelease(pad0, hits[static_cast<std::size_t>(pad0)]); }
    void strip(int position, bool contact) { player.push(platform::stripTrigger(position, contact)); }
};

// Where `rows` rows after (order, row) is, in plain order (the player's idea of "where the song would be").
void advance(const SongData& d, int& order, int& row, long rows)
{
    auto valid = [&](int o) {
        for (int k = 0; k < 2 * d.numOrders + 2; ++k)
        {
            if (o < 0 || o >= d.numOrders || d.orderPattern(o) == -2)
                o = 0;
            if (d.orderPattern(o) >= 0)
                return o;
            ++o;
        }
        return 0;
    };
    for (; rows > 0; --rows)
        if (++row >= d.patternRows(d.orderPattern(order)))
            order = valid(order + 1), row = 0;
}

void testModule(const LibraryEntry& e)
{
    std::printf("== %s/%s\n", e.folder.c_str(), e.name.c_str());
    std::string error;
    auto song = loadSong(e.path, true, 0, kRate, 100, 4, error);
    CHECK(song != nullptr, "load: %s", error.c_str());
    if (! song)
        return;
    const auto data = song->data;
    std::printf("  %s \"%s\": %d ch, %d orders, %d patterns, %d instruments\n", data->type.c_str(), data->title.c_str(), data->channels,
                data->numOrders, data->numPatterns, std::max(data->numInstruments, data->numSamples));

    Rig rig(true);
    rig.run(0.5); // the loop latency measurement runs before anything plays
    CHECK(rig.player.latencyMeasured() && rig.player.latencyFrames() == kLoopDelay, "latency %d (measured %d), want %d",
          rig.player.latencyFrames(), rig.player.latencyMeasured() ? 1 : 0, kLoopDelay);
    CHECK(rig.player.load(std::move(song)), "hand-over");
    rig.player.play();

    // Playback: 8 s.
    rig.rowsSeen = 0;
    const double level = rig.rms(8.0);
    const auto pos = rig.player.position();
    CHECK(rig.player.playing(), "playing");
    CHECK(level > 1e-4, "silent output (rms %.6f)", level);
    CHECK(rig.finite, "non-finite samples");
    CHECK(pos.order > 0 || pos.row > 0, "position did not move");
    const int clocks = rig.midi.count(0xF8);
    CHECK(rig.midi.count(0xFA) == 1, "one MIDI start, got %d", rig.midi.count(0xFA));
    CHECK(std::abs(clocks - 6 * rig.rowsSeen) <= 12, "clock pulses %d for %ld rows", clocks, rig.rowsSeen);
    std::printf("  play: order %d row %d, rms %.3f, speed %d, bpm %.1f, row %.0f ms, clocks %d\n", pos.order, pos.row, level,
                rig.player.speed(), static_cast<double>(rig.player.bpm()), rig.player.rowSeconds() * 1000, clocks);

    // A jump at the end of the pattern.
    int target = -1; // a valid order in the middle (the last ones are often silent end patterns)
    for (int o = data->numOrders / 2; o < data->numOrders; ++o)
        if (o > rig.player.position().order + 1 && data->orderPattern(o) >= 0)
        {
            target = o;
            break;
        }
    if (target >= 0)
    {
        rig.player.jump(target, Quantize::Pattern);
        rig.run(0.05);
        CHECK(rig.player.queuedOrder() == target, "queued %d", rig.player.queuedOrder());
        bool reached = false;
        for (int k = 0; k < 400 && ! reached; ++k)
        {
            rig.run(0.05);
            reached = rig.player.position().order == target;
        }
        CHECK(reached, "jump to order %d not reached (at %d)", target, rig.player.position().order);
        CHECK(rig.player.position().row < 4, "jump landed on row %d", rig.player.position().row);
        std::printf("  jump: order %d row %d\n", rig.player.position().order, rig.player.position().row);
    }

    // The stutter loop: pad 4 = 4 rows; released, the song goes on where it would be (no MIDI stop/continue).
    rig.player.padMode = static_cast<int>(PadMode::Loop);
    rig.run(0.02);
    const auto start = rig.player.position();
    const long rows0 = rig.rowsSeen;
    const int stops0 = rig.midi.count(0xFC);
    rig.pad(3);
    int minRow = 1 << 20, maxRow = -1;
    bool sameOrder = true;
    for (int k = 0; k < 60; ++k)
    {
        rig.run(0.05);
        const auto p = rig.player.position();
        sameOrder = sameOrder && p.order == start.order;
        minRow = std::min(minRow, p.row);
        maxRow = std::max(maxRow, p.row);
    }
    CHECK(rig.player.loopActive(), "loop not active");
    CHECK(sameOrder && minRow >= start.row && maxRow < start.row + 4, "loop rows %d…%d from %d (order kept %d)", minRow, maxRow,
          start.row, sameOrder ? 1 : 0);
    rig.release(3);
    const auto inLoop = rig.player.position();
    for (int k = 0; k < 200 && rig.player.position().order == inLoop.order && rig.player.position().row == inLoop.row; ++k)
        rig.run(0.005);
    const auto after = rig.player.position();
    int wantOrder = start.order, wantRow = start.row;
    advance(*data, wantOrder, wantRow, rig.rowsSeen - rows0);
    CHECK(! rig.player.loopActive(), "loop still active");
    CHECK(after.order == wantOrder && after.row == wantRow, "released to order %d row %d, the song would be at %d/%d", after.order,
          after.row, wantOrder, wantRow);
    CHECK(rig.midi.count(0xFC) == stops0, "MIDI stop during the loop");
    std::printf("  loop: rows %d…%d, released to order %d row %d after %ld rows\n", minRow, maxRow, after.order, after.row,
                rig.rowsSeen - rows0);

    // A jump on the next row: no extra clock pulses (the follower stays on the row grid).
    rig.rowsSeen = 0;
    const int clocks0 = rig.midi.count(0xF8);
    if (target >= 0)
    {
        rig.player.jump(target, Quantize::Now);
        rig.run(1.0);
        CHECK(rig.player.position().order == target || rig.player.position().order == target + 1 || rig.player.position().row > 0,
              "jump now: at %d/%d", rig.player.position().order, rig.player.position().row);
        const int pulses = rig.midi.count(0xF8) - clocks0;
        CHECK(std::abs(pulses - 6 * rig.rowsSeen) <= 6, "clock pulses %d for %ld rows around a jump", pulses, rig.rowsSeen);
    }

    // The filter routes, from the start of the song (some end in silence).
    rig.player.jump(0, Quantize::Now);
    rig.run(0.5);
    rig.player.padMode = static_cast<int>(PadMode::Filter);
    rig.pad(0);
    const double filtered = rig.rms(1.0);
    CHECK(rig.player.route() == FilterRoute::Channels, "route %d with channel 1 held", static_cast<int>(rig.player.route()));
    CHECK(rig.player.filterChannels() == 1, "filter mask %llx", static_cast<unsigned long long>(rig.player.filterChannels()));
    // A release that arrives after the pad was hit again (the UI passes releases on late) keeps the newer hit.
    rig.pad(0);
    rig.player.padRelease(0, rig.hits[0] - 1);
    rig.run(0.05);
    CHECK(rig.player.filterChannels() == 1, "a stale release ended the newer hit");
    rig.release(0);
    rig.run(0.05);
    CHECK(rig.player.route() == FilterRoute::Off, "route after release %d", static_cast<int>(rig.player.route()));
    rig.player.masterFilter = true;
    const double master = rig.rms(1.0);
    CHECK(rig.player.route() == FilterRoute::Master, "master route");
    CHECK(master > 1e-4, "master route silent");
    // ACTIVE with a FILTER pad held: the held channel stays clean, every other channel goes round the loop.
    rig.pad(0);
    rig.sendEnergy = 0;
    rig.run(0.5);
    CHECK(rig.player.route() == FilterRoute::Channels, "inverse: route %d", static_cast<int>(rig.player.route()));
    if (rig.sendEnergy == 0) // nothing sent: only fine while the other channels are silent
    {
        float others = 0;
        for (int c = 1; c < data->channels; ++c)
            others = std::max(others, rig.player.channelLevel(c));
        CHECK(others < 0.01f, "inverse: nothing sent while other channels play (%.3f)", static_cast<double>(others));
    }
    rig.release(0);
    rig.run(0.05);
    CHECK(rig.player.route() == FilterRoute::Master, "inverse released: route %d", static_cast<int>(rig.player.route()));
    rig.player.masterFilter = false;
    rig.run(0.05);
    std::printf("  filter: channels rms %.3f, master rms %.3f\n", filtered, master);

    // DRIVE. Channels route: the send ×4 at full drive (+12 dB). Master route: nothing on the send (the board filters
    // the main path), the main output driven. OVER only while something is driven into the filter.
    {
        auto run1 = [&](int drive, double& mainRms) {
            rig.player.drive = drive;
            rig.player.jump(0, Quantize::Now);
            rig.run(0.2);
            rig.sendEnergy = 0;
            mainRms = rig.rms(1.0);
            return std::sqrt(rig.sendEnergy / kRate);
        };
        double m0 = 0, m1 = 0, mm0 = 0, mm1 = 0;
        rig.player.padMode = static_cast<int>(PadMode::Filter);
        rig.pad(0);
        const double s0 = run1(0, m0), s1 = run1(1023, m1);
        rig.release(0);
        rig.run(0.05);
        if (s0 > 1e-3)
            CHECK(s1 / s0 > 3.0 && s1 / s0 < 5.0, "drive ratio %.2f on the send (want about 4)", s1 / s0);
        rig.player.masterFilter = true;
        const double ms0 = run1(0, mm0), ms1 = run1(1023, mm1);
        const float overAtFull = rig.player.overLevel();
        CHECK(ms0 < 1e-6 && ms1 < 1e-6, "master route sends %.4f / %.4f (want nothing: the main path is filtered)", ms0, ms1);
        if (mm0 > 1e-3)
            CHECK(mm1 > mm0 * 1.3, "master drive: main rms %.3f -> %.3f", mm0, mm1);
        rig.player.drive = 0;
        rig.player.masterFilter = false;
        rig.run(0.8);
        CHECK(! rig.player.over(), "OVER with no route on (level %.2f)", static_cast<double>(rig.player.overLevel()));
        std::printf("  drive: send rms %.3f -> %.3f, master main rms %.3f -> %.3f, OVER level %.2f\n", s0, s1, mm0, mm1,
                    static_cast<double>(overAtFull));
    }

    // The strip. REPEAT: the top segment loops 1 row, the bottom one 16 (or what is left of the pattern); release returns.
    {
        rig.player.jump(0, Quantize::Now);
        rig.run(0.3);
        rig.player.setStripMode(Player::StripRepeat);
        rig.strip(127, true);
        rig.run(0.2);
        CHECK(rig.player.loopActive() && rig.player.loopLength() == 1, "strip repeat top: active %d length %d",
              rig.player.loopActive() ? 1 : 0, rig.player.loopLength());
        rig.strip(0, true);
        rig.run(0.1);
        CHECK(rig.player.loopActive() && rig.player.loopLength() > 1, "strip repeat bottom: length %d", rig.player.loopLength());
        rig.strip(0, false);
        rig.run(0.1);
        CHECK(! rig.player.loopActive(), "strip repeat: loop after release");
        // HOLD keeps it until HOLD goes off.
        rig.player.setStripHold(true);
        rig.strip(100, true);
        rig.run(0.1);
        rig.strip(100, false);
        rig.run(0.3);
        CHECK(rig.player.loopActive() && rig.player.stripLatched(), "strip hold: loop %d latched %d", rig.player.loopActive() ? 1 : 0,
              rig.player.stripLatched() ? 1 : 0);
        rig.player.setStripHold(false);
        rig.run(0.1);
        CHECK(! rig.player.loopActive() && ! rig.player.stripLatched(), "strip hold off: loop still on");
        // USER2: volume, bottom = silent.
        rig.player.setStripMode(Player::StripVolume);
        rig.strip(0, true);
        rig.run(0.05);
        const double quiet = rig.rms(0.5);
        rig.strip(0, false);
        rig.run(0.05);
        CHECK(quiet < 1e-4, "strip volume at the bottom: rms %.5f", quiet);
        CHECK(rig.player.stripMode() == Player::StripVolume, "strip mode %d", rig.player.stripMode());
        rig.player.setStripMode(Player::StripPitch);
        rig.run(0.02);
        std::printf("  strip: repeat, hold and volume ok\n");
    }

    // Mutes: everything muted is (nearly) silent.
    rig.player.muteMask = ~0ull;
    rig.run(0.3);
    const double muted = rig.rms(1.0);
    CHECK(muted < level * 0.2 + 1e-3, "muted rms %.4f vs %.4f", muted, level); // short click-removal tails leak
    rig.player.muteMask = 0;
    rig.run(0.1);

    // Kit pads while playing.
    rig.player.padMode = static_cast<int>(PadMode::Kit);
    for (int p = 0; p < 4; ++p)
        rig.pad(p, 120);
    rig.run(0.5);
    for (int p = 0; p < 4; ++p)
        rig.release(p);
    rig.run(0.2);
    CHECK(rig.finite, "non-finite after Kit");

    // MIDI notes.
    rig.player.midiNotes = true;
    const int before = rig.midi.count(0x90, 0xF0);
    rig.run(4.0);
    const int notes = rig.midi.count(0x90, 0xF0) - before;
    rig.player.midiNotes = false;
    rig.run(0.05);
    CHECK(rig.midi.count(0x90, 0xF0) == rig.midi.count(0x80, 0xF0), "REC off left notes on (%d on, %d off)", rig.midi.count(0x90, 0xF0),
          rig.midi.count(0x80, 0xF0));
    std::printf("  midi: %d note-ons in 4 s, %d clocks total\n", notes, rig.midi.count(0xF8));

    // A re-measurement of the loop (no route on): same latency, the song plays on.
    const int measured = rig.player.measurements();
    rig.player.remeasure();
    rig.run(0.5);
    CHECK(rig.player.measurements() == measured + 1 && rig.player.latencyFrames() == kLoopDelay, "re-measured %d times, latency %d",
          rig.player.measurements() - measured, rig.player.latencyFrames());

    // Turning INTERP costs no more than a fraction of a period (libopenmpt used to rebuild its FIR table each time).
    rig.player.takeLoadMax();
    for (int taps : {1, 2, 4, 8, 1, 4})
    {
        rig.player.interpolation = taps;
        rig.run(0.01);
    }
    const float interpLoad = rig.player.takeLoadMax();
    CHECK(interpLoad < 0.5f, "INTERP changes took %.0f %% of a period", static_cast<double>(interpLoad * 100));

    // MIDI timestamps never go backwards by more than the latency window.
    bool ordered = true;
    for (std::size_t i = 1; i < rig.midi.messages.size(); ++i)
        if (rig.midi.messages[i].ns + 50000000ull < rig.midi.messages[i - 1].ns)
            ordered = false;
    CHECK(ordered, "MIDI timestamps out of order");

    // Stop.
    rig.player.stop();
    rig.run(0.05);
    CHECK(! rig.player.playing() && rig.player.position().order == 0 && rig.player.position().row == 0, "stop: playing %d at %d/%d",
          rig.player.playing() ? 1 : 0, rig.player.position().order, rig.player.position().row);
    CHECK(rig.midi.count(0xFC) >= 1, "MIDI stop");
    std::printf("  engine: %.1f %% of real time on this machine (%ld periods)\n",
                100.0 * rig.time / (static_cast<double>(rig.periods) * kPeriod / kRate), rig.periods);

    // The hand-over: a second song replaces the first, the first comes back for deletion.
    std::string error2;
    auto again = loadSong(e.path, true, 0, kRate, 100, 4, error2);
    auto third = loadSong(e.path, true, 0, kRate, 100, 4, error2);
    CHECK(rig.player.load(std::move(again)), "second hand-over");
    CHECK(! rig.player.load(std::move(third)) && third != nullptr, "a hand-over while one is pending must keep the song");
    rig.run(0.02);
    rig.player.collect();
}

// The single outs: a channel on OUT 3-4 leaves MAIN; with +MAIN both carry the same signal (also through a stutter
// loop: the instances move in lockstep); a mono jack; a full pool; the clean mix and the click; the output
// test tone; a channel only on a single out does not start the filter route.
void testOuts(const LibraryEntry& e)
{
    std::printf("== single outs: %s\n", e.name.c_str());
    std::string error;
    auto song = loadSong(e.path, true, 2, kRate, 100, 4, error);
    CHECK(song && song->outs.size() == 2 && song->instances() == 4, "load with 2 out stems: %s", error.c_str());
    if (! song)
        return;
    const auto data = song->data;
    Rig rig(true, true);
    rig.run(0.5);
    rig.player.load(std::move(song));
    rig.player.play();
    // The loudest channel over the first seconds.
    std::vector<float> loud(static_cast<std::size_t>(data->channels), 0.0f);
    for (int k = 0; k < 60; ++k)
    {
        rig.run(0.05);
        for (int c = 0; c < std::min(data->channels, kMaxChannels); ++c)
            loud[static_cast<std::size_t>(c)] = std::max(loud[static_cast<std::size_t>(c)], rig.player.channelLevel(c));
    }
    const int ch = static_cast<int>(std::max_element(loud.begin(), loud.end()) - loud.begin());
    auto jackRms = [&](int j) { return std::sqrt(rig.jackEnergy[static_cast<std::size_t>(j)] / (kRate * 2.0)); };
    auto measure2s = [&](double& mainRms) {
        rig.jackEnergy.fill(0);
        mainRms = rig.rms(2.0);
    };
    rig.player.jump(0, Quantize::Now);
    rig.player.soloMask = 1ull << ch;
    rig.run(0.3);
    double solo = 0, onOut = 0, both = 0;
    measure2s(solo);

    outs::Layout l;
    l.dest[static_cast<std::size_t>(ch)] = outs::pair(0);
    rig.player.setOuts(l);
    rig.player.jump(0, Quantize::Now);
    rig.run(0.3);
    measure2s(onOut);
    const double j34 = jackRms(0) + jackRms(1);
    CHECK(rig.player.outStemsUsed() == 1 && rig.player.outsUnserved() == 0, "stems used %d, unserved %x", rig.player.outStemsUsed(),
          rig.player.outsUnserved());
    CHECK(onOut < solo * 0.05 + 1e-4, "channel %d still on MAIN: rms %.4f (solo %.4f)", ch + 1, onOut, solo);
    CHECK(j34 > 1e-4 || solo < 1e-4, "OUT 3-4 silent (MAIN had %.4f)", solo);
    CHECK(jackRms(2) + jackRms(3) + jackRms(4) + jackRms(5) < 1e-6, "other jacks carry sound");

    // +MAIN: MAIN and OUT 3-4 the same, also across a stutter loop and its return.
    l.alsoMain = 1ull << ch;
    rig.player.setOuts(l);
    rig.run(0.3);
    rig.compare34 = true;
    rig.diff34 = 0;
    measure2s(both);
    rig.player.padMode = static_cast<int>(PadMode::Loop);
    rig.pad(2);
    rig.run(0.6);
    rig.release(2);
    rig.run(0.6);
    rig.compare34 = false;
    // Not bit-exact right after a channel moved: what only advances while a voice is mixed (the resampler's history,
    // volume ramps) differs until its next note. Drifting instances would differ by the signal itself (0.1 … 0.6).
    CHECK(rig.diff34 < 0.03, "+MAIN: MAIN and OUT 3-4 differ by %.6f", rig.diff34);
    CHECK(both > 1e-4 || solo < 1e-4, "+MAIN: MAIN silent");

    // A filter pad on a channel that is only on a single out: no filter route.
    l.alsoMain = 0;
    rig.player.setOuts(l);
    rig.player.padMode = static_cast<int>(PadMode::Filter);
    rig.pad(ch % 16);
    rig.run(0.2);
    if (ch < 16)
        CHECK(rig.player.route() == FilterRoute::Off, "filter route %d for a channel only on OUT 3-4", static_cast<int>(rig.player.route()));
    rig.release(ch % 16);
    rig.run(0.05);

    // Mono on OUT 5: only that jack.
    l.dest[static_cast<std::size_t>(ch)] = outs::mono(2);
    rig.player.setOuts(l);
    rig.player.jump(0, Quantize::Now);
    rig.run(0.3);
    double dummy = 0;
    measure2s(dummy);
    CHECK(jackRms(2) > 1e-4 || solo < 1e-4, "OUT 5 silent");
    CHECK(jackRms(0) + jackRms(1) + jackRms(3) < 1e-6, "mono: other jacks carry sound (%.5f %.5f %.5f)", jackRms(0), jackRms(1), jackRms(3));

    // A full pool: three destinations, two instances.
    if (data->channels >= 3)
    {
        outs::Layout full;
        for (int c = 0; c < 3; ++c)
            full.dest[static_cast<std::size_t>(c)] = outs::mono(c);
        rig.player.setOuts(full);
        rig.run(0.05);
        CHECK(rig.player.outStemsUsed() == 2 && __builtin_popcount(rig.player.outsUnserved()) == 1, "full pool: used %d unserved %x",
              rig.player.outStemsUsed(), rig.player.outsUnserved());
    }

    // The clean mix on OUT 3-4 (no route on, MAIN VOLUME full): the same as MAIN. The click on OUT 8.
    rig.player.soloMask = 0;
    outs::Layout util;
    util.source[0] = util.source[1] = outs::Source::Clean;
    util.source[5] = outs::Source::Click;
    rig.player.setOuts(util);
    rig.run(0.3);
    rig.compare34 = true;
    rig.diff34 = 0;
    measure2s(dummy);
    rig.compare34 = false;
    CHECK(rig.diff34 < 1e-5, "clean mix differs from MAIN by %.6f", rig.diff34);
    CHECK(jackRms(5) > 1e-3, "no click on OUT 8 (rms %.5f)", jackRms(5));
    std::printf("  outs: channel %d, solo %.4f, on 3-4 %.4f (MAIN %.5f), click rms %.4f\n", ch + 1, solo, j34, onOut, jackRms(5));

    // The output test, with the song stopped: the tone on OUT 4 alone, then on PHONES R alone, then off again.
    rig.player.setOuts(outs::Layout {});
    rig.player.stop();
    rig.run(0.1);
    auto toneOn = [&](int output) {
        rig.player.testTone(output);
        rig.portEnergy.fill(0);
        rig.run(0.5);
        int loud = -1, count = 0;
        for (int k = 0; k < 10; ++k)
            if (rig.portEnergy[static_cast<std::size_t>(k)] > 1.0)
                loud = k, ++count;
        CHECK(count == 1 && loud == output && rig.player.toneOutput() == output, "tone on %d: loud %d (%d outputs), playing %d", output, loud,
              count, rig.player.toneOutput());
    };
    toneOn(3);
    toneOn(9);
    rig.player.testTone(9); // again: off
    rig.run(0.05);
    rig.portEnergy.fill(0);
    rig.run(0.2);
    CHECK(rig.player.toneOutput() == -1 && rig.portEnergy[9] < 1e-9, "tone not off (%d)", rig.player.toneOutput());
    rig.player.testTone(0);
    rig.run(3.2);
    CHECK(rig.player.toneOutput() == -1, "tone still on after 3.2 s");
    CHECK(rig.finite, "non-finite samples");
}

void testLayoutText()
{
    std::printf("== outs layout text\n");
    outs::Layout l;
    l.dest[0] = outs::pair(0);
    l.dest[1] = outs::mono(2);
    l.dest[6] = outs::pair(2);
    l.alsoMain = 1ull << 6;
    l.source[4] = outs::Source::Return;
    const std::string t = outs::format(l);
    CHECK(t == "src=ch,ch,ch,ch,ret,ch 1=3-4 2=5 7=7-8+", "format: %s", t.c_str());
    CHECK(outs::parse(t) == l, "parse(format) differs");
    CHECK(outs::tapJack(outs::kMain, 0) == outs::mono(0) && outs::tapJack(outs::mono(0), 1) == outs::pair(0)
              && outs::tapJack(outs::pair(0), 0) == outs::mono(1) && outs::tapJack(outs::mono(1), 1) == outs::kMain,
          "tap rules");
}

// An external MIDI clock: pulses at `bpm` with ±0.4 ms arrival jitter, fed to the player before each period whose
// start they precede. Rows (their start at the jacks, rowStartNs) are checked against the grid of every 6th pulse
// (rows_per_beat 4) counted from the first pulse after Start.
struct Clock
{
    Rig& rig;
    double bpm = 120, next = 0; // the next pulse's true time (ns)
    int ppr = 6;                // pulses per grid row (24 / rows per beat)
    long sent = 0;
    unsigned seed = 1;
    int port = 0;
    double period() const { return 60e9 / (24.0 * bpm); }
    double jitter() { return (static_cast<double>((seed = seed * 1103515245u + 12345u) >> 16 & 0x7FFF) / 32767.0 - 0.5) * 0.8e6; }
    void message(std::uint8_t status, int value = 0) { rig.player.midiRealtime(status, rig.ns, port, value); }
    // Runs `seconds` period by period; pulses only while `ticking`. Returns the largest |row start − nearest grid
    // point| (ns) of the rows that started after `settle` seconds, and the rows seen then.
    std::pair<double, long> run(double seconds, double settle = 1e9, bool ticking = true, double gridOrigin = -1)
    {
        double worst = 0;
        long rows = 0;
        std::uint64_t lastStart = rig.player.rowStartNs();
        const double end = static_cast<double>(rig.ns) + seconds * 1e9, from = static_cast<double>(rig.ns) + settle * 1e9;
        while (static_cast<double>(rig.ns) < end)
        {
            while (ticking && next <= static_cast<double>(rig.ns))
            {
                rig.player.midiRealtime(0xF8, static_cast<std::uint64_t>(next + jitter()), port);
                next += period();
                ++sent;
            }
            rig.run(1.5 * kPeriod / kRate); // one period
            const std::uint64_t start = rig.player.rowStartNs();
            if (start != lastStart && rig.player.playing() && static_cast<double>(rig.ns) > from && gridOrigin >= 0)
            {
                const double grid = ppr * period();
                double e = std::fmod(static_cast<double>(start) - gridOrigin, grid);
                e = e > grid / 2 ? e - grid : e < -grid / 2 ? e + grid : e;
                worst = std::max(worst, std::fabs(e));
                ++rows;
            }
            lastStart = start;
        }
        return {worst, rows};
    }
};

void testSync(const LibraryEntry& e)
{
    std::printf("== MIDI clock in: %s\n", e.name.c_str());
    Rig rig(false);
    rig.run(0.05);
    std::string error;
    auto song = loadSong(e.path, false, 0, kRate, 100, 4, error);
    if (e.name == "2nd_pm.s3m") // speed 3, tempo 130 in the header
        CHECK(song && std::fabs(song->data->rowSeconds - 2.5 * 3 / 130) < 1e-4, "typical row %.4f s", song ? song->data->rowSeconds : 0.0);
    const double rowSeconds = song ? song->data->rowSeconds : 0.0;
    std::printf("  typical row %.1f ms\n", rowSeconds * 1e3);
    rig.player.load(std::move(song));
    rig.run(0.05);
    Clock clock {rig};
    clock.next = static_cast<double>(rig.ns) + 1e6;
    clock.run(0.5, 1e9, true); // a running clock before Start: nothing plays
    CHECK(! rig.player.playing() && rig.player.syncState() == Player::SyncInternal, "playing before Start");
    CHECK(std::fabs(rig.player.clockBpm() - 120.0f) < 0.5f, "clock bpm %.2f", static_cast<double>(rig.player.clockBpm()));

    // Start: from the top with the next pulse; the rows lock onto the grid of that pulse; no clock of our own.
    const int clocksOut = rig.midi.count(0xF8);
    clock.message(0xFA);
    const double origin = clock.next; // the first pulse after Start
    auto [worst, rows] = clock.run(10.0, 4.0, true, origin);
    CHECK(rig.player.playing() && rig.player.syncState() == Player::SyncLocked, "not following after Start (state %d)", rig.player.syncState());
    CHECK(worst < 3e6, "rows off the clock's grid by %.2f ms", worst / 1e6);
    CHECK(rows > 6 * 8 / 2 && rows <= 6 * 8 * 2 + 2, "%ld rows in 6 s at 120 BPM", rows); // pattern delays make fewer
    CHECK(rig.midi.count(0xF8) == clocksOut, "sent %d clocks while following", rig.midi.count(0xF8) - clocksOut);
    std::printf("  120 BPM: worst %.2f ms over %ld rows, error now %.2f ms\n", worst / 1e6, rows, static_cast<double>(rig.player.syncErrorMs()));

    // The master speeds up: the grid keeps its phase (the pulses go on), the rows follow.
    const double sinceOrigin = (clock.next - origin) / clock.period(); // pulses since the origin
    clock.bpm = 141;
    const double origin2 = clock.next - std::fmod(sinceOrigin, 6.0) * clock.period(); // the grid point before the next pulse
    std::tie(worst, rows) = clock.run(8.0, 4.0, true, origin2);
    CHECK(worst < 3e6 && std::fabs(rig.player.clockBpm() - 141.0f) < 0.5f, "141 BPM: worst %.2f ms, clock %.2f", worst / 1e6,
          static_cast<double>(rig.player.clockBpm()));
    std::printf("  141 BPM: worst %.2f ms over %ld rows\n", worst / 1e6, rows);

    // 8 rows per beat while following (TEMPO with a clock): twice the rows on the same beats, then back to 4.
    for (const int rpb : {8, 4})
    {
        if (rowSeconds * rpb * 141 / 60 > 3.5) // more than the tempo factor's 4× (slow modules: their TEMPO knob stops lower)
        {
            std::printf("  %d rows per beat: skipped, %.1f× tempo\n", rpb, rowSeconds * rpb * 141 / 60);
            continue;
        }
        rig.player.rowsPerBeat = rpb;
        clock.ppr = 24 / rpb;
        std::tie(worst, rows) = clock.run(6.0, 3.0, true, origin2);
        CHECK(worst < 3e6, "%d rows per beat: worst %.2f ms", rpb, worst / 1e6);
        std::printf("  %d rows per beat: worst %.2f ms over %ld rows\n", rpb, worst / 1e6, rows);
    }

    // Stop pauses where it is; Song Position moves it; Continue goes on from there with the next pulse.
    clock.message(0xFC);
    clock.run(0.2);
    CHECK(! rig.player.playing() && rig.player.syncState() == Player::SyncInternal, "Stop: playing %d state %d", rig.player.playing(),
          rig.player.syncState());
    clock.message(0xF2, 16); // 16 sixteenths = 16 rows at 4 rows per beat
    clock.run(0.05);
    int order = 0, row = 0;
    advance(*rig.player.current(), order, row, 16);
    CHECK(rig.player.position().order == order && rig.player.position().row == row, "song position → %d/%d, want %d/%d",
          rig.player.position().order, rig.player.position().row, order, row);
    clock.message(0xFB);
    clock.run(1.0);
    CHECK(rig.player.playing() && rig.player.syncState() == Player::SyncLocked, "Continue: playing %d", rig.player.playing());

    // The clock stops without a Stop: it plays on at the last tempo (state "lost"); the panel's STOP ends following.
    clock.run(1.0, 1e9, false);
    CHECK(rig.player.playing() && rig.player.syncState() == Player::SyncLost, "clock gone: playing %d state %d", rig.player.playing(),
          rig.player.syncState());
    rig.player.stop();
    clock.run(0.1, 1e9, false);
    CHECK(! rig.player.playing() && rig.player.syncState() == Player::SyncInternal, "panel STOP while following");

    // Our own Start coming back (a MIDI thru) while we play on our own clock is ignored.
    rig.player.play();
    clock.run(0.3, 1e9, false);
    clock.message(0xFA);
    clock.run(0.3, 1e9, false);
    CHECK(rig.player.playing() && rig.player.syncState() == Player::SyncInternal, "an echoed Start took over");
    rig.player.stop();
    clock.run(0.1, 1e9, false);

    // PLAY while a clock runs: joins it on its next pulse (no grid known after the gap) and follows.
    clock.next = static_cast<double>(rig.ns) + 1e6;
    clock.run(0.3);
    rig.player.play();
    clock.run(2.0);
    CHECK(rig.player.playing() && rig.player.syncState() == Player::SyncLocked, "PLAY with a clock: state %d", rig.player.syncState());
    rig.player.stop();

    // midi_sync=0: Start does nothing.
    rig.player.midiSync = false;
    clock.run(0.2);
    clock.message(0xFA);
    clock.run(0.5);
    CHECK(! rig.player.playing(), "Start with midi_sync=0");
    CHECK(rig.finite, "non-finite");
}

// MIDI notes: by channel they sound where that module channel plays (here: OUT 3, all channels muted so only the
// live voice is heard), the kit channel plays instrument 1 on MAIN, the control channel plays nothing; bend and the
// mod wheel keep it finite; all-notes-off ends them.
void testMidiNotes(const LibraryEntry& e)
{
    std::printf("== MIDI notes in: %s\n", e.name.c_str());
    Rig rig(false, true);
    rig.run(0.05);
    std::string error;
    rig.player.load(loadSong(e.path, false, 2, kRate, 100, 4, error));
    outs::Layout layout;
    layout.dest[0] = outs::mono(0);
    rig.player.setOuts(layout);
    rig.player.muteMask = ~0ull;
    rig.player.play();
    rig.run(0.5);
    const auto quiet = [&](double s) {
        rig.jackEnergy = {};
        rig.energy = 0;
        rig.run(s);
        return std::make_pair(rig.energy, rig.jackEnergy[0]);
    };
    const auto [main0, jack0] = quiet(0.3);
    CHECK(main0 < 1e-6 && jack0 < 1e-6, "muted module not silent (%.3g, %.3g)", main0, jack0);

    rig.player.midiNote(0, 60, 120); // MIDI channel 1 → module channel 1 → OUT 3
    const auto [main1, jack1] = quiet(0.3);
    CHECK(jack1 > 1e-3 && main1 < 1e-6, "channel 1's note: OUT 3 %.3g, MAIN %.3g", jack1, main1);
    rig.player.midiBend(0, 16383);
    rig.player.midiMod(0, 127);
    quiet(0.3);
    rig.player.midiNote(0, 60, 0);
    rig.player.midiBend(0, 8192);
    rig.player.midiMod(0, 0);
    quiet(1.0);

    rig.player.midiNote(9, 36, 120); // the kit channel: instrument 1, on MAIN
    const auto [main2, jack2] = quiet(0.3);
    CHECK(main2 > 1e-3, "kit note on MAIN %.3g", main2);
    rig.player.midiAllNotesOff();
    quiet(1.5);
    const auto [main3, jack3] = quiet(0.3);
    CHECK(main3 < 1e-4 && jack3 < 1e-4, "after all notes off: MAIN %.3g OUT 3 %.3g", main3, jack3);

    rig.player.midiNote(15, 60, 120); // the control channel: the UI's remote pads, no note here
    const auto [main4, jack4] = quiet(0.3);
    CHECK(main4 < 1e-6 && jack4 < 1e-6, "control channel played (%.3g, %.3g)", main4, jack4);
    rig.player.midiNote(15, 60, 0);
    CHECK(rig.finite, "non-finite");
}

void testNoLoop(const LibraryEntry& e)
{
    std::printf("== no filter loop: %s\n", e.name.c_str());
    Rig rig(false);
    rig.run(0.05);
    CHECK(! rig.player.stems(), "stems without a loop");
    std::string error;
    auto song = loadSong(e.path, rig.player.stems(), 0, kRate, 100, 4, error);
    CHECK(song && ! song->wet, "load without stems");
    rig.player.load(std::move(song));
    rig.player.play();
    rig.player.padMode = static_cast<int>(PadMode::Filter);
    rig.pad(0);
    const double level = rig.rms(3.0);
    CHECK(level > 1e-4 && rig.finite, "rms %.5f", level);
    CHECK(rig.player.route() == FilterRoute::Off, "route without loop");
}
} // namespace

int main(int argc, char** argv)
{
    Library library;
    library.scan({argc > 1 ? argv[1] : "modules"});
    std::printf("%d modules\n", static_cast<int>(library.entries().size()));
    if (library.entries().empty())
        return 1;
    for (const auto& e : library.entries())
        testModule(e);
    testNoLoop(library.entries().front());
    testLayoutText();
    for (const auto& e : library.entries())
        testOuts(e);
    for (const auto& e : library.entries())
        testSync(e);
    testMidiNotes(library.entries().front());
    std::printf(failures == 0 ? "ALL PASSED\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
