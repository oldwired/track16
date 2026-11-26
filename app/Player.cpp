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
void Player::padRelease(int pad0) noexcept { post({Event::PadOff, static_cast<std::int16_t>(pad0 & 15)}); }
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
        return post({Event::PadOn, t.pad, t.velocity});
    return false;
}

// ---- audio thread ---------------------------------------------------------------------------------------------------

void Player::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    delayL_.assign(kMaxDelay, 0.0f);
    delayR_.assign(kMaxDelay, 0.0f);
    delayWrite_ = 0;
}

void Player::adopt(Song* song) noexcept
{
    if (song_ != nullptr)
    {
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
    loopOn_ = loopLatch_ = false;
    filterHeld_ = filterLatchBits_ = 0;
    appliedDry_ = appliedWet_ = 0; // a fresh module has nothing muted
    masksDirty_ = true;
    appliedTempo_ = appliedPitch_ = -1;
    appliedSep_ = appliedInterp_ = -1;
    padChannel_.fill(-1);
    noteChannel_.fill(-1);
    playingNow_ = false;
    startedOnce_ = false;
    pulsesLeft_ = 0;
    order__.store(0);
    row__.store(0);
    pattern__.store(pattern_);
    queuedOrder_.store(-1);
    loopActive_.store(false);
    loopLatched_.store(false);
    serial_.fetch_add(1, std::memory_order_release);
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

void Player::setPosition(int order, int row) noexcept
{
    if (song_ == nullptr)
        return;
    order = validOrder(order);
    row = std::clamp(row, 0, std::max(0, data_->patternRows(data_->orderPattern(order)) - 1));
    song_->dry->set_position_order_row(order, row);
    if (song_->wet)
        song_->wet->set_position_order_row(order, row);
    order_ = order;
    row_ = row;
    pattern_ = data_->orderPattern(order);
    jumped_ = true;
    if (playingNow_ && midiClock.load(std::memory_order_relaxed))
    {
        midiByte(frame_, 0xFC); // stop, move, continue: followers jump with us
        midiSongPosition(frame_);
        midiByte(frame_, 0xFB);
    }
}

void Player::midiSend(std::uint64_t frame, const std::uint8_t* bytes, int size) noexcept
{
    if (midi_ == nullptr)
        return;
    const double offset = static_cast<double>(frame - periodFrame_) + audioLatencyFrames.load(std::memory_order_relaxed);
    const double ns = offset / sampleRate_ * 1e9 + 1e6 * midiOffsetMs.load(std::memory_order_relaxed);
    midi_->schedule(periodNs_ + static_cast<std::uint64_t>(std::max(0.0, ns)), bytes, size);
}

void Player::midiSongPosition(std::uint64_t frame) noexcept
{
    const long rows = rowsBefore(order_) + row_;
    const long sixteenths = rows * pulsesPerRow_ / 6;
    const std::uint8_t spp[3] = {0xF2, static_cast<std::uint8_t>(sixteenths & 0x7F), static_cast<std::uint8_t>((sixteenths >> 7) & 0x7F)};
    midiSend(frame, spp, 3);
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

void Player::clockRow(std::uint64_t frame) noexcept
{
    if (! midiClock.load(std::memory_order_relaxed) || ! playingNow_)
        return;
    for (; pulsesLeft_ > 0; --pulsesLeft_) // the last row ended early (a jump, a faster speed): catch up
        midiByte(frame, 0xF8);
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

// A row has started (naturally or after a jump): quantized actions, then the row's MIDI.
void Player::onRow(int order, int row, std::uint64_t frame) noexcept
{
    const bool natural = ! jumped_;
    const bool patternChanged = order != order_;
    const int previousOrder = order_;
    jumped_ = false;
    if (natural && rowFrame_ > 0 && frame > rowFrame_)
    {
        const double len = static_cast<double>(frame - rowFrame_);
        if (len < sampleRate_ * 4)
            rowFrames_ = len;
    }
    order_ = order;
    row_ = row;
    pattern_ = data_->orderPattern(order);

    if (loopOn_)
    {
        ++loopElapsed_;
        if (order != loopOrder_ || row >= loopStart_ + loopLen_ || row < loopStart_)
            setPosition(loopOrder_, loopStart_);
    }
    else if (pendingJump_ >= 0)
    {
        const int rpb = std::max(1, rowsPerBeat.load(std::memory_order_relaxed));
        const bool due = (pendingQuant_ == static_cast<int>(Quantize::Pattern) && patternChanged && natural)
                      || (pendingQuant_ == static_cast<int>(Quantize::Beat) && row % rpb == 0);
        if (due)
        {
            const int target = pendingJump_;
            pendingJump_ = -1;
            queuedOrder_.store(-1, std::memory_order_relaxed);
            setPosition(target, 0);
        }
    }
    else if (patternLoop.load(std::memory_order_relaxed) && patternChanged && natural)
        setPosition(previousOrder, 0);

    rowFrame_ = frame;
    order__.store(order_, std::memory_order_relaxed);
    row__.store(row_, std::memory_order_relaxed);
    pattern__.store(pattern_, std::memory_order_relaxed);
    const double ns = (static_cast<double>(frame - periodFrame_) + audioLatencyFrames.load(std::memory_order_relaxed)) / sampleRate_ * 1e9;
    rowStartNs_.store(periodNs_ + static_cast<std::uint64_t>(ns), std::memory_order_release);
    if (rowFrames_ > 0)
        rowSeconds_.store(rowFrames_ / sampleRate_, std::memory_order_relaxed);
    clockRow(frame);
    midiRowNotes(pattern_, row_, frame);
}

void Player::stopLoop(bool returnInTime) noexcept
{
    if (! loopOn_)
        return;
    loopOn_ = false;
    loopLatch_ = false;
    loopPad_ = -1;
    if (returnInTime && song_ != nullptr)
    {
        int o = loopOrder_, r = loopStart_;
        advance(o, r, loopElapsed_);
        setPosition(o, r);
        onRow(order_, row_, frame_);
    }
}

void Player::padOn(int pad, int velocity) noexcept
{
    padHeld_.fetch_or(1ull << pad, std::memory_order_relaxed);
    if (song_ == nullptr)
        return;
    const int channel = page.load(std::memory_order_relaxed) * 16 + pad;
    switch (static_cast<PadMode>(padMode.load(std::memory_order_relaxed)))
    {
    case PadMode::Filter:
        if (channel < data_->channels && channel < kMaxChannels)
        {
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
            loopElapsed_ = 0;
            loopOn_ = true;
        }
        loopLen_ = std::max(1, std::min(len, data_->patternRows(data_->orderPattern(loopOrder_)) - loopStart_));
        loopPad_ = pad;
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
    const int channel = page.load(std::memory_order_relaxed) * 16 + pad;
    if (filterHeld_ & bit(channel))
    {
        filterHeld_ &= ~bit(channel);
        masksDirty_ = true;
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
    case Event::PadOn: padOn(e.a & 15, e.b); break;
    case Event::PadOff: padOff(e.a & 15); break;
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
        onRow(order_, row_, frame_);
        break;
    case Event::Pause:
    case Event::Stop:
        if (playingNow_ && midiClock.load(std::memory_order_relaxed))
            midiByte(frame_, 0xFC);
        playingNow_ = false;
        pulsesLeft_ = 0;
        midiAllNotesOff(frame_);
        if (e.kind == Event::Stop && song_ != nullptr)
        {
            stopLoop(false);
            pendingJump_ = -1;
            queuedOrder_.store(-1);
            setPosition(0, 0);
            jumped_ = false;
            startedOnce_ = false;
            order__.store(0);
            row__.store(0);
            pattern__.store(pattern_);
        }
        break;
    case Event::Jump:
        if (song_ == nullptr)
            break;
        if (e.b == static_cast<int>(Quantize::Now) || ! playingNow_)
        {
            pendingJump_ = -1;
            queuedOrder_.store(-1);
            stopLoop(false);
            setPosition(e.a, 0);
            if (playingNow_)
                onRow(order_, row_, frame_);
            else
            {
                jumped_ = false;
                order__.store(order_);
                row__.store(row_);
                pattern__.store(pattern_);
            }
        }
        else
        {
            pendingJump_ = validOrder(e.a);
            pendingQuant_ = e.b;
            queuedOrder_.store(pendingJump_);
        }
        break;
    case Event::LoopLatch:
        if (loopOn_)
        {
            loopLatch_ = ! loopLatch_;
            if (! loopLatch_ && ! (padHeld_.load() & (1ull << std::max(0, loopPad_))))
                stopLoop(true);
        }
        break;
    case Event::FilterLatch:
        if (e.a >= 0 && e.a < kMaxChannels)
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
        if ((dry ^ appliedDry_) & b)
            song_->dryI->set_channel_mute_status(c, (dry & b) != 0);
        if (song_->wetI != nullptr && ((wet ^ appliedWet_) & b))
            song_->wetI->set_channel_mute_status(c, (wet & b) != 0);
    }
    if (song_->wetI == nullptr)
        appliedWet_ = wet;
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

// The loop latency: 4096 frames of silence (the noise floor), one short click into the send, then the return's
// strongest sample within 8192 frames.
void Player::measure(const platform::AudioIO& io, float* sendL, float* sendR, int offset, int n) noexcept
{
    const float* rl = io.in[2] + offset;
    const float* rr = io.in[3] + offset;
    for (int i = 0; i < n; ++i)
    {
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
            if (v > measurePeak_)
                measurePeak_ = v, measureBest_ = measureCount_;
            if (++measureCount_ >= 8192)
            {
                measurePhase_ = 2;
                const bool ok = measurePeak_ > 4.0f * measureNoise_ && measurePeak_ > 1e-3f && measureBest_ > 0;
                latency_.store(ok ? std::min(measureBest_, kMaxDelay - 1) : std::min(3 * io.frames, kMaxDelay - 1), std::memory_order_relaxed);
                latencyMeasured_.store(ok, std::memory_order_relaxed);
                return;
            }
        }
    }
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

    if (Song* s = incoming_.exchange(nullptr, std::memory_order_acq_rel))
        adopt(s);

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
    if (loop && measurePhase_ < 2 && latency_.load(std::memory_order_relaxed) > 0)
        measurePhase_ = 2; // a fixed latency from the settings

    if (const auto m = muteMask.load(std::memory_order_relaxed), s = soloMask.load(std::memory_order_relaxed); m != seenMute_ || s != seenSolo_)
    {
        seenMute_ = m;
        seenSolo_ = s;
        masksDirty_ = true;
    }
    for (Event e; events_.pop(e);)
        handle(e);
    applySettings();
    if (masksDirty_)
    {
        applyMasks();
        masksDirty_ = false;
    }

    // The route.
    int route = static_cast<int>(FilterRoute::Off);
    if (loop && measurePhase_ >= 2)
        route = masterFilter.load(std::memory_order_relaxed) ? static_cast<int>(FilterRoute::Master)
              : (filterHeld_ | filterLatchBits_) != 0 && song_ != nullptr && song_->wet ? static_cast<int>(FilterRoute::Channels)
                                                                                           : static_cast<int>(FilterRoute::Off);
    if (route != routeNow_)
    {
        routePrev_ = routeNow_;
        routeNow_ = route;
        fade_ = kFade;
    }
    route_.store(routeNow_, std::memory_order_relaxed);
    measuring_.store(loop && measurePhase_ < 2, std::memory_order_relaxed);

    const float volume = mainVolume.load(std::memory_order_relaxed);
    const float gain = std::pow(10.0f, gainDb.load(std::memory_order_relaxed) / 20.0f) * volume * volume;
    const int delay = loop ? std::clamp(latency_.load(std::memory_order_relaxed), 0, kMaxDelay - 1) : 0;
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
            clockChunk(frame_, frame_ + static_cast<std::uint64_t>(n));
        }

        float* ml = io.out[0] != nullptr ? io.out[0] + done : nullptr;
        float* mr = io.out[1] != nullptr ? io.out[1] + done : nullptr;
        float* sl = sendL != nullptr ? sendL + done : nullptr;
        float* sr2 = sendR != nullptr ? sendR + done : nullptr;
        const float* rl = loop ? io.in[2] + done : nullptr;
        const float* rr = loop ? io.in[3] + done : nullptr;

        if (loop && measurePhase_ < 2)
            measure(io, sl, sr2, done, n);

        for (int i = 0; i < n; ++i)
        {
            float outL, outR, sendLv = 0, sendRv = 0;
            if (loop)
            {
                // The direct paths, delayed by the loop's latency (dry and wet separately).
                const int w = delayWrite_;
                const int rd = (w - delay + kMaxDelay) % kMaxDelay;
                delayL_[static_cast<std::size_t>(w)] = dl[i];
                delayR_[static_cast<std::size_t>(w)] = dr[i];
                const float ddl = delayL_[static_cast<std::size_t>(rd)], ddr = delayR_[static_cast<std::size_t>(rd)];
                const float retL = rl[i], retR = rr[i];
                auto mixFor = [&](int r, float& L, float& R, float& SL, float& SR) {
                    switch (static_cast<FilterRoute>(r))
                    {
                    case FilterRoute::Off: L = ddl; R = ddr; SL = SR = 0; break;
                    case FilterRoute::Channels: L = ddl + retL; R = ddr + retR; SL = wl[i]; SR = wr[i]; break;
                    case FilterRoute::Master: L = retL; R = retR; SL = dl[i] + wl[i]; SR = dr[i] + wr[i]; break;
                    }
                };
                // In the Off route the wet stem carries nothing (no channel is routed), so the dry line is the mix.
                float L = 0, R = 0, SL = 0, SR = 0;
                mixFor(routeNow_, L, R, SL, SR);
                if (fade_ > 0)
                {
                    float L0 = 0, R0 = 0, SL0 = 0, SR0 = 0;
                    mixFor(routePrev_, L0, R0, SL0, SR0);
                    const float k = static_cast<float>(fade_) / kFade;
                    L = L * (1 - k) + L0 * k;
                    R = R * (1 - k) + R0 * k;
                    --fade_;
                }
                outL = L;
                outR = R;
                sendLv = SL;
                sendRv = SR;
                delayWrite_ = (w + 1) % kMaxDelay;
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
            if (loop && measurePhase_ >= 2)
            {
                sl[i] = sendLv;
                sr2[i] = sendRv;
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
