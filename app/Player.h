// The player: libopenmpt rendering on the audio thread, the live controls, the analog filter routing and the MIDI
// clock/notes. It is the platform's AudioProcessor (process() once per audio period) and its TriggerSink (pad hits and
// releases arrive straight from the panel's reader thread, so a pad acts one panel frame after it is hit).
//
// Threads
//   message thread   load() hands a Song over, collect() deletes the one it replaced; commands (play, jump, …) go
//                    through a lock-free queue; settings are atomics read once per period
//   reader threads   push(): pad hits and pressure, strip moves
//   audio thread     everything that touches libopenmpt; publishes positions, levels and states as atomics
//
// Positions: every move while playing (the stutter loop, the pattern loop, quantized jumps, the loop's return) is
// scheduled as "the row after this one" through libopenmpt's track16_position extension, so it happens exactly at a
// row boundary, costs nothing (no seek simulation) and leaves playing voices alone. Only jumps while paused and STOP
// move at once. Rendering runs in chunks of kChunk frames, so a new row is seen within 0.7 ms of its start.
//
// Analog filter (with the filter loop's FilterSend port and the return on inputs 2/3): channels that go through the
// filter play on the song's "wet" instance, the rest on the "dry" one (complementary mute masks). Routes:
//   off       MAIN = delay(dry)                      SEND = silence
//   channels  MAIN = delay(dry) + return             SEND = wet
//   master    MAIN = return                          SEND = dry + wet
// delay() = the loop's measured latency N, so the direct path stays aligned with the filtered one. A route change
// switches SEND at once and MAIN N frames later (when the return carries the new send), both cross-faded over kFade.
//
// Single outs (OUT 3–8, app/Outs.h): channels sent to a jack or jack pair play on an instance of the song's "outs"
// pool, one per destination in use, given out when a destination gets its first channel and taken back when its last
// one leaves. Those streams are delayed like MAIN, so they stay sample-aligned with it, and take GAIN and the strip's
// volume but not MAIN VOLUME. A jack can carry a utility signal instead: the clean mix (MAIN's channels before the
// filter and DRIVE), the filter's return, or a click on the beat.
#pragma once

#include "app/Outs.h"
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
enum class PadMode : int { Filter, Mute, Loop, Play, Kit, Outs };
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
    // The touch strip's modes, in the order of its MODE lamps (PITCH, REPEAT, USER1, USER2).
    enum StripMode : int { StripPitch, StripRepeat, StripTempo, StripVolume, StripModes };
    static constexpr int kStripRepeat[8] = {16, 12, 8, 6, 4, 3, 2, 1}; // REPEAT: loop rows per strip segment, bottom … top
    // DRIVE (the stock firmware's law): gain 1 + 3·x², 0 … +12 dB, on what goes into the analog filter (the main DAC
    // in the Master route, the send in the Channels route). OVER: that DAC's peak above 0.9 of full scale (the stock
    // firmware's +5.1 dBFS with its −6 dB DAC gain), held 0.5 s.
    static constexpr float kOverThreshold = 0.9f;
    static float driveGain(int value1023) noexcept
    {
        const float x = static_cast<float>(std::clamp(value1023, 0, 1023)) / 1023.0f;
        return 1.0f + 3.0f * x * x;
    }

    Player();
    ~Player() override;

    // ---- before the audio starts ----
    void setMidi(MidiScheduler* m) noexcept { midi_.store(m, std::memory_order_release); }
    void setReturnGain(float g) noexcept { returnGain_ = g; } // 1 / filter_send_gain: the return at the send's level
    // The hardware's fixed gain on the main and PHONES DACs (main_gain, phones: 0.5): the output's full scale is 1 / g
    // in the engine's units, the soft clip's ceiling.
    void setOutputGain(float g) noexcept { outputGain_ = std::clamp(g, 0.1f, 1.0f); }
    void setFixedLatency(int frames) noexcept // the filter loop's latency from the settings (no measurement)
    {
        latency_.store(std::clamp(frames, 1, kMaxDelay - 1));
        latencyMeasured_.store(true);
        fixedLatency_.store(true);
    }

    // ---- message thread ----
    bool load(std::unique_ptr<Song>&& song); // takes the song; false (song left with the caller) while a hand-over is pending
    void collect();                        // deletes songs the audio thread let go of
    std::shared_ptr<const SongData> current() const { return current_; }
    bool stems() const noexcept { return stems_.load(std::memory_order_relaxed); } // the filter loop is there
    int sampleRate() const noexcept { return static_cast<int>(sampleRate_); }      // after the audio started
    void armMeasure() noexcept { measureArm_.store(true, std::memory_order_release); } // the lamps show ACTIVE: measure now
    void remeasure() noexcept { measureRequest_.store(true, std::memory_order_release); } // after loop drops (when no route is on)

    void play() noexcept;
    void pause() noexcept;
    void togglePlay() noexcept;
    void stop() noexcept; // pause and rewind
    void jump(int order, Quantize q) noexcept;
    void padRelease(int pad0, int hit) noexcept; // `hit`: which press of the pad (pushHits() counted), so a late release cannot end a newer hit
    void toggleLoopLatch() noexcept;
    void toggleFilterLatch(int channel) noexcept;
    void midiNote(int note, int velocity) noexcept; // MIDI in: plays the selected instrument
    void setStripMode(int mode) noexcept;            // StripMode; ends what the strip did in the old mode
    void setStripHold(bool on) noexcept;             // HOLD: the strip keeps its last value after the finger leaves
    void setOuts(const outs::Layout&) noexcept;      // the single outs' layout (the audio thread takes it next period)
    // The output test: a kToneSeconds 440 Hz tone on output k alone (0/1 MAIN L/R, 2–7 OUT 3–8, 8/9 PHONES L/R), in
    // place of what it carries; the same k again (or kToneStop) stops it.
    static constexpr int kToneOutputs = 10, kToneStop = -2;
    static constexpr double kToneSeconds = 3.0;
    void testTone(int output) noexcept { toneArm_.store(output, std::memory_order_release); }

    // Settings (message thread writes, audio thread reads once per period).
    std::atomic<int> padMode {static_cast<int>(PadMode::Mute)};
    std::atomic<int> page {0};             // pads 1–16 = channels / instruments page × 16 + 1 …
    std::atomic<int> instrument {0};       // 0-based, for the Play pad mode and MIDI in
    std::atomic<int> playRoot {60};        // pad 1 in the Play mode (60 = C-5)
    std::atomic<int> quantize {static_cast<int>(Quantize::Pattern)};
    std::atomic<bool> patternLoop {false};
    std::atomic<bool> masterFilter {false};
    std::atomic<std::uint64_t> muteMask {0}, soloMask {0};
    std::atomic<float> tempoFactor {1.0f}, pitchSemitones {0.0f};
    std::atomic<float> stripPitchRange {12.0f}; // PITCH strip: ± semitones at its ends
    std::atomic<int> drive {0};            // the DRIVE pot, 0…1023
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
    bool measuring() const noexcept { return measuring_.load(std::memory_order_relaxed); } // the loop latency (or waiting to)
    int measurements() const noexcept { return measurements_.load(std::memory_order_acquire); } // finished ones
    float takePeak(int channel) noexcept { return peak_[static_cast<std::size_t>(channel & 1)].exchange(0.0f); }
    float loadAverage() const noexcept { return loadAvg_.load(std::memory_order_relaxed); }
    float takeLoadMax() noexcept { return loadMax_.exchange(0.0f); }
    std::uint64_t songSerial() const noexcept { return serial_.load(std::memory_order_relaxed); } // the song now playing
    std::uint64_t droppedEvents() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    // the touch strip: its mode, HOLD, the position it acts with and whether it acts (touched, or held by HOLD)
    int stripMode() const noexcept { return stripModePub_.load(std::memory_order_relaxed); }
    bool stripHold() const noexcept { return stripHoldPub_.load(std::memory_order_relaxed); }
    int stripPosition() const noexcept { return stripPosPub_.load(std::memory_order_relaxed); }
    bool stripTouched() const noexcept { return stripTouchPub_.load(std::memory_order_relaxed); }
    bool stripLatched() const noexcept { return stripLatchPub_.load(std::memory_order_relaxed); }
    float overLevel() const noexcept { return overLevel_.load(std::memory_order_relaxed); } // held DAC peak of the driven signal
    bool over() const noexcept { return overLevel() >= kOverThreshold; }                      // the OVER lamp
    // the single outs: instances in use / in the pool, destinations (bits 1–9) with channels but no free instance,
    // each jack's peak since the last call, and the output the test tone plays on (−1 = none)
    int outStemsUsed() const noexcept { return outStemsUsed_.load(std::memory_order_relaxed); }
    int outStemsTotal() const noexcept { return outStemsTotal_.load(std::memory_order_relaxed); }
    std::uint16_t outsUnserved() const noexcept { return outsUnserved_.load(std::memory_order_relaxed); }
    float takeJackPeak(int jack0) noexcept { return jackPeak_[static_cast<std::size_t>(jack0 % outs::kJacks)].exchange(0.0f); }
    int toneOutput() const noexcept { return toneOutPub_.load(std::memory_order_relaxed); }

    // platform::AudioProcessor
    void prepare(double sampleRate, int maxDeviceFrames) override;
    void process(const platform::AudioIO&) noexcept override;
    // platform::TriggerSink (reader threads; a pad's hits are counted, see padRelease)
    bool push(const platform::TriggerNow&) noexcept override;

private:
    struct Event
    {
        enum Kind : std::uint8_t { PadOn, PadOff, Play, Pause, Stop, Jump, LoopLatch, FilterLatch, Note, Strip, StripMode, StripHold } kind;
        std::int16_t a = 0, b = 0, c = 0;
    };
    // Why the next row is not the module's own next row.
    enum class Next : std::uint8_t { Natural, Loop, Return, Jump, PatternLoop };
    bool post(const Event&) noexcept;
    void handle(const Event&) noexcept;
    void adopt(Song* song) noexcept;
    void seekNow(int order, int row) noexcept; // paused: at once
    void rewindSong() noexcept;
    void publishPosition() noexcept;
    void captureNatural() noexcept;
    void planNext() noexcept;
    void scheduleNext(int order, int row, Next why) noexcept;
    void reassertNext() noexcept;
    void onRow(int order, int row, std::uint64_t frame) noexcept;
    void applyMasks() noexcept;
    void applySettings() noexcept;
    void padOn(int pad, int velocity, int hit) noexcept;
    void padOff(int pad) noexcept;
    void stopLoop(bool returnInTime) noexcept;
    void startLoop(int rows, int owner) noexcept; // owner: the LOOP pad, or kStripOwner
    void stripApply() noexcept;                   // the strip's effect from its mode, position, touch and HOLD
    int playNote(int instrument0, int note, double velocity); // may throw (libopenmpt); returns the dry voice
    void noteOff(int channel) noexcept;
    // Frames from the engine to the jacks: the output buffer plus the loop-latency delay of the main path.
    double outputLatency() const noexcept { return audioLatencyFrames.load(std::memory_order_relaxed) + delayNow_; }
    int validOrder(int order) const noexcept;
    long rowsBefore(int order) const noexcept;
    void advance(int& order, int& row, long rows) const noexcept;
    // MIDI
    void midiSend(std::uint64_t frame, const std::uint8_t* bytes, int size) noexcept;
    void midiByte(std::uint64_t frame, std::uint8_t b) noexcept { midiSend(frame, &b, 1); }
    void midiSongPosition(std::uint64_t frame) noexcept;
    void midiReposition(std::uint64_t frame) noexcept;
    void midiRowNotes(int pattern, int row, std::uint64_t frame) noexcept;
    void midiAllNotesOff(std::uint64_t frame) noexcept;
    void clockRow(std::uint64_t frame) noexcept;
    void flushPulses(std::uint64_t frame) noexcept;
    void clockChunk(std::uint64_t from, std::uint64_t to) noexcept;
    // the loop latency measurement
    void measure(const platform::AudioIO&, const float* rl, const float* rr, float* sendL, float* sendR, int n) noexcept;
    void finishMeasure(int frames) noexcept;

    MpscQueue<Event, 256> events_;
    std::atomic<Song*> incoming_ {nullptr}, retired_ {nullptr};
    Song* song_ = nullptr;           // audio thread
    std::shared_ptr<const SongData> current_; // message thread's view of the last loaded song
    const SongData* data_ = nullptr; // audio thread's view
    std::atomic<MidiScheduler*> midi_ {nullptr};
    double sampleRate_ = 44100.0;
    std::array<std::atomic<std::uint16_t>, 16> padHits_ {}; // presses per pad, counted by push()

    // audio thread state
    bool playingNow_ = false, startedOnce_ = false;
    int order_ = 0, row_ = 0, pattern_ = 0;
    std::uint64_t frame_ = 0, periodFrame_ = 0, periodNs_ = 0, rowFrame_ = 0;
    double rowFrames_ = 0;
    int pendingJump_ = -1, pendingQuant_ = 0;
    // the next row: the module's own (captured at each row start), and ours when we override it
    bool naturalValid_ = false;
    int natOrder_ = 0, natRow_ = 0;
    Next nextWhy_ = Next::Natural;
    int nextOrder_ = 0, nextRow_ = 0;
    // stutter loop, and the return to where the song would be
    bool loopOn_ = false, loopLatch_ = false, returnPending_ = false;
    int loopOrder_ = 0, loopStart_ = 0, loopLen_ = 0, loopPad_ = -1;
    int songOrder_ = 0, songRow_ = 0;     // where the song would be without the loop
    static constexpr int kStripOwner = -2; // loopPad_ of a loop the REPEAT strip runs
    // the touch strip
    int stripMode_ = StripPitch, stripPos_ = 64;
    bool stripTouch_ = false, stripHold_ = false, stripLatch_ = false; // latch: released while HOLD was on
    float stripPitch_ = 0, stripTempo_ = 1, stripVolume_ = 1, volumeNow_ = 1;
    // drive and OVER
    float driveNow_ = 1, driveStep_ = 0, driveTarget_ = 1, overPeak_ = 0;
    int driveLeft_ = 0, overHold_ = 0;
    int returnOrder_ = 0, returnRow_ = 0;
    // masks
    std::uint64_t filterHeld_ = 0, filterLatchBits_ = 0;
    std::array<int, 16> filterPadChannel_ {};  // the channel a held FILTER pad routes (the page may change while held)
    std::array<std::uint16_t, 16> padHitOn_ {}; // the hit each pad's last PadOn was
    std::uint64_t appliedMuted_ = 0, appliedDry_ = ~0ull, appliedWet_ = ~0ull; // mutes (all instances), silence per instance
    std::uint64_t seenMute_ = ~0ull, seenSolo_ = ~0ull;
    bool masksDirty_ = true, masksForce_ = true;
    // pads (Play / Kit): the channel each pad's note plays on
    std::array<int, 16> padChannel_ {};
    std::array<int, 128> noteChannel_ {};
    // settings applied to libopenmpt
    float appliedTempo_ = -1, appliedPitch_ = -1;
    int appliedSep_ = -1, appliedInterp_ = -1;
    // filter routing: SEND follows the route at once, MAIN after the loop's latency (queued changes)
    int routeSend_ = 0, sendPrev_ = 0, fadeSend_ = 0;
    int routeMain_ = 0, mainPrev_ = 0, fadeMain_ = 0;
    struct RouteChange
    {
        std::uint64_t frame;
        int route;
    };
    std::array<RouteChange, 8> routeQueue_ {};
    int routeHead_ = 0, routeCount_ = 0;
    // The delay lines (kMaxDelay frames each): MAIN's two stems, the click, then each out stem's left and right.
    static constexpr int kLaneDryL = 0, kLaneDryR = 1, kLaneWetL = 2, kLaneWetR = 3, kLaneClick = 4, kLaneOut0 = 5;
    static constexpr int kLanes = kLaneOut0 + 2 * outs::kMaxStems;
    std::vector<float> lanes_;
    static constexpr float kDcPole = 0.999f;          // the return's DC blocker (about 7 Hz)
    float dcInL_ = 0, dcInR_ = 0, dcOutL_ = 0, dcOutR_ = 0, returnGain_ = 2.0f, outputGain_ = 1.0f;
    bool filterInverse_ = false; // ACTIVE with FILTER pads held: the held channels bypass the filter, the rest go through it
    int delayWrite_ = 0, delayNow_ = 0, delayPrev_ = 0, fadeDelay_ = 0;
    // MIDI clock and notes
    int pulsesLeft_ = 0, pulsesPerRow_ = 6;
    double pulseStep_ = 0, nextPulse_ = 0;
    std::array<int, 16> midiNoteOn_ {};
    bool midiNotesWas_ = false;
    // latency measurement: −1 waiting (for armMeasure, or a quiet route), 0 the noise floor, 1 the click, 2 done
    int measurePhase_ = -1, measureCount_ = 0, armWait_ = 0;
    bool measureLive_ = false; // a re-measurement while a song may play (keep the old latency if it fails)
    float measureNoise_ = 0;
    std::vector<float> measureBuf_;
    // the single outs: the layout as the audio thread applies it, which destination each out stem serves (0 = free),
    // the instance each channel's VU comes from (−1 = dry/wet), the channels MAIN carries
    std::array<std::uint8_t, kMaxChannels> chDest_ {};
    std::uint64_t alsoMain_ = 0, mainMask_ = ~0ull;
    std::array<outs::Source, outs::kJacks> src_ {};
    std::array<std::uint8_t, outs::kMaxStems> stemDest_ {};
    std::array<std::uint64_t, outs::kMaxStems> appliedOut_ {};
    std::array<std::int8_t, kMaxChannels> vuStem_ {};
    std::uint32_t seenOutsSerial_ = 0;
    bool clickUsed_ = false;
    int clickLeft_ = 0, toneOut_ = -1, toneT_ = 0;
    float clickPhase_ = 0, clickStep_ = 0, clickAmp_ = 0, clickDecay_ = 0;
    // written by setOuts()
    std::array<std::atomic<std::uint8_t>, kMaxChannels> outDest_ {};
    std::array<std::atomic<std::uint8_t>, outs::kJacks> jackSource_ {};
    std::atomic<std::uint64_t> outAlsoMain_ {0};
    std::atomic<std::uint32_t> outsSerial_ {0};
    static constexpr int kToneNone = -1;
    std::atomic<int> toneArm_ {kToneNone};

    // published
    std::atomic<int> order__ {0}, row__ {0}, pattern__ {0}, speed_ {6}, queuedOrder_ {-1}, loopLength_ {0}, route_ {0};
    std::atomic<int> latency_ {0}, measurements_ {0};
    std::atomic<bool> playing_ {false}, loopActive_ {false}, loopLatched_ {false}, stems_ {false}, latencyMeasured_ {false}, measuring_ {false};
    std::atomic<bool> fixedLatency_ {false}, measureArm_ {false}, measureRequest_ {false};
    std::atomic<float> bpm_ {125.0f}, loadAvg_ {0}, loadMax_ {0}, overLevel_ {0};
    std::atomic<int> stripModePub_ {StripPitch}, stripPosPub_ {64};
    std::atomic<bool> stripHoldPub_ {false}, stripTouchPub_ {false}, stripLatchPub_ {false};
    std::atomic<double> rowSeconds_ {0.12};
    std::atomic<std::uint64_t> rowStartNs_ {0}, filterOn_ {0}, filterLatch_ {0}, padHeld_ {0}, serial_ {0}, dropped_ {0};
    std::array<std::atomic<float>, kMaxChannels> vu_ {};
    std::array<std::atomic<float>, 2> peak_ {};
    std::array<std::atomic<float>, outs::kJacks> jackPeak_ {};
    std::atomic<int> outStemsUsed_ {0}, outStemsTotal_ {0}, toneOutPub_ {-1};
    std::atomic<std::uint16_t> outsUnserved_ {0};
};
} // namespace app
