// The player: libopenmpt rendering on the audio thread, the live controls, the analog filter routing and the MIDI
// clock/notes. It is the platform's AudioProcessor (process() once per audio period) and its TriggerSink (pad hits
// arrive straight from the panel's reader thread, so a pad acts one panel frame after it is hit).
//
// Threads
//   message thread   load() hands a Song over, collect() deletes the one it replaced; commands (play, jump, …) go
//                    through a lock-free queue; settings are atomics read once per period
//   reader threads   push(): pad hits and pressure, strip moves
//   audio thread     everything that touches libopenmpt; publishes positions, levels and states as atomics
//
// Rendering: in chunks of kChunk frames, so a row change is seen within 0.7 ms; quantized actions (jumps, the
// pattern loop, the stutter loop) happen at those row changes.
//
// Analog filter (with the filter loop's FilterSend port and the return on inputs 2/3): channels that go through the
// filter play on the song's "wet" instance, the rest on the "dry" one (complementary mute masks). Routes:
//   off       MAIN = delay(dry + wet)                SEND = silence
//   channels  MAIN = delay(dry) + return             SEND = wet
//   master    MAIN = return                          SEND = dry + wet
// delay() = the loop's measured latency, so the direct path stays aligned with the filtered one. Route changes
// cross-fade over kFade frames.
#pragma once

#include "app/Queue.h"
#include "app/Song.h"

#include "Platform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace app
{
enum class PadMode : int { Filter, Mute, Loop, Play, Kit };
enum class Quantize : int { Pattern, Beat, Now };
enum class FilterRoute : int { Off, Channels, Master };

// Where the audio thread sends MIDI: timestamped messages (CLOCK_MONOTONIC ns), never blocking.
class MidiScheduler
{
public:
    virtual ~MidiScheduler() = default;
    virtual bool schedule(std::uint64_t dueNs, const std::uint8_t* bytes, int size) noexcept = 0;
};

class Player final : public platform::AudioProcessor, public platform::TriggerSink
{
public:
    static constexpr int kChunk = 32;
    static constexpr int kFade = 256;
    static constexpr int kMaxDelay = 8192;
    static constexpr int kLoopLengths[16] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 0, 0, 0, 0};

    Player();
    ~Player() override;

    // ---- message thread ----
    bool load(std::unique_ptr<Song>&& song); // takes the song; false (song left with the caller) while a hand-over is pending
    void collect();                        // deletes songs the audio thread let go of
    std::shared_ptr<const SongData> current() const { return current_; }
    bool stems() const noexcept { return stems_.load(std::memory_order_relaxed); } // the filter loop is there
    void setMidi(MidiScheduler* m) noexcept { midi_ = m; }
    void setFixedLatency(int frames) noexcept // the filter loop's latency from the settings (no measurement)
    {
        latency_.store(std::clamp(frames, 1, kMaxDelay - 1));
        latencyMeasured_.store(true);
    }

    void play() noexcept;
    void pause() noexcept;
    void togglePlay() noexcept;
    void stop() noexcept; // pause and rewind
    void jump(int order, Quantize q) noexcept;
    void padRelease(int pad0) noexcept;
    void toggleLoopLatch() noexcept;
    void toggleFilterLatch(int channel) noexcept;
    void midiNote(int note, int velocity) noexcept; // MIDI in: plays the selected instrument

    // Settings (message thread writes, audio thread reads once per period).
    std::atomic<int> padMode {static_cast<int>(PadMode::Mute)};
    std::atomic<int> page {0};             // pads 1–16 = channels / instruments page × 16 + 1 …
    std::atomic<int> instrument {0};       // 0-based, for the Play pad mode and MIDI in
    std::atomic<int> playRoot {60};        // pad 1 in the Play mode (60 = C-5)
    std::atomic<int> quantize {static_cast<int>(Quantize::Pattern)};
    std::atomic<bool> patternLoop {false};
    std::atomic<bool> masterFilter {false};
    std::atomic<std::uint64_t> muteMask {0}, soloMask {0};
    std::atomic<float> tempoFactor {1.0f}, tempoBend {1.0f}, pitchSemitones {0.0f};
    std::atomic<float> gainDb {0.0f};      // module gain
    std::atomic<float> mainVolume {1.0f};  // the MAIN VOLUME pot, 0…1
    std::atomic<int> stereoSeparation {100}, interpolation {8};
    std::atomic<int> rowsPerBeat {4};
    std::atomic<bool> midiClock {true}, midiNotes {false}, midiFollowMute {true};
    std::atomic<int> midiOffsetMs {0};
    std::atomic<int> audioLatencyFrames {0}; // the output buffer (period × periods), for MIDI timing

    // ---- published by the audio thread ----
    struct Position
    {
        int order = 0, row = 0, pattern = 0;
    };
    Position position() const noexcept;
    std::uint64_t rowStartNs() const noexcept { return rowStartNs_.load(std::memory_order_acquire); }
    double rowSeconds() const noexcept { return rowSeconds_.load(std::memory_order_relaxed); }
    bool playing() const noexcept { return playing_.load(std::memory_order_relaxed); }
    int speed() const noexcept { return speed_.load(std::memory_order_relaxed); }
    float bpm() const noexcept { return bpm_.load(std::memory_order_relaxed); }
    int queuedOrder() const noexcept { return queuedOrder_.load(std::memory_order_relaxed); }
    bool loopActive() const noexcept { return loopActive_.load(std::memory_order_relaxed); }
    bool loopLatched() const noexcept { return loopLatched_.load(std::memory_order_relaxed); }
    int loopLength() const noexcept { return loopLength_.load(std::memory_order_relaxed); }
    std::uint64_t filterChannels() const noexcept { return filterOn_.load(std::memory_order_relaxed); }
    std::uint64_t filterLatched() const noexcept { return filterLatch_.load(std::memory_order_relaxed); }
    FilterRoute route() const noexcept { return static_cast<FilterRoute>(route_.load(std::memory_order_relaxed)); }
    float channelLevel(int ch) const noexcept { return ch >= 0 && ch < kMaxChannels ? vu_[static_cast<std::size_t>(ch)].load(std::memory_order_relaxed) : 0.0f; }
    std::uint64_t padHeld() const noexcept { return padHeld_.load(std::memory_order_relaxed); }
    int latencyFrames() const noexcept { return latency_.load(std::memory_order_relaxed); }
    bool latencyMeasured() const noexcept { return latencyMeasured_.load(std::memory_order_relaxed); }
    bool measuring() const noexcept { return measuring_.load(std::memory_order_relaxed); } // the loop latency, at start
    float takePeak(int channel) noexcept { return peak_[static_cast<std::size_t>(channel & 1)].exchange(0.0f); }
    float loadAverage() const noexcept { return loadAvg_.load(std::memory_order_relaxed); }
    float takeLoadMax() noexcept { return loadMax_.exchange(0.0f); }
    std::uint64_t songSerial() const noexcept { return serial_.load(std::memory_order_relaxed); } // the song now playing
    std::uint64_t droppedEvents() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    // platform::AudioProcessor
    void prepare(double sampleRate, int maxDeviceFrames) override;
    void process(const platform::AudioIO&) noexcept override;
    // platform::TriggerSink (reader threads)
    bool push(const platform::TriggerNow&) noexcept override;

private:
    struct Event
    {
        enum Kind : std::uint8_t { PadOn, PadOff, Play, Pause, Stop, Jump, LoopLatch, FilterLatch, Note, Strip } kind;
        std::int16_t a = 0, b = 0;
    };
    bool post(const Event&) noexcept;
    void handle(const Event&) noexcept;
    void adopt(Song* song) noexcept;
    void setPosition(int order, int row) noexcept;
    void onRow(int order, int row, std::uint64_t frame) noexcept;
    void applyMasks() noexcept;
    void applySettings() noexcept;
    void padOn(int pad, int velocity) noexcept;
    void padOff(int pad) noexcept;
    void stopLoop(bool returnInTime) noexcept;
    int validOrder(int order) const noexcept;
    long rowsBefore(int order) const noexcept;
    void advance(int& order, int& row, long rows) const noexcept;
    // MIDI
    void midiSend(std::uint64_t frame, const std::uint8_t* bytes, int size) noexcept;
    void midiByte(std::uint64_t frame, std::uint8_t b) noexcept { midiSend(frame, &b, 1); }
    void midiSongPosition(std::uint64_t frame) noexcept;
    void midiRowNotes(int pattern, int row, std::uint64_t frame) noexcept;
    void midiAllNotesOff(std::uint64_t frame) noexcept;
    void clockRow(std::uint64_t frame) noexcept;
    void clockChunk(std::uint64_t from, std::uint64_t to) noexcept;
    // the loop latency measurement
    void measure(const platform::AudioIO&, float* sendL, float* sendR, int offset, int n) noexcept;

    MpscQueue<Event, 256> events_;
    std::atomic<Song*> incoming_ {nullptr}, retired_ {nullptr};
    Song* song_ = nullptr;           // audio thread
    std::shared_ptr<const SongData> current_; // message thread's view of the last loaded song
    const SongData* data_ = nullptr; // audio thread's view
    MidiScheduler* midi_ = nullptr;
    double sampleRate_ = 44100.0;

    // audio thread state
    bool playingNow_ = false, startedOnce_ = false;
    int order_ = 0, row_ = 0, pattern_ = 0;
    std::uint64_t frame_ = 0, periodFrame_ = 0, periodNs_ = 0, rowFrame_ = 0;
    double rowFrames_ = 0;
    int pendingJump_ = -1, pendingQuant_ = 0;
    bool jumped_ = false;
    // stutter loop
    bool loopOn_ = false, loopLatch_ = false;
    int loopOrder_ = 0, loopStart_ = 0, loopLen_ = 0, loopPad_ = -1;
    long loopElapsed_ = 0;
    // masks
    std::uint64_t filterHeld_ = 0, filterLatchBits_ = 0;
    std::uint64_t appliedDry_ = ~0ull, appliedWet_ = ~0ull;
    std::uint64_t seenMute_ = ~0ull, seenSolo_ = ~0ull;
    bool masksDirty_ = true;
    // pads (Play / Kit): the channel each pad's note plays on
    std::array<int, 16> padChannel_ {};
    std::array<int, 128> noteChannel_ {};
    // settings applied to libopenmpt
    float appliedTempo_ = -1, appliedPitch_ = -1;
    int appliedSep_ = -1, appliedInterp_ = -1;
    // filter routing
    int routeNow_ = 0, routePrev_ = 0, fade_ = 0;
    std::vector<float> delayL_, delayR_;
    int delayWrite_ = 0;
    // MIDI clock and notes
    int pulsesLeft_ = 0, pulsesPerRow_ = 6;
    double pulseStep_ = 0, nextPulse_ = 0;
    std::array<int, 16> midiNoteOn_ {};
    // latency measurement
    int measurePhase_ = 0, measureCount_ = 0, measureBest_ = -1;
    float measurePeak_ = 0, measureNoise_ = 0;

    // published
    std::atomic<int> order__ {0}, row__ {0}, pattern__ {0}, speed_ {6}, queuedOrder_ {-1}, loopLength_ {0}, route_ {0};
    std::atomic<int> latency_ {0};
    std::atomic<bool> playing_ {false}, loopActive_ {false}, loopLatched_ {false}, stems_ {false}, latencyMeasured_ {false}, measuring_ {false};
    std::atomic<float> bpm_ {125.0f}, loadAvg_ {0}, loadMax_ {0};
    std::atomic<double> rowSeconds_ {0.12};
    std::atomic<std::uint64_t> rowStartNs_ {0}, filterOn_ {0}, filterLatch_ {0}, padHeld_ {0}, serial_ {0}, dropped_ {0};
    std::array<std::atomic<float>, kMaxChannels> vu_ {};
    std::array<std::atomic<float>, 2> peak_ {};
    std::uint64_t serialCounter_ = 0;
};
} // namespace app
