// Host test of the player engine: every module in the given folder through app::Player, with a simulated analog
// filter loop (the send comes back on inputs 2/3 after kLoopDelay frames) and a recording MIDI scheduler.
// Checks playback, the loop latency measurement (and a re-measurement), quantized jumps, the stutter loop and its
// return in time, the filter routes, stale pad releases, mutes, the Kit pads, stop, and the MIDI clock and notes.
// Exit code 1 when a check fails.
#include "app/Library.h"
#include "app/Player.h"
#include "app/Song.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
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
    bool loop;
    std::vector<std::vector<float>> out;
    std::vector<float> in2, in3, ringL, ringR;
    float* outPtr[12] {};
    const float* inPtr[4] {};
    std::uint64_t t = 0, ns = 1000000000ull;
    double energy = 0, sendEnergy = 0, time = 0;
    long periods = 0, rowsSeen = 0;
    int lastOrder = -1, lastRow = -1;
    bool finite = true;

    explicit Rig(bool withLoop) : loop(withLoop), out(12, std::vector<float>(kPeriod)), in2(kPeriod), in3(kPeriod), ringL(1 << 16), ringR(1 << 16)
    {
        player.setMidi(&midi);
        player.prepare(kRate, kPeriod);
        player.armMeasure(); // no lamps here
        for (int i = 0; i < 12; ++i)
            outPtr[i] = out[static_cast<std::size_t>(i)].data();
        // No OUT 3-8 in the test; the send only with the loop.
        for (int p = 1; p <= 3; ++p)
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
    auto song = loadSong(e.path, true, kRate, 100, 4, error);
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
    auto again = loadSong(e.path, true, kRate, 100, 4, error2);
    auto third = loadSong(e.path, true, kRate, 100, 4, error2);
    CHECK(rig.player.load(std::move(again)), "second hand-over");
    CHECK(! rig.player.load(std::move(third)) && third != nullptr, "a hand-over while one is pending must keep the song");
    rig.run(0.02);
    rig.player.collect();
}

void testNoLoop(const LibraryEntry& e)
{
    std::printf("== no filter loop: %s\n", e.name.c_str());
    Rig rig(false);
    rig.run(0.05);
    CHECK(! rig.player.stems(), "stems without a loop");
    std::string error;
    auto song = loadSong(e.path, rig.player.stems(), kRate, 100, 4, error);
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
    std::printf(failures == 0 ? "ALL PASSED\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
