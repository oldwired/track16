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

Player::Player()
{
    padChannel_.fill(-1);
    noteChannel_.fill(-1);
    midiNoteOn_.fill(-1);
    filterPadChannel_.fill(-1);
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
void Player::toggleFilterLatch(int channel) noexcept { post({Event::FilterLatch, static_cast<std::int16_t>(channel)}); }
void Player::midiNote(int note, int velocity) noexcept
{
    post({Event::Note, static_cast<std::int16_t>(note & 127), static_cast<std::int16_t>(velocity)});
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
    delayL_.assign(kMaxDelay, 0.0f);
    delayR_.assign(kMaxDelay, 0.0f);
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
    appliedDry_ = 0;    // a fresh module has nothing muted …
    appliedWet_ = ~0ull; // … but the wet one (loadSong mutes all of it)
    masksDirty_ = true;
    appliedTempo_ = appliedPitch_ = -1;
    appliedSep_ = appliedInterp_ = -1;
    padChannel_.fill(-1);
    noteChannel_.fill(-1);
    playingNow_ = false;
    startedOnce_ = false;
    pulsesLeft_ = 0;
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
    song_->dryP->jump_to_position(order, row);
    if (song_->wetP != nullptr)
        song_->wetP->jump_to_position(order, row);
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
    song_->dryP->rewind();
    if (song_->wetP != nullptr)
        song_->wetP->rewind();
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
    song_->dryP->get_next_position(natOrder_, natRow_);
    naturalValid_ = true;
    nextWhy_ = Next::Natural;
}

void Player::scheduleNext(int order, int row, Next why) noexcept
{
    if (why == Next::Natural)
    {
        if (nextWhy_ != Next::Natural) // give the module its own next row back
        {
            song_->dryP->set_next_position(natOrder_, natRow_);
            if (song_->wetP != nullptr)
                song_->wetP->set_next_position(natOrder_, natRow_);
        }
        nextWhy_ = Next::Natural;
        return;
    }
    if (why == nextWhy_ && order == nextOrder_ && row == nextRow_)
        return;
    song_->dryP->set_next_position(order, row);
    if (song_->wetP != nullptr)
        song_->wetP->set_next_position(order, row);
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
    song_->dryP->get_next_position(o, r);
    if (o != nextOrder_ || r != nextRow_)
    {
        song_->dryP->set_next_position(nextOrder_, nextRow_);
        if (song_->wetP != nullptr)
            song_->wetP->set_next_position(nextOrder_, nextRow_);
    }
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
    const double offset = static_cast<double>(frame - periodFrame_) + audioLatencyFrames.load(std::memory_order_relaxed);
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
    if (! playingNow_ || ! midiClock.load(std::memory_order_relaxed))
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

void Player::clockRow(std::uint64_t frame) noexcept
{
    if (! midiClock.load(std::memory_order_relaxed) || ! playingNow_)
        return;
    flushPulses(frame);
    static constexpr int divisors[] = {1, 2, 3, 4, 6, 8, 12, 24};
    int rpb = rowsPerBeat.load(std::memory_order_relaxed);
    if (std::find(std::begin(divisors), std::end(divisors), rpb) == std::end(divisors))
        rpb = 4;
    pulsesPerRow_ = 24 / rpb;
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
    const double ns = (static_cast<double>(frame - periodFrame_) + audioLatencyFrames.load(std::memory_order_relaxed)) / sampleRate_ * 1e9;
    rowStartNs_.store(periodNs_ + static_cast<std::uint64_t>(ns), std::memory_order_release);
    if (rowFrames_ > 0)
        rowSeconds_.store(rowFrames_ / sampleRate_, std::memory_order_relaxed);
    if (reposition)
        midiReposition(frame);
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
    case PadMode::Loop:
    {
        const int len = kLoopLengths[pad];
        if (len <= 0 || ! playingNow_)
            break;
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
        loopLen_ = std::max(1, std::min(len, data_->patternRows(data_->orderPattern(loopOrder_)) - loopStart_));
        loopPad_ = pad;
        planNext();
        break;
    }
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
            if (padChannel_[static_cast<std::size_t>(pad)] >= 0 && song_->dryI2 != nullptr)
                song_->dryI2->note_off(padChannel_[static_cast<std::size_t>(pad)]);
            padChannel_[static_cast<std::size_t>(pad)] = song_->dryI->play_note(inst, note, std::clamp(velocity / 127.0, 0.05, 1.0), 0.0);
        }
        catch (...)
        {
            padChannel_[static_cast<std::size_t>(pad)] = -1;
        }
        break;
    }
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
        try
        {
            if (song_->dryI2 != nullptr)
                song_->dryI2->note_off(ch);
            else
                song_->dryI->stop_note(ch);
        }
        catch (...)
        {
        }
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
        tempoBend.store(e.b != 0 ? 1.0f + 0.08f * (static_cast<float>(e.a) - 64.0f) / 64.0f : 1.0f, std::memory_order_relaxed);
        break;
    case Event::Play:
        if (song_ == nullptr || playingNow_)
            break;
        playingNow_ = true;
        if (midiClock.load(std::memory_order_relaxed))
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
        rowStartNs_.store(periodNs_ + static_cast<std::uint64_t>(audioLatencyFrames.load(std::memory_order_relaxed) / sampleRate_ * 1e9),
                          std::memory_order_release);
        clockRow(frame_);
        midiRowNotes(pattern_, row_, frame_);
        planNext(); // with a valid plan from before the pause (else after the first read)
        break;
    case Event::Pause:
    case Event::Stop:
        if (playingNow_ && midiClock.load(std::memory_order_relaxed))
        {
            flushPulses(frame_);
            midiByte(frame_, 0xFC);
        }
        playingNow_ = false;
        pulsesLeft_ = 0;
        midiAllNotesOff(frame_);
        if (e.kind == Event::Stop && song_ != nullptr)
        {
            stopLoop(false);
            returnPending_ = false;
            pendingJump_ = -1;
            queuedOrder_.store(-1);
            rewindSong();
            startedOnce_ = false;
        }
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
    case Event::Note:
    {
        if (song_ == nullptr)
            break;
        auto& ch = noteChannel_[static_cast<std::size_t>(e.a & 127)];
        try
        {
            if (ch >= 0)
            {
                if (song_->dryI2 != nullptr)
                    song_->dryI2->note_off(ch);
                else
                    song_->dryI->stop_note(ch);
                ch = -1;
            }
            const int count = data_->numInstruments > 0 ? data_->numInstruments : data_->numSamples;
            const int inst = instrument.load(std::memory_order_relaxed);
            if (e.b > 0 && inst >= 0 && inst < count && e.a < 120)
                ch = song_->dryI->play_note(inst, e.a, e.b / 127.0, 0.0);
        }
        catch (...)
        {
            ch = -1;
        }
        break;
    }
    }
}

void Player::applyMasks() noexcept
{
    if (song_ == nullptr)
        return;
    const std::uint64_t solo = soloMask.load(std::memory_order_relaxed);
    const std::uint64_t muted = muteMask.load(std::memory_order_relaxed) | (solo != 0 ? ~solo : 0);
    const bool split = song_->wet != nullptr && stems_.load(std::memory_order_relaxed);
    const std::uint64_t routed = split ? (filterHeld_ | filterLatchBits_) : 0;
    const std::uint64_t dry = muted | routed, wet = muted | ~routed;
    const int n = std::min(data_->channels, kMaxChannels);
    for (int c = 0; c < n; ++c)
    {
        const std::uint64_t b = bit(c);
        if (masksForce_ || ((dry ^ appliedDry_) & b))
            song_->dryI->set_channel_mute_status(c, (dry & b) != 0);
        if (song_->wetI != nullptr && (masksForce_ || ((wet ^ appliedWet_) & b)))
            song_->wetI->set_channel_mute_status(c, (wet & b) != 0);
    }
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
    const float tempo = std::clamp(tempoFactor.load(std::memory_order_relaxed) * tempoBend.load(std::memory_order_relaxed), 0.25f, 4.0f);
    const float pitch = std::exp2(std::clamp(pitchSemitones.load(std::memory_order_relaxed), -24.0f, 24.0f) / 12.0f);
    const int sep = std::clamp(stereoSeparation.load(std::memory_order_relaxed), 0, 200);
    int interp = interpolation.load(std::memory_order_relaxed);
    interp = interp >= 8 ? 8 : interp >= 4 ? 4 : interp >= 2 ? 2 : 1;
    auto each = [this](auto&& f) {
        f(*song_->dry, *song_->dryI);
        if (song_->wet)
            f(*song_->wet, *song_->wetI);
    };
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
void Player::measure(const platform::AudioIO& io, float* sendL, float* sendR, int offset, int n) noexcept
{
    const float* rl = io.in[2] + offset;
    const float* rr = io.in[3] + offset;
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
    const bool ok = peak > 4.0f * measureNoise_ && peak > 1e-3f && onset > 0;
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
    if (! holdEvents)
    {
        if (Song* song = incoming_.exchange(nullptr, std::memory_order_acq_rel))
            adopt(song);
        for (Event e; events_.pop(e);)
            handle(e);
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
    int route = static_cast<int>(FilterRoute::Off);
    if (loop && ! measuringNow)
        route = masterFilter.load(std::memory_order_relaxed) ? static_cast<int>(FilterRoute::Master)
              : (filterHeld_ | filterLatchBits_) != 0 && song_ != nullptr && song_->wet ? static_cast<int>(FilterRoute::Channels)
                                                                                           : static_cast<int>(FilterRoute::Off);
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

    const float volume = mainVolume.load(std::memory_order_relaxed);
    const float gain = std::pow(10.0f, gainDb.load(std::memory_order_relaxed) / 20.0f) * volume * volume;
    float peakL = 0, peakR = 0;

    for (int done = 0; done < io.frames;)
    {
        const int n = std::min(kChunk, io.frames - done);
        float dl[kChunk] = {}, dr[kChunk] = {}, wl[kChunk] = {}, wr[kChunk] = {};
        if (song_ != nullptr && playingNow_)
        {
            song_->dry->read(static_cast<std::int32_t>(sampleRate_), static_cast<std::size_t>(n), dl, dr);
            if (song_->wet)
                song_->wet->read(static_cast<std::int32_t>(sampleRate_), static_cast<std::size_t>(n), wl, wr);
            const int o = song_->dry->get_current_order(), r = song_->dry->get_current_row();
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

        float* ml = io.out[0] != nullptr ? io.out[0] + done : nullptr;
        float* mr = io.out[1] != nullptr ? io.out[1] + done : nullptr;
        float* sl = sendL != nullptr ? sendL + done : nullptr;
        float* sr2 = sendR != nullptr ? sendR + done : nullptr;
        const float* rl = loop ? io.in[2] + done : nullptr;
        const float* rr = loop ? io.in[3] + done : nullptr;

        const bool measured = loop && measurePhase_ >= 0 && measurePhase_ < 2; // measure() writes the send
        if (measured)
            measure(io, sl, sr2, done, n);

        for (int i = 0; i < n; ++i)
        {
            float outL = 0, outR = 0;
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
                // The direct path, delayed by the loop's latency (a latency change cross-fades the two taps).
                const int w = delayWrite_;
                delayL_[static_cast<std::size_t>(w)] = dl[i];
                delayR_[static_cast<std::size_t>(w)] = dr[i];
                const int rd = (w - delayNow_ + kMaxDelay) % kMaxDelay;
                float ddl = delayL_[static_cast<std::size_t>(rd)], ddr = delayR_[static_cast<std::size_t>(rd)];
                if (fadeDelay_ > 0)
                {
                    const int rp = (w - delayPrev_ + kMaxDelay) % kMaxDelay;
                    const float k = static_cast<float>(fadeDelay_--) / kFade;
                    ddl = ddl * (1 - k) + delayL_[static_cast<std::size_t>(rp)] * k;
                    ddr = ddr * (1 - k) + delayR_[static_cast<std::size_t>(rp)] * k;
                }
                delayWrite_ = (w + 1) % kMaxDelay;
                const float retL = rl[i], retR = rr[i];
                // In the Off route the wet stem carries nothing (no channel is routed), so the dry line is the mix.
                auto mainFor = [&](int r, float& L, float& R) {
                    switch (static_cast<FilterRoute>(r))
                    {
                    case FilterRoute::Off: L = ddl; R = ddr; break;
                    case FilterRoute::Channels: L = ddl + retL; R = ddr + retR; break;
                    case FilterRoute::Master: L = retL; R = retR; break;
                    }
                };
                auto sendFor = [&](int r, float& L, float& R) {
                    switch (static_cast<FilterRoute>(r))
                    {
                    case FilterRoute::Off: L = R = 0; break;
                    case FilterRoute::Channels: L = wl[i]; R = wr[i]; break;
                    case FilterRoute::Master: L = dl[i] + wl[i]; R = dr[i] + wr[i]; break;
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
                }
            }
            else
            {
                outL = dl[i] + wl[i];
                outR = dr[i] + wr[i];
            }
            outL = softClip(outL * gain);
            outR = softClip(outR * gain);
            peakL = std::max(peakL, std::fabs(outL));
            peakR = std::max(peakR, std::fabs(outR));
            if (ml != nullptr)
                ml[i] = outL;
            if (mr != nullptr)
                mr[i] = outR;
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
    // Ports nobody writes stay silent (the device zeroes them? not guaranteed): clear the extra jacks.
    for (int p = 1; p < io.numOutPorts; ++p)
        if (p != static_cast<int>(platform::AudioPort::Phones) && p != kSend)
            for (int s = 0; s < 2; ++s)
                if (io.out[2 * p + s] != nullptr)
                    std::fill(io.out[2 * p + s], io.out[2 * p + s] + io.frames, 0.0f);

    // Publish.
    playing_.store(playingNow_, std::memory_order_relaxed);
    raise(peak_[0], peakL);
    raise(peak_[1], peakR);
    if (song_ != nullptr)
    {
        speed_.store(song_->dry->get_current_speed(), std::memory_order_relaxed);
        bpm_.store(static_cast<float>(song_->dry->get_current_estimated_bpm()), std::memory_order_relaxed);
        const int n = std::min(data_->channels, kMaxChannels);
        for (int c = 0; c < n; ++c)
        {
            float v = song_->dry->get_current_channel_vu_mono(c);
            if (song_->wet)
                v = std::max(v, song_->wet->get_current_channel_vu_mono(c));
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
