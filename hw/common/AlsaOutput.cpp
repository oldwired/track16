#include "common/AlsaOutput.h"


#include "ProjectIdentity.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace platform::common
{
namespace
{
// Denormals flushed to zero on the audio thread: the Cortex-A9's VFP traps on them (slow), NEON always flushes.
void enableFlushToZero() noexcept
{
#if defined(__arm__)
    unsigned fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr | (1u << 24)));
#elif defined(__x86_64__)
    unsigned csr;
    __asm__ volatile("stmxcsr %0" : "=m"(csr));
    csr |= 0x8040; // FTZ | DAZ
    __asm__ volatile("ldmxcsr %0" : : "m"(csr));
#endif
}
} // namespace

namespace
{
// Round to nearest with saturation, no libm call per sample (the A9 has no single-instruction lrintf in softfp builds).
inline std::int16_t toS16(float x) noexcept
{
    x = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
    const float s = x * 32767.0f;
    return static_cast<std::int16_t>(static_cast<int>(s + (s >= 0.0f ? 0.5f : -0.5f)));
}

// S24_LE: a sign-extended 24-bit sample in the low three bytes of a 32-bit word (ALSA's S24_LE container).
inline std::int32_t toS24(float x) noexcept
{
    x = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
    const float s = x * 8388607.0f;
    return static_cast<std::int32_t>(s + (s >= 0.0f ? 0.5f : -0.5f));
}

inline std::uint64_t monotonicNs() noexcept
{
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

inline void sleepMs(long ms) noexcept
{
    timespec ts {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
    {
    }
}

// A two-channel PCM at the main stream's rate and period, non-blocking, S24_LE when the codec has it (the AK4452's
// format under the stock firmware) else S16_LE. Null and `info` on failure.
snd_pcm_t* openStereo(const std::string& name, snd_pcm_stream_t stream, unsigned rate, unsigned period, unsigned periods,
                      bool& s24, std::string& info)
{
    snd_pcm_t* pcm = nullptr;
    int err = snd_pcm_open(&pcm, name.c_str(), stream, SND_PCM_NONBLOCK);
    if (err < 0)
    {
        info = "open " + name + ": " + snd_strerror(err);
        return nullptr;
    }
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_uframes_t p = period, buffer = static_cast<snd_pcm_uframes_t>(period) * (periods > 1 ? periods : 2);
    int dir = 0;
    s24 = snd_pcm_hw_params_test_format(pcm, hw, SND_PCM_FORMAT_S24_LE) == 0;
    if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0
        || (err = snd_pcm_hw_params_set_format(pcm, hw, s24 ? SND_PCM_FORMAT_S24_LE : SND_PCM_FORMAT_S16_LE)) < 0
        || (err = snd_pcm_hw_params_set_channels(pcm, hw, 2)) < 0
        || (err = snd_pcm_hw_params_set_rate(pcm, hw, rate, 0)) < 0
        || (err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &p, &dir)) < 0
        || (err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0
        || (err = snd_pcm_hw_params(pcm, hw)) < 0)
    {
        info = "hw params on " + name + ": " + snd_strerror(err);
        snd_pcm_close(pcm);
        return nullptr;
    }
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, stream == SND_PCM_STREAM_PLAYBACK ? period : 1);
    snd_pcm_sw_params(pcm, sw);
    return pcm;
}

unsigned envUnsigned(const char* suffix, unsigned fallback)
{
    const std::string key = std::string(identity::envPrefix) + suffix;
    const char* v = std::getenv(key.c_str());
    if (v == nullptr || *v == 0)
        return fallback;
    char* end = nullptr;
    const unsigned long n = std::strtoul(v, &end, 10);
    return end != nullptr && *end == 0 && n > 0 ? static_cast<unsigned>(n) : fallback;
}
} // namespace

bool parsePortMap(const std::string& text, AlsaPortMap& base)
{
    static constexpr const char* kNames[] = {"main", "out34", "out56", "out78", "phones", "filtersend"};
    AlsaPortMap map = base;
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t end = text.find(',', at);
        if (end == std::string::npos)
            end = text.size();
        const std::string field = text.substr(at, end - at);
        at = end + 1;
        if (field.empty())
            continue;
        const auto colon = field.find(':'), slash = field.find('/');
        if (colon == std::string::npos || slash == std::string::npos || slash < colon)
            return false;
        const std::string name = field.substr(0, colon);
        int port = -1;
        for (int p = 0; p < static_cast<int>(AudioPort::Count); ++p)
            if (name == kNames[p])
                port = p;
        char* e1 = nullptr;
        char* e2 = nullptr;
        const long l = std::strtol(field.c_str() + colon + 1, &e1, 10);
        const long r = std::strtol(field.c_str() + slash + 1, &e2, 10);
        if (port < 0 || e1 != field.c_str() + slash || *e2 != 0 || l < -1 || l > 63 || r < -1 || r > 63 || (l < 0) != (r < 0))
            return false;
        map[static_cast<std::size_t>(port)] = {static_cast<AudioPort>(port), static_cast<std::int8_t>(l),
                                               static_cast<std::int8_t>(r)};
    }
    base = map;
    return true;
}

bool AlsaOutput::openAux(std::string& info)
{
    snd_pcm_t* pcm = nullptr;
    int err = snd_pcm_open(&pcm, device_.auxDevice.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0)
    {
        info = "open " + device_.auxDevice + ": " + snd_strerror(err);
        return false;
    }
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_uframes_t period = period_, buffer = static_cast<snd_pcm_uframes_t>(period_) * (periods_ > 1 ? periods_ : 2);
    int dir = 0;
    // The AK4452 (PHONES) runs S24_LE under the stock firmware; its S16 path was never exercised there and the unit's
    // phones distorted badly on it. S16 stays the fallback for codecs that lack S24_LE.
    auxS24_ = ! device_.auxS16 && snd_pcm_hw_params_test_format(pcm, hw, SND_PCM_FORMAT_S24_LE) == 0;
    if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0
        || (err = snd_pcm_hw_params_set_format(pcm, hw, auxS24_ ? SND_PCM_FORMAT_S24_LE : SND_PCM_FORMAT_S16_LE)) < 0
        || (err = snd_pcm_hw_params_set_channels(pcm, hw, device_.auxChannels)) < 0
        || (err = snd_pcm_hw_params_set_rate(pcm, hw, rate_, 0)) < 0
        || (err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &dir)) < 0
        || (err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0
        || (err = snd_pcm_hw_params(pcm, hw)) < 0)
    {
        info = "hw params on " + device_.auxDevice + ": " + snd_strerror(err);
        snd_pcm_close(pcm);
        return false;
    }
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, period_);
    snd_pcm_sw_params(pcm, sw);
    const bool linked = snd_pcm_link(static_cast<snd_pcm_t*>(pcm_), pcm) == 0; // best effort: one start for both
    aux_ = pcm;
    snd_pcm_hw_params_get_period_size(hw, &period, &dir);
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);
    info = device_.auxDevice + (auxS24_ ? " S24_LE x" : " S16_LE x") + std::to_string(device_.auxChannels) + ", period " + std::to_string(period)
         + ", buffer " + std::to_string(buffer) + (linked ? ", linked" : ", not linked");
    return true;
}

void AlsaOutput::writeAux() noexcept
{
    auto* pcm = static_cast<snd_pcm_t*>(aux_);
    const float* l = out_[2 * static_cast<std::size_t>(device_.auxPort)];
    const float* r = out_[2 * static_cast<std::size_t>(device_.auxPort) + 1];
    if (pcm == nullptr || l == nullptr)
        return;
    const unsigned ch = device_.auxChannels;
    const void* data = nullptr;
    if (auxS24_)
    {
        const float g = device_.auxGain;
        std::int32_t* out = auxInterleaved32_.data();
        for (unsigned i = 0; i < period_; ++i, out += ch)
        {
            out[0] = toS24(l[i] * g);
            out[1] = toS24(r[i] * g);
        }
        data = auxInterleaved32_.data();
    }
    else
    {
        const float g = device_.auxGain;
        std::int16_t* out = auxInterleaved_.data();
        for (unsigned i = 0; i < period_; ++i, out += ch)
        {
            out[0] = toS16(l[i] * g);
            out[1] = toS16(r[i] * g);
        }
        data = auxInterleaved_.data();
    }
    const auto n = snd_pcm_writei(pcm, data, period_);
    if (n == static_cast<snd_pcm_sframes_t>(period_))
        return;
    auxDrops_.fetch_add(1, std::memory_order_relaxed); // full (-EAGAIN), short write or xrun: never wait for it
    if (n < 0 && n != -EAGAIN)
        snd_pcm_recover(pcm, static_cast<int>(n), 1);
}

bool AlsaOutput::secondary(const PortInfo& p) const noexcept
{
    return (! device_.auxDevice.empty() && p.port == device_.auxPort)
        || (! device_.loopDevice.empty() && p.port == AudioPort::FilterSend);
}

bool AlsaOutput::openLoop(std::string& info)
{
    std::string why;
    auto* out = openStereo(device_.loopDevice, SND_PCM_STREAM_PLAYBACK, rate_, period_, periods_, loopOutS24_, why);
    auto* in = out != nullptr
                 ? openStereo(device_.loopDevice, SND_PCM_STREAM_CAPTURE, rate_, period_, periods_, loopInS24_, why)
                 : nullptr;
    if (in == nullptr)
    {
        if (out != nullptr)
            snd_pcm_close(out);
        info = why;
        return false;
    }
    // Not linked to the main PCM (a first unit run with a linked loop stopped right after the audio started): a
    // linked capture's overrun stops the whole group. The send starts at its threshold; the return starts now and is
    // read one period behind (readLoop). Its clocks come from hw:ak4452audio,0 (the PHONES PCM, started with the main
    // stream): until they run, the return brings nothing and is reported absent.
    // (Ported from freesp16's AlsaOutput, which found this on the unit.)
    snd_pcm_prepare(in);
    snd_pcm_start(in);
    loopOut_ = out;
    loopIn_ = in;
    const auto n = static_cast<std::size_t>(period_) * 2;
    loopOut32_.assign(loopOutS24_ ? n : 0, 0);
    loopOut16_.assign(loopOutS24_ ? 0 : n, 0);
    loopIn32_.assign(loopInS24_ ? n : 0, 0);
    loopIn16_.assign(loopInS24_ ? 0 : n, 0);
    loopReturn_.assign(n, 0.0f);
    info = device_.loopDevice + " send " + (loopOutS24_ ? "S24_LE" : "S16_LE") + " return "
         + (loopInS24_ ? "S24_LE" : "S16_LE") + " x2, not linked";
    return true;
}

void AlsaOutput::closeLoop() noexcept
{
    for (void** h : {&loopOut_, &loopIn_})
        if (*h != nullptr)
        {
            auto* pcm = static_cast<snd_pcm_t*>(*h);
            snd_pcm_drop(pcm);
            snd_pcm_close(pcm);
            *h = nullptr;
        }
}

void AlsaOutput::readLoop() noexcept
{
    auto* pcm = static_cast<snd_pcm_t*>(loopIn_);
    if (pcm == nullptr)
        return;
    void* buf = loopInS24_ ? static_cast<void*>(loopIn32_.data()) : static_cast<void*>(loopIn16_.data());
    const auto p = static_cast<snd_pcm_sframes_t>(period_);
    auto avail = snd_pcm_avail_update(pcm);
    if (avail < 0) // an overrun: restart (the app measures the loop again)
    {
        snd_pcm_recover(pcm, static_cast<int>(avail), 1);
        snd_pcm_start(pcm);
        avail = 0;
    }
    // One period behind, never more than two: the read below then always finds a whole period (the capture and the
    // main stream run off one clock family, but their period boundaries are not aligned). A short read would leave a
    // zero-filled gap every period: a buzz at the period rate on any hiss or offset the return carries.
    if (avail > 3 * p)
    {
        snd_pcm_forward(pcm, static_cast<snd_pcm_uframes_t>(avail - 2 * p));
        avail = 2 * p;
    }
    snd_pcm_sframes_t n = 0;
    if (avail >= p)
    {
        n = snd_pcm_readi(pcm, buf, period_);
        if (n < 0)
        {
            if (n != -EAGAIN)
            {
                snd_pcm_recover(pcm, static_cast<int>(n), 1);
                snd_pcm_start(pcm);
            }
            n = 0;
        }
    }
    const auto got = static_cast<unsigned>(n);
    if (got < period_)
        loopDrops_.fetch_add(1, std::memory_order_relaxed);
    float* l = loopReturn_.data();
    float* r = l + period_;
    constexpr float k24 = 1.0f / 8388608.0f, k16 = 1.0f / 32768.0f;
    for (unsigned i = 0; i < got; ++i)
    {
        if (loopInS24_) // sign-extend the low 24 bits
        {
            l[i] = static_cast<float>(static_cast<std::int32_t>(static_cast<std::uint32_t>(loopIn32_[2 * i]) << 8) >> 8) * k24;
            r[i] = static_cast<float>(static_cast<std::int32_t>(static_cast<std::uint32_t>(loopIn32_[2 * i + 1]) << 8) >> 8) * k24;
        }
        else
        {
            l[i] = static_cast<float>(loopIn16_[2 * i]) * k16;
            r[i] = static_cast<float>(loopIn16_[2 * i + 1]) * k16;
        }
    }
    for (unsigned i = got; i < period_; ++i)
        l[i] = r[i] = 0.0f;
    float peak = 0.0f;
    for (unsigned i = 0; i < got; ++i)
        peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
    if (peak > loopReturnPeak_.load(std::memory_order_relaxed))
        loopReturnPeak_.store(peak, std::memory_order_relaxed);
    loopFrames_.fetch_add(got, std::memory_order_relaxed);
    // A dead return (no clock, a failed codec) is reported absent, so the app plays without the loop instead of
    // playing silence; it comes back with the first frames.
    if (got > 0)
    {
        loopStarved_ = 0;
        if (! returnLive_)
        {
            returnLive_ = true;
            in_[2] = l;
            in_[3] = r;
        }
    }
    else if (returnLive_ && ++loopStarved_ >= loopDeadPeriods_)
    {
        returnLive_ = false;
        in_[2] = in_[3] = nullptr;
    }
    loopLive_.store(returnLive_, std::memory_order_relaxed);
}

void AlsaOutput::writeLoop() noexcept
{
    auto* pcm = static_cast<snd_pcm_t*>(loopOut_);
    constexpr auto fs = static_cast<std::size_t>(AudioPort::FilterSend);
    const float* l = out_[2 * fs];
    const float* r = out_[2 * fs + 1];
    if (pcm == nullptr || l == nullptr)
        return;
    const float g = device_.loopGain;
    float peak = 0.0f;
    for (unsigned i = 0; i < period_; ++i)
        peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
    peak *= g; // as the send DAC sees it
    if (peak > loopSendPeak_.load(std::memory_order_relaxed))
        loopSendPeak_.store(peak, std::memory_order_relaxed);
    const void* data = nullptr;
    if (loopOutS24_)
    {
        for (unsigned i = 0; i < period_; ++i)
        {
            loopOut32_[2 * i] = toS24(l[i] * g);
            loopOut32_[2 * i + 1] = toS24(r[i] * g);
        }
        data = loopOut32_.data();
    }
    else
    {
        for (unsigned i = 0; i < period_; ++i)
        {
            loopOut16_[2 * i] = toS16(l[i] * g);
            loopOut16_[2 * i + 1] = toS16(r[i] * g);
        }
        data = loopOut16_.data();
    }
    const auto n = snd_pcm_writei(pcm, data, period_);
    if (n == static_cast<snd_pcm_sframes_t>(period_))
        return;
    loopDrops_.fetch_add(1, std::memory_order_relaxed);
    if (n < 0 && n != -EAGAIN)
        snd_pcm_recover(pcm, static_cast<int>(n), 1);
}

bool AlsaOutput::open(unsigned wantPeriod, unsigned wantPeriods, bool exactPeriod, std::string& info)
{
    snd_pcm_t* pcm = nullptr;
    int err = snd_pcm_open(&pcm, device_.device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0)
    {
        info = "alsa: open " + device_.device + ": " + snd_strerror(err);
        return false;
    }
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    unsigned rate = config_.sampleRate;
    snd_pcm_uframes_t period = wantPeriod, buffer = static_cast<snd_pcm_uframes_t>(wantPeriod) * wantPeriods;
    int dir = 0;
    if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0
        || (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0
        || (err = snd_pcm_hw_params_set_channels(pcm, hw, device_.channels)) < 0
        || (err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, &dir)) < 0
        || (err = exactPeriod ? snd_pcm_hw_params_set_period_size(pcm, hw, period, 0)
                              : snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, &dir)) < 0
        || (err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0
        || (err = snd_pcm_hw_params(pcm, hw)) < 0)
    {
        info = "alsa: hw params on " + device_.device + ": " + snd_strerror(err);
        snd_pcm_close(pcm);
        return false;
    }
    snd_pcm_hw_params_get_period_size(hw, &period, &dir); // what the driver granted
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);
    if (exactPeriod && (period != period_ || rate != rate_))
    {
        info = "alsa: reopen got another period/rate";
        snd_pcm_close(pcm);
        return false;
    }
    // Start as soon as one period is queued; wake when a whole period fits (one write per process() call).
    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, period);
    snd_pcm_sw_params_set_avail_min(pcm, sw, period);
    snd_pcm_sw_params(pcm, sw);
    pcm_ = pcm;
    period_ = static_cast<unsigned>(period);
    buffer_ = static_cast<unsigned>(buffer);
    periods_ = period_ > 0 ? buffer_ / period_ : 0;
    rate_ = rate;
    return true;
}

void AlsaOutput::close() noexcept
{
    if (pcm_ != nullptr)
    {
        snd_pcm_drop(static_cast<snd_pcm_t*>(pcm_));
        snd_pcm_close(static_cast<snd_pcm_t*>(pcm_));
        pcm_ = nullptr;
    }
}

bool AlsaOutput::start(const AudioConfig& config, platform::AudioProcessor& processor, std::string& info)
{
    stop();
    config_ = config;
    processor_ = &processor;
    const unsigned wantPeriod = std::clamp(envUnsigned("_AUDIO_PERIOD", device_.periodFrames ? device_.periodFrames : config.periodFrames), 32u, 2048u);
    const unsigned wantPeriods = std::clamp(envUnsigned("_AUDIO_PERIODS", device_.periods ? device_.periods : config.periods), 2u, 8u);
    if (! open(wantPeriod, wantPeriods, false, info))
        return false;
    // The second PCM (imx6 PHONES); its port is absent when it does not open.
    ports_ = device_.ports;
    std::string auxInfo;
    const bool hasAux = ! device_.auxDevice.empty();
    const auto auxIndex = static_cast<std::size_t>(device_.auxPort);
    if (hasAux)
    {
        const bool ok = device_.auxChannels >= 2 && openAux(auxInfo);
        ports_[auxIndex] = {device_.auxPort, static_cast<std::int8_t>(ok ? 0 : -1), static_cast<std::int8_t>(ok ? 1 : -1)};
        const std::size_t auxSamples = ok ? static_cast<std::size_t>(period_) * device_.auxChannels : 0;
        auxInterleaved_.assign(ok && ! auxS24_ ? auxSamples : 0, 0);
        auxInterleaved32_.assign(ok && auxS24_ ? auxSamples : 0, 0);
        if (! ok)
            auxInfo = "unavailable: " + auxInfo;
    }
    std::string loopInfo;
    const bool hasLoop = ! device_.loopDevice.empty();
    constexpr auto loopIndex = static_cast<std::size_t>(AudioPort::FilterSend);
    in_.fill(nullptr);
    if (hasLoop)
    {
        const bool ok = openLoop(loopInfo);
        ports_[loopIndex] = {AudioPort::FilterSend, static_cast<std::int8_t>(ok ? 0 : -1), static_cast<std::int8_t>(ok ? 1 : -1)};
        if (ok)
        {
            in_[2] = loopReturn_.data();
            in_[3] = loopReturn_.data() + period_;
            returnLive_ = true; // until it stays silent for loopDeadPeriods_
            loopStarved_ = 0;
            loopDeadPeriods_ = std::max(1u, (rate_ / 2) / std::max(1u, period_));
            loopFrames_ = 0;
        }
        else
            loopInfo = "unavailable: " + loopInfo;
    }
    const auto isAux = [&](const PortInfo& p) { return secondary(p); };
    // Everything the audio thread touches is sized here (no allocation in run()): the app renders planar float
    // into one period per live port channel; absent ports get null pointers; silent channels stay zero.
    interleaved_.assign(static_cast<std::size_t>(period_) * device_.channels, 0);
    const auto live = [&](const PortInfo& p) {
        if (hasLoop && p.port == AudioPort::FilterSend)
            return loopOut_ != nullptr;
        return isAux(p) ? aux_ != nullptr
                        : p.left >= 0 && p.left < static_cast<int>(device_.channels) && p.right >= 0
                              && p.right < static_cast<int>(device_.channels);
    };
    int liveChannels = 0;
    for (const auto& p : ports_)
        liveChannels += live(p) ? 2 : 0;
    planar_.assign(static_cast<std::size_t>(period_) * static_cast<std::size_t>(liveChannels), 0.0f);
    out_.fill(nullptr);
    float* next = planar_.data();
    for (const auto& p : ports_)
        if (live(p))
        {
            out_[2 * static_cast<std::size_t>(p.port)] = next;
            out_[2 * static_cast<std::size_t>(p.port) + 1] = next + period_;
            next += 2 * period_;
        }
    processor.prepare(static_cast<double>(rate_), static_cast<int>(period_));
    periodsDone_ = 0;
    xruns_ = 0;
    reopens_ = 0;
    quit_ = false;
    thread_ = std::thread([this] { run(); });
    const double latencyMs = 1000.0 * buffer_ / (rate_ > 0 ? rate_ : 1);
    char ms[16];
    std::snprintf(ms, sizeof ms, "%.1f", latencyMs);
    info = "alsa: " + device_.device + " S16_LE x" + std::to_string(device_.channels) + " " + std::to_string(rate_) + " Hz, period "
         + std::to_string(period_) + " frames, buffer " + std::to_string(buffer_) + " frames (" + std::to_string(periods_)
         + " periods, " + ms + " ms), MAIN = ch " + std::to_string(ports_[0].left) + "/" + std::to_string(ports_[0].right);
    static constexpr const char* kNames[] = {"MAIN", "OUT3-4", "OUT5-6", "OUT7-8", "PHONES", "FILTER SEND"};
    for (std::size_t p = 1; p < ports_.size(); ++p)
        if (! isAux(ports_[p]) && live(ports_[p]))
            info += std::string(", ") + kNames[p] + " = ch " + std::to_string(ports_[p].left) + "/"
                  + std::to_string(ports_[p].right);
    if (hasAux)
        info += std::string(", ") + kNames[auxIndex] + " = " + auxInfo;
    if (hasLoop)
        info += std::string(", ") + kNames[loopIndex] + " + RETURN = " + loopInfo;
    return true;
}

void AlsaOutput::stop()
{
    quit_ = true;
    if (thread_.joinable())
        thread_.join();
    if (aux_ != nullptr)
    {
        snd_pcm_t* aux = static_cast<snd_pcm_t*>(aux_);
        if (pcm_ != nullptr)
            snd_pcm_unlink(aux);
        snd_pcm_drop(aux);
        snd_pcm_close(aux);
        aux_ = nullptr;
    }
    closeLoop();
    close();
}

AudioStats AlsaOutput::stats() const noexcept
{
    AudioStats s {periodsDone_.load(std::memory_order_relaxed), xruns_.load(std::memory_order_relaxed), rate_, period_,
                  realtime_.load(std::memory_order_relaxed), loopDrops_.load(std::memory_order_relaxed)};
    if (loopIn_ != nullptr)
    {
        s.loopFrames = loopFrames_.load(std::memory_order_relaxed);
        s.loopLive = loopLive_.load(std::memory_order_relaxed);
        s.loopSendPeak = loopSendPeak_.exchange(0.0f, std::memory_order_relaxed);
        s.loopReturnPeak = loopReturnPeak_.exchange(0.0f, std::memory_order_relaxed);
    }
    return s;
}

void AlsaOutput::convert() noexcept
{
    const unsigned ch = device_.channels;
    for (const auto& port : ports_)
    {
        if (secondary(port))
            continue; // the second PCM's port (writeAux) or the loop's send (writeLoop)
        const float* l = out_[2 * static_cast<std::size_t>(port.port)];
        const float* r = out_[2 * static_cast<std::size_t>(port.port) + 1];
        if (l == nullptr)
            continue;
        std::int16_t* out = interleaved_.data();
        const auto li = static_cast<std::size_t>(port.left), ri = static_cast<std::size_t>(port.right);
        for (unsigned i = 0; i < period_; ++i, out += ch)
        {
            out[li] = toS16(l[i]);
            out[ri] = toS16(r[i]);
        }
    }
}

bool AlsaOutput::reopen() noexcept
{
    // Failure path only (the codec is gone or wedged): there is no audio to protect, so this thread may allocate
    // (snd_pcm_open), sleep and retry. Same period and rate, or keep trying: the app's buffers stay as they are.
    close();
    while (! quit_.load(std::memory_order_relaxed))
    {
        sleepMs(100);
        std::string info;
        if (open(period_, periods_ > 1 ? periods_ : 2, true, info))
        {
            reopens_.fetch_add(1, std::memory_order_relaxed);
            std::fill(interleaved_.begin(), interleaved_.end(), std::int16_t {0});
            return true;
        }
    }
    return false;
}

void AlsaOutput::run()
{
    sched_param p {};
    p.sched_priority = config_.rtPriority;
    // best effort: may be refused without CAP_SYS_NICE
    realtime_ = pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) == 0;
    enableFlushToZero();
    const unsigned ch = device_.channels;
    const bool loop = loopIn_ != nullptr;
    const platform::AudioIO io {out_.data(), static_cast<int>(AudioPort::Count), loop ? in_.data() : nullptr, loop ? 4 : 0,
                              static_cast<int>(period_), 0};
    while (! quit_.load(std::memory_order_relaxed))
    {
        platform::AudioIO now = io;
        now.hostTimeNs = monotonicNs();
        readLoop(); // the analog filter's return of the last period (no-op without the loop)
        processor_->process(now);
        convert();
        snd_pcm_uframes_t done = 0;
        while (done < period_ && ! quit_.load(std::memory_order_relaxed))
        {
            auto* pcm = static_cast<snd_pcm_t*>(pcm_);
            const auto n = snd_pcm_writei(pcm, interleaved_.data() + done * ch, period_ - done);
            if (n >= 0)
            {
                done += static_cast<snd_pcm_uframes_t>(n);
                continue;
            }
            if (n == -EAGAIN || n == -EINTR)
                continue;
            if (n == -EPIPE || n == -ESTRPIPE)
                xruns_.fetch_add(1, std::memory_order_relaxed);
            if (snd_pcm_recover(pcm, static_cast<int>(n), 1) < 0 && ! reopen())
                return; // stop() was called while the device was gone
            break; // drop the rest of this period: the stream restarts with the next one
        }
        writeAux(); // PHONES on the second PCM, non-blocking
        writeLoop(); // the analog filter's send, non-blocking
        periodsDone_.fetch_add(1, std::memory_order_relaxed);
    }
}
} // namespace platform::common
