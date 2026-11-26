// Direct ALSA playback on a PCM opened by name ("alsa-direct"), shared by the SP-16 and the Linux desktop.
// The PCM name, its channel count and the logical port map are parameters (AlsaOutputConfig):
//  - imx6: the unit's main codec, hw:ak4458audio,0, 16 interleaved S16 channels at 44.1 kHz only, MAIN L/R on
//    channels 12/14; OUT3-4 = 8/10, OUT5-6 = 4/6, OUT7-8 = 0/2 (inferred, not verified; the setting audio_map
//    corrects them) and PHONES on the second PCM hw:ak4452audio,0 channels 0/1 (AlsaOutputConfig::auxDevice).
//  - linux: `default` (or the setting audio_device), 2 channels, MAIN = 0/1, the other ports absent; snd_pcm_writei
//    (RW interleaved access, no mmap), so it works through the PipeWire/PulseAudio ALSA plugins.
// JUCE's ALSA backend only opens devices it enumerated, and the codec is not always enumerable; this thin device
// opens it by name, runs its own SCHED_FIFO thread and writes only the live channels into a pre-zeroed interleaved
// buffer (no per-period conversion of silent channels).
//
// Period / latency: AudioConfig (periodFrames × periods, default 256 × 3 = 17.4 ms), overridden by AlsaOutputConfig
// and then by the environment `<PREFIX>_AUDIO_PERIOD` / `<PREFIX>_AUDIO_PERIODS` (e.g. 128 × 3 once measured on
// hardware). Period 32…2048 frames, 2…8 periods.
//
// Xrun recovery: an underrun (-EPIPE) or suspend (-ESTRPIPE) is counted in stats().xruns and recovered with
// snd_pcm_recover; the rest of that period is dropped and the stream restarts at the start threshold (one period).
// When recovery fails (device gone, -ENODEV/-EIO) the PCM is closed and reopened with the same format and period
// every 100 ms until it works or stop() is called — the app keeps its buffers (same period size), so nothing is
// reallocated. stats().realtime says whether SCHED_FIFO was granted.
#pragma once

#include "Platform.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace platform::common
{
// One entry per logical port, in AudioPort order.
using AlsaPortMap = std::array<PortInfo, static_cast<int>(AudioPort::Count)>;

// MAIN on interleaved channels left/right, every other port absent.
constexpr AlsaPortMap mainOnly(std::int8_t left, std::int8_t right) noexcept
{
    return {{{AudioPort::Main, left, right},
             {AudioPort::Out34, -1, -1},
             {AudioPort::Out56, -1, -1},
             {AudioPort::Out78, -1, -1},
             {AudioPort::Phones, -1, -1},
             {AudioPort::FilterSend, -1, -1}}};
}

// "main:12/14,out34:8/10,…" (names main, out34, out56, out78, phones, filtersend; L/R = interleaved channels, -1 =
// absent) applied over `base`; false (and `base` unchanged) when a field does not parse.
bool parsePortMap(const std::string& text, AlsaPortMap& base);

struct AlsaOutputConfig
{
    std::string device = "default";
    unsigned channels = 2;
    unsigned periodFrames = 0; // 0 = AudioConfig::periodFrames
    unsigned periods = 0;      // 0 = AudioConfig::periods
    AlsaPortMap ports = mainOnly(0, 1);
    // An optional second PCM that plays one logical port on its channels 0/1 (imx6: PHONES on
    // hw:ak4452audio,0). Opened non-blocking with the main PCM's rate and period and written once per period after
    // it, so it can never stall the main stream (a full buffer drops that period, counted in auxDrops()); the two
    // PCMs are linked (snd_pcm_link, best effort) to start together; both run off the codec's one clock.
    // Empty = none. When it does not open, the port is absent (logged in start()'s info).
    std::string auxDevice {};
    unsigned auxChannels = 2;
    AudioPort auxPort = AudioPort::Phones;
    // A fixed gain on the aux PCM's samples (imx6 PHONES: 0.5, the −6 dB the stock firmware applies to every DAC pair,
    // which the AK4452 headphone stage needs as headroom; setting phones_gain).
    float auxGain = 1.0f;
    bool auxS16 = false; // force S16_LE on the aux PCM (setting phones_format=s16; default: S24_LE when offered)
    // An optional send/return loop (imx6: the analog filter on hw:ak4452audio,1, "Send & Return"): its playback plays
    // the FilterSend port on channels 0/1 and its capture becomes the app's input channels 2/3, the filter's return.
    // Both non-blocking and NOT linked to the main PCM. The capture runs from the open and is read one period behind,
    // so a read finds a whole period (one it cannot fill is silence, counted in loopDrops()); a return that brings no
    // frame for 0.5 s is reported absent (inputs 2/3 null) until frames come back. Empty = none; when either
    // direction does not open, FilterSend and the return are absent.
    std::string loopDevice {};
    // A fixed gain on the loop's send (imx6: 0.5, the stock firmware's −6 dB on its filter send pair, so the analog
    // drive stage has headroom; setting filter_send_gain).
    float loopGain = 1.0f;
};

class AlsaOutput final : public AudioDevice
{
public:
    explicit AlsaOutput(AlsaOutputConfig config = {}) : device_(std::move(config)), ports_(device_.ports) {}
    ~AlsaOutput() override { stop(); }

    std::string_view id() const noexcept override { return "alsa-direct"; }
    std::span<const PortInfo> ports() const noexcept override { return ports_; }
    bool start(const AudioConfig&, platform::AudioProcessor&, std::string& info) override; // message thread
    void stop() override;
    AudioStats stats() const noexcept override;
    std::uint64_t reopens() const noexcept { return reopens_.load(std::memory_order_relaxed); }
    std::uint64_t auxDrops() const noexcept { return auxDrops_.load(std::memory_order_relaxed); }
    std::uint64_t loopDrops() const noexcept { return loopDrops_.load(std::memory_order_relaxed); }
    bool auxOpen() const noexcept { return aux_ != nullptr; }

private:
    // Opens and configures the PCM; `exactPeriod` (on reopen) refuses any period other than period_.
    bool open(unsigned wantPeriod, unsigned wantPeriods, bool exactPeriod, std::string& info);
    void close() noexcept;
    bool reopen() noexcept; // audio thread, failure path only
    void run();
    void convert() noexcept;
    bool openAux(std::string& info);
    void writeAux() noexcept; // audio thread: one period, non-blocking
    bool openLoop(std::string& info);
    void closeLoop() noexcept;
    void readLoop() noexcept;  // audio thread: the return's period (silence where it fell short), non-blocking
    void writeLoop() noexcept; // audio thread: the FilterSend port, non-blocking
    bool secondary(const PortInfo& p) const noexcept; // a port on aux_ / the loop, not in interleaved_

    AlsaOutputConfig device_;
    AudioConfig config_ {};
    AlsaPortMap ports_;
    platform::AudioProcessor* processor_ = nullptr;
    void* pcm_ = nullptr; // snd_pcm_t*
    unsigned period_ = 0, periods_ = 0, rate_ = 0, buffer_ = 0;
    std::vector<std::int16_t> interleaved_;
    void* aux_ = nullptr;                    // snd_pcm_t* of AlsaOutputConfig::auxDevice (null = none)
    std::vector<std::int16_t> auxInterleaved_;
    std::vector<std::int32_t> auxInterleaved32_; // S24_LE (auxS24_)
    bool auxS24_ = false;                        // the aux PCM runs S24_LE (else S16_LE)
    void* loopOut_ = nullptr;                    // snd_pcm_t* playback of loopDevice (null = no loop)
    void* loopIn_ = nullptr;                     // snd_pcm_t* capture of loopDevice
    bool loopOutS24_ = false, loopInS24_ = false;
    std::vector<std::int32_t> loopOut32_, loopIn32_;
    std::vector<std::int16_t> loopOut16_, loopIn16_;
    std::vector<float> loopReturn_;                     // L then R, one period each
    std::array<const float*, 4> in_ {};                 // AudioIO::in: 0/1 none (LINE IN), 2/3 the loop return
    unsigned loopStarved_ = 0, loopDeadPeriods_ = 1;   // audio thread: periods without return frames, the limit
    bool returnLive_ = false;                          // audio thread: in_[2]/[3] point at loopReturn_
    std::atomic<std::uint64_t> loopFrames_ {0};
    std::atomic<bool> loopLive_ {false};
    mutable std::atomic<float> loopSendPeak_ {0.0f}, loopReturnPeak_ {0.0f};
    std::vector<float> planar_;                                         // one period per live port channel
    std::array<float*, 2 * static_cast<int>(AudioPort::Count)> out_ {}; // AudioIO::out (null = absent port)
    std::thread thread_;
    std::atomic<bool> quit_ {false}, realtime_ {false};
    std::atomic<std::uint64_t> periodsDone_ {0}, xruns_ {0}, reopens_ {0}, auxDrops_ {0}, loopDrops_ {0};
};
} // namespace platform::common
