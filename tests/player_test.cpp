// Host test of the player engine: every module in the given folder through app::Player, with a simulated analog
// filter loop (the send comes back on inputs 2/3 after kLoopDelay frames) and a recording MIDI scheduler.
// Checks playback, the loop latency measurement, quantized jumps, the stutter loop, the filter routes, mutes, the Kit
// pads, stop, and the MIDI clock and notes. Exit code 1 when a check fails.
#include "app/Library.h"
#include "app/Player.h"
#include "app/Song.h"

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
    double energy = 0, time = 0;
    long periods = 0, rowsSeen = 0;
    int lastOrder = -1, lastRow = -1;
    bool finite = true;

    explicit Rig(bool withLoop) : loop(withLoop), out(12, std::vector<float>(kPeriod)), in2(kPeriod), in3(kPeriod), ringL(1 << 16), ringR(1 << 16)
    {
        player.setMidi(&midi);
        player.prepare(kRate, kPeriod);
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
                in2[static_cast<std::size_t>(i)] = ringL[at] * 0.9f;
                in3[static_cast<std::size_t>(i)] = ringR[at] * 0.9f;
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
    void pad(int pad0, int velocity = 100) { player.push({static_cast<std::uint8_t>(pad0), static_cast<std::uint8_t>(velocity), 60, static_cast<std::int8_t>(pad0)}); }
};

void testModule(const LibraryEntry& e)
{
    std::printf("== %s/%s\n", e.folder.c_str(), e.name.c_str());
    std::string error;
    auto song = loadSong(e.path, true, 100, 4, error);
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

    // The stutter loop: pad 4 = 4 rows.
    rig.player.padMode = static_cast<int>(PadMode::Loop);
    rig.run(0.02);
    const auto start = rig.player.position();
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
    rig.player.padRelease(3);
    rig.run(0.02);
    const auto after = rig.player.position();
    CHECK(! rig.player.loopActive(), "loop still active");
    CHECK(after.order != start.order || after.row >= start.row + 4, "released at order %d row %d, loop was %d/%d", after.order, after.row,
          start.order, start.row);
    std::printf("  loop: rows %d…%d, released to order %d row %d\n", minRow, maxRow, after.order, after.row);

    // The filter routes, from the start of the song (some end in silence).
    rig.player.jump(0, Quantize::Now);
    rig.run(0.5);
    rig.player.padMode = static_cast<int>(PadMode::Filter);
    rig.pad(0);
    const double filtered = rig.rms(1.0);
    CHECK(rig.player.route() == FilterRoute::Channels, "route %d with channel 1 held", static_cast<int>(rig.player.route()));
    CHECK(rig.player.filterChannels() == 1, "filter mask %llx", static_cast<unsigned long long>(rig.player.filterChannels()));
    rig.player.padRelease(0);
    rig.run(0.05);
    CHECK(rig.player.route() == FilterRoute::Off, "route after release %d", static_cast<int>(rig.player.route()));
    rig.player.masterFilter = true;
    const double master = rig.rms(1.0);
    CHECK(rig.player.route() == FilterRoute::Master, "master route");
    CHECK(master > 1e-4, "master route silent");
    rig.player.masterFilter = false;
    rig.run(0.05);
    std::printf("  filter: channels rms %.3f, master rms %.3f\n", filtered, master);

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
        rig.player.padRelease(p);
    rig.run(0.2);
    CHECK(rig.finite, "non-finite after Kit");

    // MIDI notes.
    rig.player.midiNotes = true;
    const int before = rig.midi.count(0x90, 0xF0);
    rig.run(4.0);
    const int notes = rig.midi.count(0x90, 0xF0) - before;
    rig.player.midiNotes = false;
    std::printf("  midi: %d note-ons in 4 s, %d clocks total\n", notes, rig.midi.count(0xF8));

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
    auto again = loadSong(e.path, true, 100, 4, error2);
    auto third = loadSong(e.path, true, 100, 4, error2);
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
    auto song = loadSong(e.path, rig.player.stems(), 100, 4, error);
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
