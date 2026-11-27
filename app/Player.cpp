#include "app/Player.h"

#include "app/Log.h"

#include <libopenmpt/libopenmpt.hpp>
#include <libopenmpt/libopenmpt_ext.hpp>

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>

namespace app
{
namespace
{
double monotonicSeconds() noexcept
{
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + 1e-9 * static_cast<double>(ts.tv_nsec);
}

inline void raise(std::atomic<float>& peak, float value) noexcept
{
    if (value > peak.load(std::memory_order_relaxed))
        peak.store(value, std::memory_order_relaxed);
}

inline float softClip(float x) noexcept // rational tanh approximation, |out| ≤ 1
{
    x = std::clamp(x, -3.0f, 3.0f);
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

inline std::uint64_t bit(int ch) noexcept { return ch >= 0 && ch < 64 ? (1ull << ch) : 0ull; }

// The audio thread on the last core, away from the UI (the boot menu starts apps pinned to core 0; Main widens it).
void pinAudioThread() noexcept
{
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus < 2)
        return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(cpus - 1), &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}
} // namespace

constexpr int Player::kLoopLengths[16];
constexpr int Player::kStripRepeat[8];

Player::Player()
{
    padChannel_.fill(-1);
    for (auto& v : liveVoice_)
        v.fill(-1);
    lastInstrument_.fill(-1);
    midiNoteOn_.fill(-1);
    filterPadChannel_.fill(-1);
    vuStem_.fill(-1);
    appliedOut_.fill(~0ull);
}

Player::~Player()
{
    delete incoming_.exchange(nullptr);
    delete retired_.exchange(nullptr);
    delete song_;
}

// ---- message thread -------------------------------------------------------------------------------------------------

bool Player::load(std::unique_ptr<Song>&& song)
{
    collect();
    if (incoming_.load() != nullptr || retired_.load() != nullptr || ! song)
        return false;
    current_ = song->data;
    incoming_.store(song.release(), std::memory_order_release);
    return true;
}

void Player::collect() { delete retired_.exchange(nullptr, std::memory_order_acq_rel); }

bool Player::post(const Event& e) noexcept
{
    if (events_.push(e))
        return true;
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void Player::play() noexcept { post({Event::Play}); }
void Player::pause() noexcept { post({Event::Pause}); }
void Player::togglePlay() noexcept { post({playing() ? Event::Pause : Event::Play}); }
void Player::stop() noexcept { post({Event::Stop}); }
void Player::jump(int order, Quantize q) noexcept
{
    post({Event::Jump, static_cast<std::int16_t>(order), static_cast<std::int16_t>(q)});
}
void Player::padRelease(int pad0, int hit) noexcept
{
    post({Event::PadOff, static_cast<std::int16_t>(pad0 & 15), 0, static_cast<std::int16_t>(static_cast<std::uint16_t>(hit))});
}
void Player::toggleLoopLatch() noexcept { post({Event::LoopLatch}); }
void Player::setStripMode(int mode) noexcept { post({Event::StripMode, static_cast<std::int16_t>(std::clamp(mode, 0, StripModes - 1))}); }
void Player::setStripHold(bool on) noexcept { post({Event::StripHold, static_cast<std::int16_t>(on ? 1 : 0)}); }
void Player::toggleFilterLatch(int channel) noexcept { post({Event::FilterLatch, static_cast<std::int16_t>(channel)}); }
void Player::midiNote(int channel, int note, int velocity) noexcept
{
    post({Event::Note, static_cast<std::int16_t>(note & 127), static_cast<std::int16_t>(velocity & 127), static_cast<std::int16_t>(channel & 15)});
}
void Player::midiBend(int channel, int value14) noexcept
{
    post({Event::Bend, static_cast<std::int16_t>(channel & 15), static_cast<std::int16_t>(std::clamp(value14, 0, 16383) - 8192)});
}
void Player::midiMod(int channel, int value) noexcept
{
    post({Event::Mod, static_cast<std::int16_t>(channel & 15), static_cast<std::int16_t>(std::clamp(value, 0, 127))});
}
void Player::midiAllNotesOff() noexcept { post({Event::AllOff}); }
void Player::midiRealtime(std::uint8_t status, std::uint64_t ns, int port, int value) noexcept
{
    if (! realtime_.push({ns, status, static_cast<std::uint8_t>(port), static_cast<std::uint16_t>(value)}))
        dropped_.fetch_add(1, std::memory_order_relaxed);
}

void Player::setOuts(const outs::Layout& l) noexcept
{
    for (int c = 0; c < kMaxChannels; ++c)
        outDest_[static_cast<std::size_t>(c)].store(l.dest[static_cast<std::size_t>(c)], std::memory_order_relaxed);
    outAlsoMain_.store(l.alsoMain, std::memory_order_relaxed);
    for (int j = 0; j < outs::kJacks; ++j)
        jackSource_[static_cast<std::size_t>(j)].store(static_cast<std::uint8_t>(l.source[static_cast<std::size_t>(j)]), std::memory_order_relaxed);
    outsSerial_.fetch_add(1, std::memory_order_release);
}

Player::Position Player::position() const noexcept
{
    return {order__.load(std::memory_order_relaxed), row__.load(std::memory_order_relaxed), pattern__.load(std::memory_order_relaxed)};
}

// ---- reader threads -------------------------------------------------------------------------------------------------

bool Player::push(const platform::TriggerNow& t) noexcept
{
    if (t.pad == platform::kStripTriggerPad)
        return post({Event::Strip, t.velocity, t.note});
    if (t.pad == platform::kPressureTriggerPad)
        return true; // pressure: not used
    if (t.pad >= 0 && t.pad < 16 && t.velocity > 0)
    {
        const auto hit = static_cast<std::uint16_t>(padHits_[static_cast<std::size_t>(t.pad)].fetch_add(1, std::memory_order_relaxed) + 1);
        return post({Event::PadOn, t.pad, t.velocity, static_cast<std::int16_t>(hit)});
    }
    return false;
}

// ---- audio thread ---------------------------------------------------------------------------------------------------

void Player::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    lanes_.assign(static_cast<std::size_t>(kLanes) * kMaxDelay, 0.0f);
    measureBuf_.assign(8192, 0.0f);
    delayWrite_ = 0;
}

void Player::adopt(Song* song) noexcept
{
    if (song_ != nullptr)
    {
        if (playingNow_ && midiClock.load(std::memory_order_relaxed))
            midiByte(frame_, 0xFC);
        midiAllNotesOff(frame_);
        retired_.store(song_, std::memory_order_release);
    }
    song_ = song;
    data_ = song->data.get();
    order_ = row_ = 0;
    pattern_ = data_->orderPattern(0);
    rowFrame_ = 0;
    rowFrames_ = 0;
    pendingJump_ = -1;
    naturalValid_ = false;
    nextWhy_ = Next::Natural;
    loopOn_ = loopLatch_ = returnPending_ = false;
    loopPad_ = -1;
    filterHeld_ = filterLatchBits_ = 0;
    filterPadChannel_.fill(-1);
    appliedMuted_ = 0;   // a fresh module has nothing muted, nothing silent on the dry instance …
    appliedDry_ = 0;
    appliedWet_ = ~0ull; // … and everything silent on the other ones (loadSong)
    appliedOut_.fill(~0ull);
    stemDest_.fill(outs::kMain);
    vuStem_.fill(-1);
    mainMask_ = ~0ull;
    clickLeft_ = 0;
    masksDirty_ = true;
    appliedTempo_ = appliedPitch_ = -1;
    appliedSep_ = appliedInterp_ = -1;
    padChannel_.fill(-1);
    for (auto& v : liveVoice_)
        v.fill(-1);
    liveCount_.fill(0);
    lastInstrument_.fill(-1);
    for (auto& r : released_)
        r.voice = -1;
    playingNow_ = false;
    startedOnce_ = false;
    pulsesLeft_ = 0;
    following_ = false; // a new module that plays joins a running clock again (Play)
    syncArm_ = ArmNone;
    queuedOrder_.store(-1);
    loopActive_.store(false);
    loopLatched_.store(false);
    publishPosition();
    serial_.fetch_add(1, std::memory_order_release);
    if (song->playOnAdopt)
        handle({Event::Play});
}

int Player::validOrder(int order) const noexcept
{
    if (data_ == nullptr || data_->orders.empty())
        return 0;
    const int n = static_cast<int>(data_->orders.size());
    for (int tries = 0; tries < 2 * n + 2; ++tries)
    {
        if (order < 0 || order >= n || data_->orders[static_cast<std::size_t>(order)] == -2)
            order = 0;
        if (data_->orders[static_cast<std::size_t>(order)] >= 0)
            return order;
        ++order;
    }
    return 0;
}

long Player::rowsBefore(int order) const noexcept
{
    long rows = 0;
    for (int o = 0; o < order && o < static_cast<int>(data_->orders.size()); ++o)
    {
        const int p = data_->orders[static_cast<std::size_t>(o)];
        if (p == -2)
            break;
        rows += data_->patternRows(p);
    }
    return rows;
}

void Player::advance(int& order, int& row, long rows) const noexcept
{
    for (; rows > 0; --rows)
    {
        ++row;
        if (row >= data_->patternRows(data_->orderPattern(order)))
        {
            order = validOrder(order + 1);
            row = 0;
        }
    }
}

void Player::publishPosition() noexcept
{
    order__.store(order_, std::memory_order_relaxed);
    row__.store(row_, std::memory_order_relaxed);
    pattern__.store(pattern_, std::memory_order_relaxed);
}

// ---- positions ----

void Player::seekNow(int order, int row) noexcept
{
    if (song_ == nullptr)
        return;
    order = validOrder(order);
    row = std::clamp(row, 0, std::max(0, data_->patternRows(data_->orderPattern(order)) - 1));
    song_->each([&](Stem& s) { s.p->jump_to_position(order, row); });
    order_ = songOrder_ = order;
    row_ = songRow_ = row;
    pattern_ = data_->orderPattern(order);
    naturalValid_ = false; // the module starts the row with its next read
    nextWhy_ = Next::Natural;
    rowFrame_ = 0;
    publishPosition();
}

void Player::rewindSong() noexcept
{
    if (song_ == nullptr)
        return;
    song_->each([](Stem& s) { s.p->rewind(); });
    order_ = songOrder_ = 0;
    row_ = songRow_ = 0;
    pattern_ = data_->orderPattern(0);
    naturalValid_ = false;
    nextWhy_ = Next::Natural;
    rowFrame_ = 0;
    masksDirty_ = masksForce_ = true; // the reset may touch the channels' flags: apply the masks again
    publishPosition();
}

void Player::captureNatural() noexcept
{
    song_->dry.p->get_next_position(natOrder_, natRow_);
    naturalValid_ = true;
    nextWhy_ = Next::Natural;
}

void Player::scheduleNext(int order, int row, Next why) noexcept
{
    if (why == Next::Natural)
    {
        if (nextWhy_ != Next::Natural) // give the module its own next row back
        {
            song_->each([&](Stem& s) { s.p->set_next_position(natOrder_, natRow_); });
        }
        nextWhy_ = Next::Natural;
        return;
    }
    if (why == nextWhy_ && order == nextOrder_ && row == nextRow_)
        return;
    song_->each([&](Stem& s) { s.p->set_next_position(order, row); });
    nextWhy_ = why;
    nextOrder_ = order;
    nextRow_ = row;
}

// A row with a pattern delay runs its navigation effects again on each repeat, which overwrites ours: set it again.
void Player::reassertNext() noexcept
{
    if (nextWhy_ == Next::Natural)
        return;
    std::int32_t o = 0, r = 0;
    song_->dry.p->get_next_position(o, r);
    if (o != nextOrder_ || r != nextRow_)
        song_->each([&](Stem& s) { s.p->set_next_position(nextOrder_, nextRow_); });
}

// What the row after this one is: the stutter loop, a due jump, the loop's return, the pattern loop, or the module's.
void Player::planNext() noexcept
{
    if (song_ == nullptr || ! playingNow_ || ! naturalValid_)
        return;
    if (loopOn_)
    {
        int next = row_ + 1;
        if (order_ != loopOrder_ || next >= loopStart_ + loopLen_ || next < loopStart_)
            next = loopStart_;
        scheduleNext(loopOrder_, next, Next::Loop);
        return;
    }
    if (pendingJump_ >= 0)
    {
        const int rpb = std::max(1, rowsPerBeat.load(std::memory_order_relaxed));
        const bool patternEnds = natOrder_ != order_ || natRow_ == 0;
        const bool due = pendingQuant_ == static_cast<int>(Quantize::Now)
                      || (pendingQuant_ == static_cast<int>(Quantize::Pattern) && patternEnds)
                      || (pendingQuant_ == static_cast<int>(Quantize::Beat) && (patternEnds || natRow_ % rpb == 0));
        if (due)
        {
            scheduleNext(pendingJump_, 0, Next::Jump);
            return;
        }
    }
    if (returnPending_)
        scheduleNext(returnOrder_, returnRow_, Next::Return);
    else if (patternLoop.load(std::memory_order_relaxed) && natOrder_ != order_)
        scheduleNext(order_, 0, Next::PatternLoop);
    else
        scheduleNext(0, 0, Next::Natural);
}

void Player::midiSend(std::uint64_t frame, const std::uint8_t* bytes, int size) noexcept
{
    MidiScheduler* midi = midi_.load(std::memory_order_acquire);
    if (midi == nullptr)
        return;
    const double offset = static_cast<double>(frame - periodFrame_) + outputLatency();
    const double ns = offset / sampleRate_ * 1e9 + 1e6 * midiOffsetMs.load(std::memory_order_relaxed);
    midi->schedule(periodNs_ + static_cast<std::uint64_t>(std::max(0.0, ns)), bytes, size);
}

void Player::midiSongPosition(std::uint64_t frame) noexcept
{
    const long rows = rowsBefore(order_) + row_;
    const long sixteenths = rows * pulsesPerRow_ / 6;
    const std::uint8_t spp[3] = {0xF2, static_cast<std::uint8_t>(sixteenths & 0x7F), static_cast<std::uint8_t>((sixteenths >> 7) & 0x7F)};
    midiSend(frame, spp, 3);
}

// The song moved somewhere a follower cannot count to: stop, the new position, continue (the row's first pulse follows).
void Player::midiReposition(std::uint64_t frame) noexcept
{
    if (! playingNow_ || ! midiClock.load(std::memory_order_relaxed) || following_)
        return;
    flushPulses(frame); // the last row's pulses belong before the stop
    midiByte(frame, 0xFC);
    midiSongPosition(frame);
    midiByte(frame, 0xFB);
}

void Player::midiAllNotesOff(std::uint64_t frame) noexcept
{
    for (int c = 0; c < 16; ++c)
        if (midiNoteOn_[static_cast<std::size_t>(c)] >= 0)
        {
            const std::uint8_t off[3] = {static_cast<std::uint8_t>(0x80 | c), static_cast<std::uint8_t>(midiNoteOn_[static_cast<std::size_t>(c)]), 0};
            midiSend(frame, off, 3);
            midiNoteOn_[static_cast<std::size_t>(c)] = -1;
        }
}

void Player::midiRowNotes(int pattern, int row, std::uint64_t frame) noexcept
{
    if (! midiNotes.load(std::memory_order_relaxed) || data_ == nullptr)
        return;
    const std::uint64_t muted = muteMask.load(std::memory_order_relaxed)
                              | (soloMask.load(std::memory_order_relaxed) != 0 ? ~soloMask.load(std::memory_order_relaxed) : 0);
    const bool follow = midiFollowMute.load(std::memory_order_relaxed);
    const int n = std::min(16, data_->channels);
    for (int c = 0; c < n; ++c)
    {
        const Cell* cell = data_->cell(pattern, row, c);
        if (cell == nullptr || cell->noteValue == 0)
            continue;
        auto& on = midiNoteOn_[static_cast<std::size_t>(c)];
        if (on >= 0)
        {
            const std::uint8_t off[3] = {static_cast<std::uint8_t>(0x80 | c), static_cast<std::uint8_t>(on), 0};
            midiSend(frame, off, 3);
            on = -1;
        }
        if (cell->noteValue < 1 || cell->noteValue > 120 || (follow && (muted & bit(c))))
            continue;
        int velocity = 100;
        if (cell->volCommand == 1) // set volume, 0…64
        {
            if (cell->volValue == 0)
                continue;
            velocity = std::clamp(cell->volValue * 2, 1, 127);
        }
        const int note = std::clamp(cell->noteValue - 1, 0, 127); // C-5 (61) → 60
        const std::uint8_t msg[3] = {static_cast<std::uint8_t>(0x90 | c), static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(velocity)};
        midiSend(frame, msg, 3);
        on = note;
    }
}

void Player::flushPulses(std::uint64_t frame) noexcept
{
    for (; pulsesLeft_ > 0; --pulsesLeft_) // the last row ended early (a faster speed, a pattern delay ending): catch up
        midiByte(frame, 0xF8);
}

int Player::pulsesPerRow() const noexcept
{
    return 24 / validRowsPerBeat(rowsPerBeat.load(std::memory_order_relaxed));
}

void Player::clockRow(std::uint64_t frame) noexcept
{
    if (! midiClock.load(std::memory_order_relaxed) || ! playingNow_ || following_)
        return;
    flushPulses(frame);
    pulsesPerRow_ = pulsesPerRow();
    midiByte(frame, 0xF8);
    const double rowLen = rowFrames_ > 0 ? rowFrames_ : sampleRate_ * rowSeconds_.load(std::memory_order_relaxed);
    pulseStep_ = rowLen / pulsesPerRow_;
    nextPulse_ = static_cast<double>(frame) + pulseStep_;
    pulsesLeft_ = pulsesPerRow_ - 1;
}

void Player::clockChunk(std::uint64_t, std::uint64_t to) noexcept
{
    while (pulsesLeft_ > 0 && nextPulse_ < static_cast<double>(to))
    {
        midiByte(static_cast<std::uint64_t>(nextPulse_), 0xF8);
        nextPulse_ += pulseStep_;
        --pulsesLeft_;
    }
}

// A row has started: what brought us here (a scheduled move or the module), the row's MIDI, then the next row's plan.
void Player::onRow(int order, int row, std::uint64_t frame) noexcept
{
    if (rowFrame_ > 0 && frame > rowFrame_)
    {
        const double len = static_cast<double>(frame - rowFrame_);
        if (len < sampleRate_ * 4)
            rowFrames_ = len;
    }
    const Next arrived = nextWhy_ != Next::Natural && order == nextOrder_ && row == nextRow_ ? nextWhy_ : Next::Natural;
    const bool continuous = nextOrder_ == natOrder_ && nextRow_ == natRow_; // where the module went anyway
    const bool reposition = (arrived == Next::Jump || arrived == Next::PatternLoop) && ! continuous;
    if (arrived == Next::Jump)
    {
        pendingJump_ = -1;
        queuedOrder_.store(-1, std::memory_order_relaxed);
        returnPending_ = false;
    }
    if (arrived == Next::Return)
        returnPending_ = false;
    order_ = order;
    row_ = row;
    pattern_ = data_->orderPattern(order);
    // Where the song would be without the loop: a follower counting the clock is there too.
    if (loopOn_ || returnPending_)
        advance(songOrder_, songRow_, 1);
    else
    {
        songOrder_ = order;
        songRow_ = row;
    }
    captureNatural();

    rowFrame_ = frame;
    publishPosition();
    const double ns = (static_cast<double>(frame - periodFrame_) + outputLatency()) / sampleRate_ * 1e9;
    rowStartNs_.store(periodNs_ + static_cast<std::uint64_t>(ns), std::memory_order_release);
    // The instrument each of the first 16 channels plays now: MIDI channel n plays its voice.
    for (int c = 0; c < std::min(16, data_->channels); ++c)
        if (const Cell* cell = data_->cell(pattern_, row_, c); cell != nullptr && cell->instrument > 0)
            lastInstrument_[static_cast<std::size_t>(c)] = static_cast<std::int16_t>(cell->instrument - 1);
    if (following_)
        syncRow(static_cast<double>(periodNs_) + ns, true);
    if (rowFrames_ > 0)
        rowSeconds_.store(rowFrames_ / sampleRate_, std::memory_order_relaxed);
    if (reposition)
        midiReposition(frame);
    if (clickUsed_)
    {
        const int rpb = std::max(1, rowsPerBeat.load(std::memory_order_relaxed));
        if (row % rpb == 0)
        {
            const bool bar = row % (4 * rpb) == 0;
            clickPhase_ = 0;
            clickStep_ = static_cast<float>(2.0 * 3.14159265358979 * (bar ? 1600.0 : 1000.0) / sampleRate_);
            clickAmp_ = bar ? 0.8f : 0.5f;
            clickDecay_ = static_cast<float>(std::exp(-1.0 / (0.008 * sampleRate_))); // 8 ms
            clickLeft_ = static_cast<int>(0.04 * sampleRate_);
        }
    }
    clockRow(frame);
    midiRowNotes(pattern_, row_, frame);
    planNext();
}

// Ends the stutter loop. returnInTime: the song continues with the next row where it would be without the loop.
void Player::stopLoop(bool returnInTime) noexcept
{
    if (! loopOn_)
        return;
    loopOn_ = false;
    loopLatch_ = false;
    loopPad_ = -1;
    if (returnInTime && song_ != nullptr)
    {
        returnOrder_ = songOrder_;
        returnRow_ = songRow_;
        advance(returnOrder_, returnRow_, 1);
        returnPending_ = true;
    }
    else
    {
        songOrder_ = order_;
        songRow_ = row_;
    }
    planNext();
}

// Starts the stutter loop at the current row, or changes the running one's length (it keeps its start).
void Player::startLoop(int rows, int owner) noexcept
{
    if (song_ == nullptr || ! playingNow_ || rows <= 0)
        return;
    if (! loopOn_)
    {
        loopOrder_ = order_;
        loopStart_ = row_;
        loopOn_ = true;
        if (! returnPending_) // a loop right after a loop: the song position keeps counting from the first one
        {
            songOrder_ = order_;
            songRow_ = row_;
        }
        returnPending_ = false;
    }
    const int len = std::max(1, std::min(rows, data_->patternRows(data_->orderPattern(loopOrder_)) - loopStart_));
    if (len == loopLen_ && owner == loopPad_)
        return;
    loopLen_ = len;
    loopPad_ = owner;
    planNext();
}

// The strip acts while touched, and after the finger left while HOLD was on (until HOLD goes off or the mode changes).
// PITCH bends ± stripPitchRange semitones, REPEAT runs a stutter loop whose length the position picks (kStripRepeat),
// USER1 bends the tempo ± 8 %, USER2 sets the volume (top = full, bottom = silent). The middle (64) is neutral.
void Player::stripApply() noexcept
{
    const bool on = stripTouch_ || stripLatch_;
    const float x = (static_cast<float>(stripPos_) - 64.0f) / 64.0f;
    stripPitch_ = on && stripMode_ == StripPitch ? x * stripPitchRange.load(std::memory_order_relaxed) : 0.0f;
    stripTempo_ = on && stripMode_ == StripTempo ? 1.0f + 0.08f * x : 1.0f;
    const float v = static_cast<float>(stripPos_) / 127.0f;
    stripVolume_ = on && stripMode_ == StripVolume ? v * v : 1.0f;
    if (on && stripMode_ == StripRepeat)
        startLoop(kStripRepeat[std::clamp(stripPos_ * 8 / 128, 0, 7)], kStripOwner);
    else if (loopOn_ && loopPad_ == kStripOwner)
        stopLoop(true);
}

void Player::padOn(int pad, int velocity, int hit) noexcept
{
    padHitOn_[static_cast<std::size_t>(pad)] = static_cast<std::uint16_t>(hit);
    padHeld_.fetch_or(1ull << pad, std::memory_order_relaxed);
    if (song_ == nullptr)
        return;
    const int channel = page.load(std::memory_order_relaxed) * 16 + pad;
    switch (static_cast<PadMode>(padMode.load(std::memory_order_relaxed)))
    {
    case PadMode::Filter:
        if (channel < data_->channels && channel < kMaxChannels)
        {
            filterPadChannel_[static_cast<std::size_t>(pad)] = channel;
            filterHeld_ |= bit(channel);
            masksDirty_ = true;
        }
        break;
    case PadMode::Mute: break; // the UI toggles mutes (it knows SHIFT)
    case PadMode::Outs: break; // the UI edits the layout (pad + STEP)
    case PadMode::Loop:
        if (kLoopLengths[pad] > 0)
            startLoop(kLoopLengths[pad], pad);
        break;
    case PadMode::Play:
    case PadMode::Kit:
    {
        const int count = data_->numInstruments > 0 ? data_->numInstruments : data_->numSamples;
        const bool kit = static_cast<PadMode>(padMode.load(std::memory_order_relaxed)) == PadMode::Kit;
        const int inst = kit ? channel : instrument.load(std::memory_order_relaxed);
        const int note = kit ? 60 : playRoot.load(std::memory_order_relaxed) + pad;
        if (inst < 0 || inst >= count || note < 0 || note > 119)
            break;
        try
        {
            if (padChannel_[static_cast<std::size_t>(pad)] >= 0)
                noteOff(padChannel_[static_cast<std::size_t>(pad)]);
            padChannel_[static_cast<std::size_t>(pad)] = playNote(inst, note, std::clamp(velocity / 127.0, 0.05, 1.0));
        }
        catch (...)
        {
            padChannel_[static_cast<std::size_t>(pad)] = -1;
        }
        break;
    }
    }
}

// A live note (pads, MIDI in) plays on every instance, so their voice allocation stays the same; only the instances
// that carry module channel `as` hear it (the dry one for as < 0, or when that channel is heard nowhere).
int Player::playNote(int instrument0, int note, double velocity, int as)
{
    const auto heard = [&](const Stem& s) {
        if (as < 0 || as >= kMaxChannels)
            return &s == &song_->dry;
        const std::uint64_t b = bit(as);
        if (&s == &song_->dry)
            return ! (appliedDry_ & b);
        if (&s == &song_->wet)
            return ! (appliedWet_ & b);
        for (std::size_t k = 0; k < song_->outs.size(); ++k)
            if (&s == &song_->outs[k])
                return ! (appliedOut_[k] & b);
        return false;
    };
    bool any = false;
    song_->each([&](Stem& s) { any = any || heard(s); });
    int channel = -1;
    song_->each([&](Stem& s) {
        const int c = s.i->play_note(instrument0, note, velocity, 0.0);
        if (&s == &song_->dry)
            channel = c;
        s.p->set_channel_silent(c, any ? ! heard(s) : &s != &song_->dry);
    });
    return channel;
}

// ---- MIDI in ----

// Note on / off from MIDI channel `channel`: the kit channel plays instrument (note − 36) at its standard pitch; the
// other channels play module channel n's last instrument (or the selected one) at the note's pitch, where that
// channel plays. The control channel's notes are the UI's (remote pads).
void Player::liveNote(int channel, int note, int velocity) noexcept
{
    if (song_ == nullptr || channel < 0 || channel > 15)
        return;
    const int control = midiControlChannel.load(std::memory_order_relaxed) - 1;
    const int kit = midiKitChannel.load(std::memory_order_relaxed) - 1;
    const int mode = midiInNotes.load(std::memory_order_relaxed);
    if (channel == control)
        return;
    auto& voice = liveVoice_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(note & 127)];
    if (voice >= 0)
    {
        noteOff(voice);
        voice = -1;
        --liveCount_[static_cast<std::size_t>(channel)];
    }
    if (velocity <= 0)
        return;
    const int count = data_->numInstruments > 0 ? data_->numInstruments : data_->numSamples;
    int instrument0 = instrument.load(std::memory_order_relaxed), pitch = note, as = -1;
    if (channel == kit)
    {
        instrument0 = note - 36;
        pitch = 60;
    }
    else if (mode == NotesOff)
        return;
    else if (mode == NotesChannels && channel < std::min(data_->channels, kMaxChannels))
    {
        as = channel;
        if (const int last = lastInstrument_[static_cast<std::size_t>(channel)]; last >= 0)
            instrument0 = last;
    }
    if (instrument0 < 0 || instrument0 >= count || pitch < 0 || pitch > 119)
        return;
    try
    {
        const int v = playNote(instrument0, pitch, velocity / 127.0, as);
        if (v >= 0)
        {
            voice = static_cast<std::int16_t>(v);
            ++liveCount_[static_cast<std::size_t>(channel)];
            applyVoicePitch(v, voicePitch(channel));
        }
    }
    catch (...)
    {
    }
}

float Player::voicePitch(int channel) const noexcept
{
    const auto c = static_cast<std::size_t>(channel);
    const float vibrato = mod_[c] > 0 ? 0.5f * mod_[c] * std::sin(lfoPhase_) : 0.0f; // the mod wheel: up to ± ½ semitone
    return std::exp2((bend_[c] + vibrato) / 12.0f);
}

void Player::applyVoicePitch(int voice, float factor) noexcept
{
    song_->each([&](Stem& s) { s.p->set_voice_pitch(voice, factor); });
}

void Player::updateVoices(int channel) noexcept
{
    const auto c = static_cast<std::size_t>(channel);
    if (song_ == nullptr || liveCount_[c] <= 0)
        return;
    const float f = voicePitch(channel);
    for (const auto v : liveVoice_[c])
        if (v >= 0)
            applyVoicePitch(v, f);
}

// Real-time messages and song position, in arrival order. Start, Continue and Song Position are taken while the
// player is stopped or already following (so its own Start coming back through a MIDI thru is ignored); Stop while
// following. Each needs midiSync.
void Player::realtime(const Realtime& r) noexcept
{
    if (! midiSync.load(std::memory_order_relaxed))
    {
        following_ = false;
        syncArm_ = ArmNone;
        return;
    }
    const bool mayStart = ! playingNow_ || following_;
    switch (r.status)
    {
    case 0xF8: syncPulse(r.ns, r.port); break;
    case 0xFA: // Start: from the top, with the next pulse
    case 0xFB: // Continue: from here (Song Position may have moved it), with the next pulse
        if (! mayStart || song_ == nullptr)
            break;
        if (playingNow_)
            pausePlaying(false);
        if (r.status == 0xFA)
        {
            stopLoop(false);
            returnPending_ = false;
            pendingJump_ = -1;
            queuedOrder_.store(-1);
            rewindSong();
            startedOnce_ = false;
        }
        following_ = true;
        syncPort_ = r.port;
        syncArm_ = r.status == 0xFA ? ArmStart : ArmContinue;
        break;
    case 0xFC: // Stop: pause where it is (a Continue goes on from here)
        if (following_ && (syncPort_ < 0 || r.port == syncPort_))
        {
            if (playingNow_)
                pausePlaying(false);
            following_ = false;
            syncArm_ = ArmNone;
        }
        break;
    case 0xF2: // Song Position, in MIDI beats (sixteenths)
        if (playingNow_ || song_ == nullptr)
            break;
        {
            const long rows = static_cast<long>(r.value) * std::max(1, rowsPerBeat.load(std::memory_order_relaxed)) / 4;
            int order = validOrder(0), row = 0;
            advance(order, row, rows);
            stopLoop(false);
            returnPending_ = false;
            pendingJump_ = -1;
            queuedOrder_.store(-1);
            seekNow(order, row);
            startedOnce_ = true; // a Continue, not a Start, goes on from here
        }
        break;
    default: break;
    }
}

// A clock pulse: the filtered pulse time and period (a second-order loop that smooths the arrival jitter), the row
// grid's origin (a beat; rows every pulsesPerRow() pulses from it), and an armed start that is due.
void Player::syncPulse(std::uint64_t ns, int port) noexcept
{
    const double t = static_cast<double>(ns);
    const bool gap = pulseRawNs_ == 0 || ns <= pulseRawNs_ || ns - pulseRawNs_ > 300000000ull; // under ~8 BPM: stopped
    if (! gap && syncPort_ >= 0 && port != syncPort_ && following_)
        return; // a second clock on another port: the one we follow wins
    if (gap)
    {
        pulseNs_ = t;
        pulsePeriod_ = 0;
        gridKnown_ = false;
    }
    else
    {
        const double interval = t - static_cast<double>(pulseRawNs_);
        if (pulsePeriod_ <= 0)
        {
            pulsePeriod_ = interval;
            pulseNs_ = t;
        }
        else
        {
            const double predicted = pulseNs_ + pulsePeriod_;
            const double err = t - predicted;
            if (std::fabs(err) > 0.5 * pulsePeriod_) // a jump in the master's tempo or a lost pulse: start over from here
            {
                pulsePeriod_ = interval;
                pulseNs_ = t;
            }
            else
            {
                pulseNs_ = predicted + 0.3 * err;
                pulsePeriod_ = std::max(1e5, pulsePeriod_ + 0.05 * err);
            }
        }
    }
    pulseRawNs_ = ns;
    if (syncPort_ < 0 || ! following_)
        syncPort_ = port;
    ++pulseIndex_;
    if (syncArm_ != ArmNone && following_)
    {
        // Join (PLAY while a clock runs): on the next beat of a known grid, else on this pulse.
        const bool due = syncArm_ != ArmJoin || ! gridKnown_ || (pulseIndex_ - gridOrigin_) % 24 == 0;
        if (due)
        {
            if (syncArm_ != ArmJoin || ! gridKnown_)
                gridOrigin_ = pulseIndex_;
            gridKnown_ = true;
            syncArm_ = ArmNone;
            syncIntegral_ = 0;
            startPlaying();
        }
    }
}

// A row started while following, heard at `audibleNs`: the tempo factor that makes this row end on the clock's grid
// (a grid row = the clock's period × pulses per row), corrected by the phase to the nearest grid point (half of it per
// row, plus an integral of it). When the row's first tick already ran at the old factor (`firstTickDone`), the new
// factor is for its remaining ticks, so rows of only a few ticks (speed changes on every row) land on the grid too.
void Player::syncRow(double audibleNs, bool firstTickDone) noexcept
{
    if (! playingNow_ || pulsePeriod_ <= 0 || song_ == nullptr || ! gridKnown_)
        return;
    const int ppr = pulsesPerRow();
    const double gridRow = pulsePeriod_ * ppr;
    const double nominal = song_->dry.p->get_row_seconds() * 1e9;
    if (gridRow <= 0 || nominal <= 0)
        return;
    // The last grid point from the latest pulse and the pulses per row now (rows per beat may change while following;
    // the beats stay where they are, every ratio divides them).
    const double gridNs = pulseNs_ - static_cast<double>(((pulseIndex_ - gridOrigin_) % ppr + ppr) % ppr) * pulsePeriod_;
    double e = std::fmod(audibleNs - 1e6 * midiSyncOffsetMs.load(std::memory_order_relaxed) - gridNs, gridRow);
    if (e > 0.5 * gridRow)
        e -= gridRow;
    else if (e < -0.5 * gridRow)
        e += gridRow;
    // Proportional: half the phase per row; integral: what the module's nominal row length misses (tick rounding,
    // tempo slides, fine delays), so the phase settles at zero instead of at a constant offset.
    syncIntegral_ = std::clamp(syncIntegral_ + 0.1 * e / gridRow, -0.1, 0.1);
    const double correction = std::clamp(0.5 * e / gridRow + syncIntegral_, -0.15, 0.15);
    const double want = gridRow * (1.0 - correction); // this row's length
    const int speed = std::max(1, song_->dry.m->get_current_speed());
    const double tick = nominal / speed; // at factor 1
    double factor = tick * speed / want;
    if (firstTickDone && speed > 1 && appliedTempo_ > 0)
    {
        const double rest = want - tick / appliedTempo_;
        factor = rest > 0.25 * tick * (speed - 1) ? tick * (speed - 1) / rest : 4.0;
    }
    syncFactor_ = static_cast<float>(std::clamp(factor, 0.25, 4.0));
    syncError_.store(static_cast<float>(e / 1e6), std::memory_order_relaxed);
    if (syncFactor_ != appliedTempo_)
    {
        song_->each([&](Stem& s) {
            try
            {
                s.i->set_tempo_factor(syncFactor_);
            }
            catch (...)
            {
            }
        });
        appliedTempo_ = syncFactor_;
    }
}

void Player::startPlaying() noexcept
{
    if (song_ == nullptr || playingNow_)
        return;
    playingNow_ = true;
    if (midiClock.load(std::memory_order_relaxed) && ! following_)
    {
        if (! startedOnce_ && order_ == 0 && row_ == 0)
            midiByte(frame_, 0xFA);
        else
        {
            midiSongPosition(frame_);
            midiByte(frame_, 0xFB);
        }
    }
    startedOnce_ = true;
    rowFrame_ = 0; // no row length from before the pause
    rowStartNs_.store(periodNs_ + static_cast<std::uint64_t>(outputLatency() / sampleRate_ * 1e9), std::memory_order_release);
    if (following_)
        syncRow(static_cast<double>(periodNs_) + outputLatency() / sampleRate_ * 1e9, false);
    clockRow(frame_);
    midiRowNotes(pattern_, row_, frame_);
    planNext(); // with a valid plan from before the pause (else after the first read)
}

void Player::pausePlaying(bool stop) noexcept
{
    if (playingNow_ && midiClock.load(std::memory_order_relaxed) && ! following_)
    {
        flushPulses(frame_);
        midiByte(frame_, 0xFC);
    }
    playingNow_ = false;
    pulsesLeft_ = 0;
    midiAllNotesOff(frame_);
    if (stop && song_ != nullptr)
    {
        if (stripMode_ == StripRepeat)
            stripLatch_ = false; // a held REPEAT does not start again with the next PLAY
        stopLoop(false);
        returnPending_ = false;
        pendingJump_ = -1;
        queuedOrder_.store(-1);
        rewindSong();
        startedOnce_ = false;
    }
}

// Ends a live voice. Sample-based modules (MOD, S3M) have no release: a looping sample would sound on after a key-off,
// so the voice stops (libopenmpt ramps it out). Instruments get a key-off (their release envelope), and a stop
// kReleaseSeconds later in case they sustain without one.
void Player::noteOff(int channel) noexcept
{
    const bool samples = data_ == nullptr || data_->numInstruments == 0;
    song_->each([&](Stem& s) {
        try
        {
            if (s.i2 != nullptr && ! samples)
                s.i2->note_off(channel);
            else
                s.i->stop_note(channel);
        }
        catch (...)
        {
        }
    });
    if (! samples)
    {
        released_[static_cast<std::size_t>(releasedHead_)] = {static_cast<std::int16_t>(channel),
                                                              frame_ + static_cast<std::uint64_t>(kReleaseSeconds * sampleRate_)};
        releasedHead_ = (releasedHead_ + 1) % static_cast<int>(released_.size());
    }
}

// Released instrument voices whose time is up: stopped, unless a live note took the voice since.
void Player::endReleased() noexcept
{
    for (auto& r : released_)
    {
        if (r.voice < 0 || r.dueFrame > frame_)
            continue;
        bool live = false;
        for (std::size_t c = 0; c < liveVoice_.size() && ! live; ++c)
            live = liveCount_[c] > 0 && std::find(liveVoice_[c].begin(), liveVoice_[c].end(), r.voice) != liveVoice_[c].end();
        for (const int p : padChannel_)
            live = live || p == r.voice;
        if (! live && song_ != nullptr)
            song_->each([&](Stem& s) {
                try
                {
                    s.i->stop_note(r.voice);
                }
                catch (...)
                {
                }
            });
        r.voice = -1;
    }
}

void Player::padOff(int pad) noexcept
{
    padHeld_.fetch_and(~(1ull << pad), std::memory_order_relaxed);
    if (song_ == nullptr)
        return;
    if (auto& ch = filterPadChannel_[static_cast<std::size_t>(pad)]; ch >= 0)
    {
        filterHeld_ &= ~bit(ch);
        masksDirty_ = true;
        ch = -1;
    }
    if (loopOn_ && loopPad_ == pad && ! loopLatch_)
        stopLoop(true);
    if (auto& ch = padChannel_[static_cast<std::size_t>(pad)]; ch >= 0)
    {
        noteOff(ch);
        ch = -1;
    }
}

void Player::handle(const Event& e) noexcept
{
    switch (e.kind)
    {
    case Event::PadOn: padOn(e.a & 15, e.b, static_cast<std::uint16_t>(e.c)); break;
    case Event::PadOff:
        // A release that belongs to an earlier hit of this pad (the pad was hit again before the UI passed the
        // release on) must not end the newer one.
        if (static_cast<std::uint16_t>(e.c) == padHitOn_[static_cast<std::size_t>(e.a & 15)])
            padOff(e.a & 15);
        break;
    case Event::Strip:
        stripTouch_ = e.b != 0;
        if (stripTouch_)
        {
            stripPos_ = std::clamp<int>(e.a, 0, 127);
            stripLatch_ = false;
        }
        else
            stripLatch_ = stripHold_; // HOLD keeps the last position acting
        stripApply();
        break;
    case Event::StripMode:
        if (e.a != stripMode_)
        {
            stripLatch_ = false;
            stripMode_ = e.a;
            stripApply();
        }
        break;
    case Event::StripHold:
        stripHold_ = e.a != 0;
        if (! stripHold_ && stripLatch_)
        {
            stripLatch_ = false;
            stripApply();
        }
        break;
    case Event::Play:
        if (song_ == nullptr || playingNow_ || syncArm_ != ArmNone)
            break;
        // PLAY while an external clock runs: follow it, starting on its next beat (or pulse).
        if (midiSync.load(std::memory_order_relaxed) && pulseRawNs_ != 0 && periodNs_ > pulseRawNs_
            && periodNs_ - pulseRawNs_ < 300000000ull)
        {
            following_ = true;
            syncArm_ = ArmJoin;
            break;
        }
        following_ = false;
        startPlaying();
        break;
    case Event::Pause:
    case Event::Stop:
        if (syncArm_ != ArmNone && ! playingNow_) // armed for an external start: cancel it
        {
            syncArm_ = ArmNone;
            following_ = false;
            if (e.kind == Event::Pause)
                break;
        }
        pausePlaying(e.kind == Event::Stop);
        following_ = false;
        break;
    case Event::Jump:
        if (song_ == nullptr)
            break;
        if (! playingNow_)
        {
            stopLoop(false);
            returnPending_ = false;
            pendingJump_ = -1;
            queuedOrder_.store(-1);
            seekNow(e.a, 0);
        }
        else
        {
            stopLoop(true); // the jump takes over when it is due; until then the song continues in time
            pendingJump_ = validOrder(e.a);
            pendingQuant_ = e.b;
            queuedOrder_.store(pendingJump_);
            planNext();
        }
        break;
    case Event::LoopLatch:
        if (loopOn_)
        {
            loopLatch_ = ! loopLatch_;
            if (! loopLatch_ && (loopPad_ < 0 || ! (padHeld_.load() & (1ull << loopPad_))))
                stopLoop(true);
        }
        break;
    case Event::FilterLatch:
        if (song_ != nullptr && e.a >= 0 && e.a < std::min(data_->channels, kMaxChannels))
        {
            filterLatchBits_ ^= bit(e.a);
            masksDirty_ = true;
        }
        break;
    case Event::Note: liveNote(e.c, e.a & 127, e.b); break;
    case Event::Bend:
    {
        const float semitones = static_cast<float>(e.b) / 8192.0f * std::clamp(midiBendRange.load(std::memory_order_relaxed), 0.0f, 24.0f);
        if (e.a == midiControlChannel.load(std::memory_order_relaxed) - 1)
            globalBend_ = semitones;
        else
        {
            bend_[static_cast<std::size_t>(e.a & 15)] = semitones;
            updateVoices(e.a & 15);
        }
        break;
    }
    case Event::Mod:
        if (e.a == midiControlChannel.load(std::memory_order_relaxed) - 1)
            globalMod_ = static_cast<float>(e.b) / 127.0f;
        else
        {
            mod_[static_cast<std::size_t>(e.a & 15)] = static_cast<float>(e.b) / 127.0f;
            updateVoices(e.a & 15);
        }
        break;
    case Event::AllOff:
        if (song_ != nullptr)
            for (std::size_t c = 0; c < liveVoice_.size(); ++c)
            {
                for (auto& v : liveVoice_[c])
                    if (v >= 0)
                    {
                        noteOff(v);
                        v = -1;
                    }
                liveCount_[c] = 0;
            }
        break;
    }
}

// Every instance's mute mask. The single outs first: the destinations that have channels get an instance from the pool
// (one keeps the instance it had; a destination on a jack that carries a utility signal gets none), then
//   out stem k   the channels on its destination
//   dry / wet    the channels on MAIN (not on a single out, or with +MAIN), split by the filter routing
// and every muted channel is muted everywhere.
void Player::applyMasks() noexcept
{
    if (song_ == nullptr)
        return;
    const std::uint64_t solo = soloMask.load(std::memory_order_relaxed);
    const std::uint64_t muted = muteMask.load(std::memory_order_relaxed) | (solo != 0 ? ~solo : 0);
    const int n = std::min(data_->channels, kMaxChannels);
    const int nOut = static_cast<int>(song_->outs.size());
    unsigned channelJacks = 0;
    for (int j = 0; j < outs::kJacks; ++j)
        if (src_[static_cast<std::size_t>(j)] == outs::Source::Channels)
            channelJacks |= 1u << j;
    unsigned used = 0;
    for (int c = 0; c < n; ++c)
        if (const auto d = chDest_[static_cast<std::size_t>(c)]; d != outs::kMain && d < outs::kDests && (outs::jackMask(d) & ~channelJacks) == 0)
            used |= 1u << d;
    unsigned served = 0;
    for (int k = 0; k < nOut; ++k)
    {
        auto& d = stemDest_[static_cast<std::size_t>(k)];
        if (d != outs::kMain && ! ((used >> d) & 1u))
            d = outs::kMain; // free again
        served |= d != outs::kMain ? 1u << d : 0u;
    }
    for (std::uint8_t d = 1; d < outs::kDests; ++d)
        if (((used >> d) & 1u) && ! ((served >> d) & 1u))
            for (int k = 0; k < nOut; ++k)
                if (stemDest_[static_cast<std::size_t>(k)] == outs::kMain)
                {
                    stemDest_[static_cast<std::size_t>(k)] = d;
                    // Its delay lines were not written while it was free: start them silent.
                    std::fill_n(lanes_.begin() + static_cast<std::ptrdiff_t>(kLaneOut0 + 2 * k) * kMaxDelay, 2 * kMaxDelay, 0.0f);
                    served |= 1u << d;
                    break;
                }
    std::array<std::uint64_t, outs::kMaxStems> stemMask {};
    std::uint64_t outMask = 0;
    int stemsUsed = 0;
    for (int k = 0; k < nOut; ++k)
        stemsUsed += stemDest_[static_cast<std::size_t>(k)] != outs::kMain ? 1 : 0;
    for (int c = 0; c < n; ++c)
    {
        vuStem_[static_cast<std::size_t>(c)] = -1;
        const auto d = chDest_[static_cast<std::size_t>(c)];
        if (d == outs::kMain || d >= outs::kDests || ! ((served >> d) & 1u))
            continue;
        for (int k = 0; k < nOut; ++k)
            if (stemDest_[static_cast<std::size_t>(k)] == d)
            {
                stemMask[static_cast<std::size_t>(k)] |= bit(c);
                outMask |= bit(c);
                if (! ((alsoMain_ >> c) & 1u))
                    vuStem_[static_cast<std::size_t>(c)] = static_cast<std::int8_t>(k);
            }
    }
    outsUnserved_.store(static_cast<std::uint16_t>(used & ~served), std::memory_order_relaxed);
    outStemsUsed_.store(stemsUsed, std::memory_order_relaxed);
    outStemsTotal_.store(nOut, std::memory_order_relaxed);

    const std::uint64_t all = data_->channels >= 64 ? ~0ull : (1ull << std::max(0, data_->channels)) - 1;
    const std::uint64_t onMain = all & (~outMask | alsoMain_);
    mainMask_ = onMain;
    const bool split = song_->wet && stems_.load(std::memory_order_relaxed);
    const std::uint64_t picked = filterHeld_ | filterLatchBits_;
    const std::uint64_t routed = ! split ? 0 : filterInverse_ ? (all & ~picked) : picked;
    // Mutes: the same in every instance (a muted channel stands still, so all must agree). Routing: silence per instance.
    const std::uint64_t dry = ~onMain | routed, wet = ~onMain | ~routed;
    for (int c = 0; c < n; ++c)
    {
        const std::uint64_t b = bit(c);
        if (masksForce_ || ((muted ^ appliedMuted_) & b))
            song_->each([&](Stem& s) { s.i->set_channel_mute_status(c, (muted & b) != 0); });
        if (masksForce_ || ((dry ^ appliedDry_) & b))
            song_->dry.p->set_channel_silent(c, (dry & b) != 0);
        if (song_->wet && (masksForce_ || ((wet ^ appliedWet_) & b)))
            song_->wet.p->set_channel_silent(c, (wet & b) != 0);
        for (int k = 0; k < nOut; ++k)
        {
            const std::uint64_t m = ~stemMask[static_cast<std::size_t>(k)];
            if (masksForce_ || ((m ^ appliedOut_[static_cast<std::size_t>(k)]) & b))
                song_->outs[static_cast<std::size_t>(k)].p->set_channel_silent(c, (m & b) != 0);
        }
    }
    appliedMuted_ = muted;
    for (int k = 0; k < nOut; ++k)
        appliedOut_[static_cast<std::size_t>(k)] = ~stemMask[static_cast<std::size_t>(k)];
    masksForce_ = false;
    // MIDI follows the mute: a channel muted now ends its note.
    if (midiFollowMute.load(std::memory_order_relaxed))
        for (int c = 0; c < std::min(16, n); ++c)
            if ((muted & bit(c)) && midiNoteOn_[static_cast<std::size_t>(c)] >= 0)
            {
                const std::uint8_t off[3] = {static_cast<std::uint8_t>(0x80 | c), static_cast<std::uint8_t>(midiNoteOn_[static_cast<std::size_t>(c)]), 0};
                midiSend(frame_, off, 3);
                midiNoteOn_[static_cast<std::size_t>(c)] = -1;
            }
    appliedDry_ = dry;
    appliedWet_ = wet;
    filterOn_.store(filterHeld_ | filterLatchBits_, std::memory_order_relaxed);
    filterLatch_.store(filterLatchBits_, std::memory_order_relaxed);
}

void Player::applySettings() noexcept
{
    if (song_ == nullptr)
        return;
    // Following an external clock, the clock sets the tempo (syncRow); TEMPO and the strip's tempo bend wait.
    const float tempo = following_ && playingNow_ ? syncFactor_
                                                  : std::clamp(tempoFactor.load(std::memory_order_relaxed) * stripTempo_, 0.25f, 4.0f);
    const float vibrato = globalMod_ > 0 ? 0.5f * globalMod_ * std::sin(lfoPhase_) : 0.0f; // the control channel's mod wheel
    const float pitch = std::exp2(std::clamp(pitchSemitones.load(std::memory_order_relaxed) + stripPitch_ + globalBend_ + vibrato, -24.0f, 24.0f) / 12.0f);
    const int sep = std::clamp(stereoSeparation.load(std::memory_order_relaxed), 0, 200);
    int interp = interpolation.load(std::memory_order_relaxed);
    interp = interp >= 8 ? 8 : interp >= 4 ? 4 : interp >= 2 ? 2 : 1;
    auto each = [this](auto&& f) { song_->each([&](Stem& s) { f(*s.m, *s.i); }); };
    if (tempo != appliedTempo_)
    {
        each([&](openmpt::module_ext&, openmpt::ext::interactive& i) { i.set_tempo_factor(tempo); });
        appliedTempo_ = tempo;
    }
    if (pitch != appliedPitch_)
    {
        each([&](openmpt::module_ext&, openmpt::ext::interactive& i) { i.set_pitch_factor(pitch); });
        appliedPitch_ = pitch;
    }
    if (sep != appliedSep_)
    {
        each([&](openmpt::module_ext& m, openmpt::ext::interactive&) { m.set_render_param(openmpt::module::RENDER_STEREOSEPARATION_PERCENT, sep); });
        appliedSep_ = sep;
    }
    if (interp != appliedInterp_)
    {
        each([&](openmpt::module_ext& m, openmpt::ext::interactive&) { m.set_render_param(openmpt::module::RENDER_INTERPOLATIONFILTER_LENGTH, interp); });
        appliedInterp_ = interp;
    }
}

// The loop latency: 4096 frames of silence (the noise floor), one short click into the send, then the return's onset
// within 8192 frames: the first sample above 30 % of the peak (the peak itself moves with the LP cutoff).
void Player::measure(const platform::AudioIO& io, const float* rl, const float* rr, float* sendL, float* sendR, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        if (measurePhase_ >= 2) // finished within this chunk
        {
            sendL[i] = sendR[i] = 0.0f;
            continue;
        }
        const float v = std::max(std::fabs(rl[i]), std::fabs(rr[i]));
        if (measurePhase_ == 0)
        {
            measureNoise_ = std::max(measureNoise_, v);
            sendL[i] = sendR[i] = 0.0f;
            if (++measureCount_ >= 4096)
                measurePhase_ = 1, measureCount_ = 0;
        }
        else
        {
            sendL[i] = sendR[i] = measureCount_ < 8 ? 0.3f : 0.0f;
            measureBuf_[static_cast<std::size_t>(measureCount_)] = v;
            if (++measureCount_ >= static_cast<int>(measureBuf_.size()))
                finishMeasure(io.frames);
        }
    }
}

void Player::finishMeasure(int frames) noexcept
{
    float peak = 0;
    for (float v : measureBuf_)
        peak = std::max(peak, v);
    const float threshold = std::max(4.0f * measureNoise_, 0.3f * peak);
    int onset = -1;
    for (int i = 0; i < static_cast<int>(measureBuf_.size()); ++i)
        if (measureBuf_[static_cast<std::size_t>(i)] >= threshold)
        {
            onset = i;
            break;
        }
    // An onset beyond any plausible loop (the send's buffer plus the return read a period or two behind) is an xrun's
    // artefact, not the loop.
    const bool ok = peak > 4.0f * measureNoise_ && peak > 1e-3f && onset > 0 && onset < 6 * frames + 1024;
    if (ok)
    {
        latency_.store(std::min(onset, kMaxDelay - 1), std::memory_order_relaxed);
        latencyMeasured_.store(true, std::memory_order_relaxed);
    }
    else if (! measureLive_) // at start: a guess; later: keep what the last measurement found
    {
        latency_.store(std::min(3 * frames, kMaxDelay - 1), std::memory_order_relaxed);
        latencyMeasured_.store(false, std::memory_order_relaxed);
    }
    measurePhase_ = 2;
    measureLive_ = false;
    measurements_.fetch_add(1, std::memory_order_release);
}

void Player::process(const platform::AudioIO& io) noexcept
{
    if (frame_ == 0 && periodNs_ == 0)
    {
        nameThisThread("audio");
        pinAudioThread();
    }
    const double t0 = monotonicSeconds();
    periodFrame_ = frame_;
    periodNs_ = io.hostTimeNs;

    // The filter loop: the send port and its return.
    constexpr int kSend = static_cast<int>(platform::AudioPort::FilterSend);
    float* sendL = kSend < io.numOutPorts ? io.out[2 * kSend] : nullptr;
    float* sendR = kSend < io.numOutPorts ? io.out[2 * kSend + 1] : nullptr;
    const bool loop = sendL != nullptr && sendR != nullptr && io.numIn >= 4 && io.in[2] != nullptr && io.in[3] != nullptr;
    if (loop != stems_.load(std::memory_order_relaxed))
    {
        stems_.store(loop, std::memory_order_relaxed);
        masksDirty_ = true;
    }

    // The latency measurement: at start once the lamps show ACTIVE (armMeasure, or after 3 s without), again on
    // request when no route is on. The first one holds the events back, so nothing plays while the delay is unknown.
    if (loop && fixedLatency_.load(std::memory_order_relaxed))
        measurePhase_ = 2;
    else if (loop && measurePhase_ < 0)
    {
        armWait_ += io.frames;
        if (measureArm_.load(std::memory_order_acquire) || armWait_ > 3 * static_cast<int>(sampleRate_))
            measurePhase_ = 0, measureCount_ = 0, measureNoise_ = 0;
    }
    else if (loop && measurePhase_ == 2 && measureRequest_.load(std::memory_order_acquire) && routeSend_ == 0 && routeMain_ == 0
             && routeCount_ == 0)
    {
        measureRequest_.store(false, std::memory_order_relaxed);
        measurePhase_ = 0, measureCount_ = 0, measureNoise_ = 0;
        measureLive_ = true;
    }
    const bool measuringNow = loop && measurePhase_ < 2;
    const bool holdEvents = measuringNow && ! measureLive_;

    if (const auto m = muteMask.load(std::memory_order_relaxed), s = soloMask.load(std::memory_order_relaxed); m != seenMute_ || s != seenSolo_)
    {
        seenMute_ = m;
        seenSolo_ = s;
        masksDirty_ = true;
    }
    if (const auto serial = outsSerial_.load(std::memory_order_acquire); serial != seenOutsSerial_)
    {
        seenOutsSerial_ = serial;
        for (int c = 0; c < kMaxChannels; ++c)
            chDest_[static_cast<std::size_t>(c)] = outDest_[static_cast<std::size_t>(c)].load(std::memory_order_relaxed);
        alsoMain_ = outAlsoMain_.load(std::memory_order_relaxed);
        const bool clickWas = clickUsed_;
        clickUsed_ = false;
        for (int j = 0; j < outs::kJacks; ++j)
        {
            src_[static_cast<std::size_t>(j)] = static_cast<outs::Source>(jackSource_[static_cast<std::size_t>(j)].load(std::memory_order_relaxed));
            clickUsed_ = clickUsed_ || src_[static_cast<std::size_t>(j)] == outs::Source::Click;
        }
        if (clickUsed_ && ! clickWas && ! lanes_.empty()) // its delay line was not written meanwhile
            std::fill_n(lanes_.begin() + static_cast<std::ptrdiff_t>(kLaneClick) * kMaxDelay, kMaxDelay, 0.0f);
        masksDirty_ = true;
    }
    if (! holdEvents)
    {
        if (Song* song = incoming_.exchange(nullptr, std::memory_order_acq_rel))
            adopt(song);
        for (Event e; events_.pop(e);)
            handle(e);
        for (Realtime r; realtime_.pop(r);)
            realtime(r);
    }
    if (song_ != nullptr)
        endReleased();
    // The mod wheels' vibrato (5.5 Hz), on the voices of each MIDI channel whose wheel is up.
    lfoPhase_ = std::fmod(lfoPhase_ + static_cast<float>(2.0 * 3.14159265358979 * 5.5 * io.frames / sampleRate_), 6.2831853f);
    for (int c = 0; c < 16; ++c)
        if (mod_[static_cast<std::size_t>(c)] > 0)
            updateVoices(c);
    // Following: the clock's tempo, the state.
    {
        const bool alive = pulseRawNs_ != 0 && periodNs_ > pulseRawNs_ && periodNs_ - pulseRawNs_ < 300000000ull && pulsePeriod_ > 0;
        clockBpm_.store(alive ? static_cast<float>(60e9 / (24.0 * pulsePeriod_)) : 0.0f, std::memory_order_relaxed);
        const int state = ! following_ ? SyncInternal : syncArm_ != ArmNone ? SyncWaiting : alive ? SyncLocked : SyncLost;
        syncState_.store(state, std::memory_order_relaxed);
    }
    const bool notes = midiNotes.load(std::memory_order_relaxed);
    if (midiNotesWas_ && ! notes) // REC off: end the notes that are on
        midiAllNotesOff(frame_);
    midiNotesWas_ = notes;
    applySettings();
    if (masksDirty_)
    {
        applyMasks();
        masksDirty_ = false;
    }

    // The route: SEND switches now, MAIN when the return carries the new send (delayNow_ frames later).
    const int delayWant = loop ? std::clamp(latency_.load(std::memory_order_relaxed), 0, kMaxDelay - 1) : 0;
    if (delayWant != delayNow_)
    {
        delayPrev_ = delayNow_;
        delayNow_ = delayWant;
        fadeDelay_ = kFade;
    }
    // ACTIVE: the whole mix through the board's main path (Master). With FILTER pads held as well the picked channels
    // stay clean: the select line goes back to the loop and every other channel goes round it (Channels, inverted).
    int route = static_cast<int>(FilterRoute::Off);
    bool inverse = false;
    if (loop && ! measuringNow)
    {
        const bool picked = ((filterHeld_ | filterLatchBits_) & mainMask_) != 0 && song_ != nullptr && song_->wet;
        if (masterFilter.load(std::memory_order_relaxed))
        {
            route = static_cast<int>(picked ? FilterRoute::Channels : FilterRoute::Master);
            inverse = picked;
        }
        else if (picked)
            route = static_cast<int>(FilterRoute::Channels);
    }
    if (inverse != filterInverse_)
    {
        filterInverse_ = inverse;
        masksDirty_ = true;
        applyMasks();
        masksDirty_ = false;
    }
    if (! loop)
    {
        routeSend_ = routeMain_ = route;
        fadeSend_ = fadeMain_ = 0;
        routeCount_ = 0;
    }
    else if (route != routeSend_)
    {
        sendPrev_ = routeSend_;
        routeSend_ = route;
        fadeSend_ = kFade;
        const RouteChange change {frame_ + static_cast<std::uint64_t>(delayNow_), route};
        if (routeCount_ < static_cast<int>(routeQueue_.size()))
            routeQueue_[static_cast<std::size_t>((routeHead_ + routeCount_++) % static_cast<int>(routeQueue_.size()))] = change;
        else // full: the newest change replaces the last one queued
            routeQueue_[static_cast<std::size_t>((routeHead_ + routeCount_ - 1) % static_cast<int>(routeQueue_.size()))] = change;
    }
    route_.store(routeSend_ != 0 ? routeSend_ : routeMain_, std::memory_order_relaxed);
    measuring_.store(measuringNow, std::memory_order_relaxed);

    // DRIVE: a 100-sample linear ramp to a new gain (the stock firmware's).
    if (const float want = driveGain(drive.load(std::memory_order_relaxed)); want != driveTarget_)
    {
        driveTarget_ = want;
        driveStep_ = (want - driveNow_) / 100.0f;
        driveLeft_ = 100;
    }
    const float volumeSmooth = static_cast<float>(1.0 / (0.005 * sampleRate_)); // the strip's volume glides over ~5 ms
    const float ceiling = 1.0f / outputGain_;
    const int overHoldFrames = static_cast<int>(0.5 * sampleRate_);

    const float volume = mainVolume.load(std::memory_order_relaxed);
    const float midiVol = std::clamp(midiVolume.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float gain = std::pow(10.0f, gainDb.load(std::memory_order_relaxed) / 20.0f) * volume * volume * midiVol * midiVol;
    float peakL = 0, peakR = 0;

    // The single outs: OUT 3-4, 5-6, 7-8 (jack j on port 1 + j / 2, left for even j). Gain and the strip's volume act on
    // them, MAIN VOLUME does not (direct outs).
    float* jackOut[outs::kJacks] {};
    bool anyJack = false;
    for (int j = 0; j < outs::kJacks; ++j)
    {
        const int port = 1 + j / 2;
        jackOut[j] = port < io.numOutPorts ? io.out[2 * port + (j & 1)] : nullptr;
        anyJack = anyJack || jackOut[j] != nullptr;
    }
    const float jackGain = std::pow(10.0f, gainDb.load(std::memory_order_relaxed) / 20.0f) * midiVol * midiVol;
    const int nOut = song_ != nullptr ? static_cast<int>(song_->outs.size()) : 0;
    // Work for the jacks only while something is on them (an out stem in use, a utility signal); else silence.
    bool jackWork = false;
    for (int k = 0; k < nOut; ++k)
        jackWork = jackWork || stemDest_[static_cast<std::size_t>(k)] != outs::kMain;
    for (int j = 0; j < outs::kJacks; ++j)
        jackWork = jackWork || src_[static_cast<std::size_t>(j)] != outs::Source::Channels;
    jackWork = jackWork && anyJack;
    if (! jackWork)
        for (int j = 0; j < outs::kJacks; ++j)
            if (jackOut[j] != nullptr)
                std::fill(jackOut[j], jackOut[j] + io.frames, 0.0f);
    std::array<float, outs::kJacks> jackPeak {};
    // The test tone: a request (output index, or kToneStop) starts it, or stops the one playing on that output.
    if (const int want = toneArm_.exchange(kToneNone, std::memory_order_acq_rel); want != kToneNone)
    {
        if (want == kToneStop || want == toneOut_)
            toneOut_ = -1;
        else
        {
            toneOut_ = want;
            toneT_ = 0;
        }
    }

    for (int done = 0; done < io.frames;)
    {
        const int n = std::min(kChunk, io.frames - done);
        float dl[kChunk] = {}, dr[kChunk] = {}, wl[kChunk] = {}, wr[kChunk] = {};
        float ol[outs::kMaxStems][kChunk] = {}, orr[outs::kMaxStems][kChunk] = {};
        if (song_ != nullptr && playingNow_)
        {
            const auto rate = static_cast<std::int32_t>(sampleRate_);
            song_->dry.m->read(rate, static_cast<std::size_t>(n), dl, dr);
            if (song_->wet)
                song_->wet.m->read(rate, static_cast<std::size_t>(n), wl, wr);
            for (int k = 0; k < nOut; ++k) // every instance reads, used or not: they stay in lockstep
                song_->outs[static_cast<std::size_t>(k)].m->read(rate, static_cast<std::size_t>(n), ol[k], orr[k]);
            const int o = song_->dry.m->get_current_order(), r = song_->dry.m->get_current_row();
            if (o != order_ || r != row_)
                onRow(o, r, frame_ + static_cast<std::uint64_t>(n));
            else if (! naturalValid_) // the first read after a seek started the row: now its next row is known
            {
                captureNatural();
                planNext();
            }
            else
                reassertNext();
            clockChunk(frame_, frame_ + static_cast<std::uint64_t>(n));
        }
        // The click (a decaying sine; onRow starts it on each beat, accented on the bar).
        float ck[kChunk] = {};
        if (clickLeft_ > 0)
            for (int i = 0; i < n && clickLeft_ > 0; ++i, --clickLeft_)
            {
                ck[i] = clickAmp_ * std::sin(clickPhase_);
                clickPhase_ += clickStep_;
                clickAmp_ *= clickDecay_;
            }

        float* ml = io.out[0] != nullptr ? io.out[0] + done : nullptr;
        float* mr = io.out[1] != nullptr ? io.out[1] + done : nullptr;
        float* sl = sendL != nullptr ? sendL + done : nullptr;
        float* sr2 = sendR != nullptr ? sendR + done : nullptr;
        // The return without its DC offset (a step in an offset is a click on every route change) and at the send's
        // level again (the hardware sends at filter_send_gain).
        float rl[kChunk] = {}, rr[kChunk] = {};
        if (loop)
            for (int i = 0; i < n; ++i)
            {
                const float xl = io.in[2][done + i], xr = io.in[3][done + i];
                dcOutL_ = xl - dcInL_ + kDcPole * dcOutL_;
                dcOutR_ = xr - dcInR_ + kDcPole * dcOutR_;
                dcInL_ = xl;
                dcInR_ = xr;
                rl[i] = dcOutL_ * returnGain_;
                rr[i] = dcOutR_ * returnGain_;
            }

        const bool measured = loop && measurePhase_ >= 0 && measurePhase_ < 2; // measure() writes the send
        if (measured)
            measure(io, rl, rr, sl, sr2, n);

        for (int i = 0; i < n; ++i)
        {
            // Every stream, delayed by the loop's latency (with the loop; a latency change cross-fades the two taps),
            // so a route change never shifts the song in time and the single outs stay aligned with MAIN.
            const int w = delayWrite_, rd = (w - delayNow_ + kMaxDelay) % kMaxDelay, rp = (w - delayPrev_ + kMaxDelay) % kMaxDelay;
            const float kf = loop && fadeDelay_ > 0 ? static_cast<float>(fadeDelay_) / kFade : 0.0f;
            auto lane = [&](int which, float x) -> float {
                if (! loop)
                    return x;
                float* b = lanes_.data() + static_cast<std::size_t>(which) * kMaxDelay;
                b[w] = x;
                const float y = b[rd];
                return kf > 0 ? y * (1 - kf) + b[rp] * kf : y;
            };
            const float ddl = lane(kLaneDryL, dl[i]), ddr = lane(kLaneDryR, dr[i]);
            const float dwl = lane(kLaneWetL, wl[i]), dwr = lane(kLaneWetR, wr[i]);
            const float click = clickUsed_ ? lane(kLaneClick, ck[i]) : 0.0f;
            float outL = 0, outR = 0;
            float dacPeak = 0; // the driven signal as the DAC that feeds the filter sees it (1 = full scale)
            const float retL = rl[i], retR = rr[i];
            if (loop)
            {
                const std::uint64_t now = frame_ + static_cast<std::uint64_t>(i);
                while (routeCount_ > 0 && routeQueue_[static_cast<std::size_t>(routeHead_)].frame <= now)
                {
                    mainPrev_ = routeMain_;
                    routeMain_ = routeQueue_[static_cast<std::size_t>(routeHead_)].route;
                    fadeMain_ = kFade;
                    routeHead_ = (routeHead_ + 1) % static_cast<int>(routeQueue_.size());
                    --routeCount_;
                }
                if (driveLeft_ > 0)
                    driveNow_ = --driveLeft_ == 0 ? driveTarget_ : driveNow_ + driveStep_;
                // Off: the mix (the wet stem carries nothing). Channels: the dry stem plus the filter's return.
                // Master: the whole mix, driven, on the main DAC, which the filter board then filters in its main path
                // (the select line high: AnalogBoard.h), with no loop and no latency of its own.
                auto mainFor = [&](int r, float& L, float& R) {
                    switch (static_cast<FilterRoute>(r))
                    {
                    case FilterRoute::Off: L = ddl + dwl; R = ddr + dwr; break;
                    case FilterRoute::Channels: L = ddl + retL; R = ddr + retR; break;
                    case FilterRoute::Master: L = (ddl + dwl) * driveNow_; R = (ddr + dwr) * driveNow_; break;
                    }
                };
                auto sendFor = [&](int r, float& L, float& R) { // what goes into the loop, driven
                    switch (static_cast<FilterRoute>(r))
                    {
                    case FilterRoute::Channels: L = wl[i] * driveNow_; R = wr[i] * driveNow_; break;
                    default: L = R = 0; break;
                    }
                };
                mainFor(routeMain_, outL, outR);
                if (fadeMain_ > 0)
                {
                    float L0 = 0, R0 = 0;
                    mainFor(mainPrev_, L0, R0);
                    const float k = static_cast<float>(fadeMain_--) / kFade;
                    outL = outL * (1 - k) + L0 * k;
                    outR = outR * (1 - k) + R0 * k;
                }
                if (! measured)
                {
                    float SL = 0, SR = 0;
                    sendFor(routeSend_, SL, SR);
                    if (fadeSend_ > 0)
                    {
                        float SL0 = 0, SR0 = 0;
                        sendFor(sendPrev_, SL0, SR0);
                        const float k = static_cast<float>(fadeSend_--) / kFade;
                        SL = SL * (1 - k) + SL0 * k;
                        SR = SR * (1 - k) + SR0 * k;
                    }
                    sl[i] = SL;
                    sr2[i] = SR;
                    dacPeak = std::max(std::fabs(SL), std::fabs(SR)) / returnGain_; // the send DAC runs at 1 / returnGain_
                }
                if (routeMain_ == static_cast<int>(FilterRoute::Master) || (fadeMain_ > 0 && mainPrev_ == static_cast<int>(FilterRoute::Master)))
                    dacPeak = std::max(dacPeak, std::max(std::fabs(outL), std::fabs(outR)) * gain * volumeNow_ * outputGain_);
            }
            else
            {
                outL = ddl + dwl;
                outR = ddr + dwr;
            }
            if (loop) // OVER: that peak, held 0.5 s
            {
                if (dacPeak >= overPeak_ || overHold_ <= 0)
                {
                    overPeak_ = dacPeak;
                    overHold_ = overHoldFrames;
                }
                else
                    --overHold_;
            }
            volumeNow_ += (stripVolume_ - volumeNow_) * volumeSmooth;
            // The soft clip at the DAC's full scale (1 / outputGain_ in engine units: 2 = +6 dB of headroom).
            outL = softClip(outL * gain * volumeNow_ * outputGain_) * ceiling;
            outR = softClip(outR * gain * volumeNow_ * outputGain_) * ceiling;
            peakL = std::max(peakL, std::fabs(outL));
            peakR = std::max(peakR, std::fabs(outR));
            if (ml != nullptr)
                ml[i] = outL;
            if (mr != nullptr)
                mr[i] = outR;

            // The single outs: the out stems on their jacks (a mono jack gets ½ (L + R): libopenmpt pans linearly, so
            // that is a channel's level at any pan), then the utility signals.
            float jk[outs::kJacks] = {};
            for (int k = 0; k < nOut && jackWork; ++k)
            {
                if (stemDest_[static_cast<std::size_t>(k)] == outs::kMain)
                    continue; // free: its lanes start silent when it is given out
                const float a = lane(kLaneOut0 + 2 * k, ol[k][i]), b = lane(kLaneOut0 + 2 * k + 1, orr[k][i]);
                const auto d = stemDest_[static_cast<std::size_t>(k)];
                if (outs::isPair(d))
                {
                    jk[2 * outs::pairIndex(d)] += a;
                    jk[2 * outs::pairIndex(d) + 1] += b;
                }
                else if (outs::isMono(d))
                    jk[outs::monoJack(d)] += 0.5f * (a + b);
            }
            if (loop)
                delayWrite_ = (w + 1) % kMaxDelay;
            if (kf > 0)
                --fadeDelay_;
            if (jackWork)
            {
                for (int j = 0; j < outs::kJacks; ++j)
                {
                    const auto src = src_[static_cast<std::size_t>(j)];
                    if (src == outs::Source::Channels)
                        continue;
                    float L = 0, R = 0;
                    switch (src)
                    {
                    case outs::Source::Clean: L = ddl + dwl, R = ddr + dwr; break;
                    case outs::Source::Return: L = retL, R = retR; break;
                    case outs::Source::Click: L = R = click; break;
                    default: break;
                    }
                    jk[j] = src_[static_cast<std::size_t>(j ^ 1)] == src ? ((j & 1) ? R : L) : 0.5f * (L + R);
                }
                for (int j = 0; j < outs::kJacks; ++j)
                {
                    const float v = softClip(jk[j] * jackGain * volumeNow_ * outputGain_) * ceiling;
                    jackPeak[static_cast<std::size_t>(j)] = std::max(jackPeak[static_cast<std::size_t>(j)], std::fabs(v));
                    if (jackOut[j] != nullptr)
                        jackOut[j][done + i] = v;
                }
            }
        }
        // PHONES: the same mix.
        constexpr int kPh = static_cast<int>(platform::AudioPort::Phones);
        if (kPh < io.numOutPorts && io.out[2 * kPh] != nullptr && ml != nullptr)
        {
            std::copy(ml, ml + n, io.out[2 * kPh] + done);
            if (io.out[2 * kPh + 1] != nullptr && mr != nullptr)
                std::copy(mr, mr + n, io.out[2 * kPh + 1] + done);
        }
        frame_ += static_cast<std::uint64_t>(n);
        done += n;
    }
    for (int j = 0; j < outs::kJacks; ++j)
        raise(jackPeak_[static_cast<std::size_t>(j)], jackPeak[static_cast<std::size_t>(j)]);
    // The test tone replaces what its output carries: 440 Hz at −12 dB of the DAC's full scale for kToneSeconds, with
    // 10 ms fades. Output k is io.out[k]: 0/1 MAIN, 2–7 OUT 3–8, 8/9 PHONES (AudioPort × 2 + side).
    if (toneOut_ >= 0)
    {
        const int total = static_cast<int>(kToneSeconds * sampleRate_), ramp = static_cast<int>(0.01 * sampleRate_);
        float* out = toneOut_ < 2 * io.numOutPorts ? io.out[toneOut_] : nullptr;
        for (int i = 0; i < io.frames && toneT_ < total; ++i, ++toneT_)
        {
            const float env = std::min({1.0f, static_cast<float>(toneT_) / ramp, static_cast<float>(total - toneT_) / ramp});
            const float v = 0.25f * ceiling * env * std::sin(static_cast<float>(2.0 * 3.14159265358979 * 440.0 / sampleRate_) * static_cast<float>(toneT_));
            if (out != nullptr)
                out[i] = v;
        }
        if (toneT_ >= total)
            toneOut_ = -1;
    }
    toneOutPub_.store(toneOut_, std::memory_order_relaxed);

    // Publish.
    playing_.store(playingNow_, std::memory_order_relaxed);
    overLevel_.store(loop ? overPeak_ : 0.0f, std::memory_order_relaxed);
    stripModePub_.store(stripMode_, std::memory_order_relaxed);
    stripPosPub_.store(stripPos_, std::memory_order_relaxed);
    stripHoldPub_.store(stripHold_, std::memory_order_relaxed);
    stripTouchPub_.store(stripTouch_, std::memory_order_relaxed);
    stripLatchPub_.store(stripLatch_, std::memory_order_relaxed);
    raise(peak_[0], peakL);
    raise(peak_[1], peakR);
    if (song_ != nullptr)
    {
        speed_.store(song_->dry.m->get_current_speed(), std::memory_order_relaxed);
        // BPM: beats of rows_per_beat rows at the current speed and tempo (what a MIDI clock would have to send)
        if (const double rowSec = song_->dry.p->get_row_seconds(); rowSec > 0)
            bpm_.store(static_cast<float>(60.0 / (rowSec * validRowsPerBeat(rowsPerBeat.load(std::memory_order_relaxed)))),
                       std::memory_order_relaxed);
        const int n = std::min(data_->channels, kMaxChannels);
        for (int c = 0; c < n; ++c)
        {
            float v = 0;
            if (const int k = vuStem_[static_cast<std::size_t>(c)]; k >= 0 && k < nOut)
                v = song_->outs[static_cast<std::size_t>(k)].m->get_current_channel_vu_mono(c);
            else
            {
                v = song_->dry.m->get_current_channel_vu_mono(c);
                if (song_->wet)
                    v = std::max(v, song_->wet.m->get_current_channel_vu_mono(c));
            }
            vu_[static_cast<std::size_t>(c)].store(playingNow_ ? v : 0.0f, std::memory_order_relaxed);
        }
        loopActive_.store(loopOn_, std::memory_order_relaxed);
        loopLatched_.store(loopLatch_, std::memory_order_relaxed);
        loopLength_.store(loopOn_ ? loopLen_ : 0, std::memory_order_relaxed);
    }
    if (io.frames > 0)
    {
        const float load = static_cast<float>((monotonicSeconds() - t0) * sampleRate_ / io.frames);
        loadAvg_.store(loadAvg_.load(std::memory_order_relaxed) * 0.99f + load * 0.01f, std::memory_order_relaxed);
        raise(loadMax_, load);
    }
}
} // namespace app
