#include "imx6/AnalogBoard.h"

#include "ProjectIdentity.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace platform::imx6::panel
{
namespace
{
constexpr const char* kGpioDevice = "/dev/gpiodrv";
constexpr const char* kDacDevice = "/dev/dac_spi4.0";

void analogLog(const char* what, const char* device, int err) noexcept
{
    std::printf("[sp16-mod] %s ANALOG %s %s%s%s\n", identity::envPrefix, what, device, err != 0 ? ": " : "",
                err != 0 ? std::strerror(err) : "");
    std::fflush(stdout);
}
} // namespace

AnalogBoard::~AnalogBoard()
{
    if (gpioFd_ >= 0)
        ::close(gpioFd_);
    if (dacFd_ >= 0)
        ::close(dacFd_);
}

unsigned AnalogBoard::dacCode(int value1023) noexcept
{
    const double x = static_cast<double>(std::clamp(value1023, 0, 1023)) / 1023.0;
    const auto code = static_cast<int>(kDacMin + (kDacMax - kDacMin) * x * x * x + 0.5);
    return static_cast<unsigned>(std::clamp(code, kDacMin, kDacMax));
}

bool AnalogBoard::writeLine(int line, unsigned char level, const char* what) noexcept
{
    // Write only: a read() on /dev/gpiodrv turns the line into an input.
    if (gpioFd_ < 0)
    {
        gpioFd_ = ::open(kGpioDevice, O_WRONLY | O_CLOEXEC);
        if (gpioFd_ < 0)
        {
            gpioFailed_ = true;
            analogLog("gpio FAILED open", kGpioDevice, errno);
            return false;
        }
    }
    if (::lseek(gpioFd_, line, SEEK_SET) != line || ::write(gpioFd_, &level, 1) != 1)
    {
        gpioFailed_ = true;
        analogLog(what, kGpioDevice, errno);
        return false;
    }
    return true;
}

void AnalogBoard::setActive(bool lit) noexcept
{
    if (! enabled_.load(std::memory_order_relaxed) || gpioFailed_ || lastActive_ == static_cast<int>(lit))
        return;
    if (! limitDone_ && limit_.load(std::memory_order_relaxed))
    {
        if (! writeLine(kLimitLine, 1, "limiter FAILED write"))
            return;
        analogLog("limiter ON (GPIO5_IO06 = 1)", kGpioDevice, 0);
    }
    limitDone_ = true;
    if (! writeLine(kSelectLine, selectLevel(lit), "filter-select FAILED write"))
        return;
    std::printf("[sp16-mod] %s ANALOG filter-select %s (GPIO5_IO05 = %d)\n", identity::envPrefix, lit ? "FILTER" : "THRU",
                selectLevel(lit));
    std::fflush(stdout);
    lastActive_ = lit ? 1 : 0;
}

void AnalogBoard::setHpCutoff(int value1023) noexcept
{
    if (! enabled_.load(std::memory_order_relaxed) || dacFailed_)
        return;
    const unsigned code = dacCode(value1023);
    if (lastCode_ == static_cast<int>(code))
        return;
    if (dacFd_ < 0)
    {
        dacFd_ = ::open(kDacDevice, O_WRONLY | O_CLOEXEC);
        if (dacFd_ < 0)
        {
            dacFailed_ = true;
            analogLog("hp-cutoff FAILED open", kDacDevice, errno);
            return;
        }
    }
    const unsigned char bytes[2] = {static_cast<unsigned char>(code & 0xFF), static_cast<unsigned char>((code >> 8) & 0x0F)};
    if (! dacWriteOk(::write(dacFd_, bytes, 2)))
    {
        // A transient error is retried at the next change; a device that keeps failing is given up, logged once.
        const int err = errno;
        if (dacErrors_++ == 0)
            analogLog("hp-cutoff write error (retrying)", kDacDevice, err);
        if (dacErrors_ >= kDacRetries)
        {
            dacFailed_ = true;
            analogLog("hp-cutoff FAILED write", kDacDevice, err);
        }
        return;
    }
    dacErrors_ = 0;
    if (lastCode_ < 0)
        analogLog("hp-cutoff ok (12-bit code 4…3276 per HP CUTOFF change)", kDacDevice, 0);
    lastCode_ = static_cast<int>(code);
}
} // namespace platform::imx6::panel
