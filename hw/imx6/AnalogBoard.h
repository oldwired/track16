// The analog filter board's digital controls (/dev/gpiodrv and /dev/dac_spi4.0). The DAC handling and the limiter come
// from freesp16's AnalogBoard; the select line's meaning was found on the unit (Track16 test 2026-11-26).
//
//   filter select  GPIO5_IO05 "SEL_xDSF/THRU": the vendor GPIO driver, one byte per line at offset (bank − 1)·32 +
//                  number = 133 (seek, then write; never read(): the driver turns the line into an input first).
//                  1 = MAIN INSERT: the main DAC goes through the filter to MAIN OUT (the filter output also reaches
//                      the return ADC); the stock firmware sets it while ANALOG FILTER ACTIVE is lit.
//                  0 = LOOP: MAIN OUT comes straight from the main DAC; the filter takes the send DAC
//                      (hw:ak4452audio,1) and feeds only the return ADC: the send/return loop for single channels.
//                  On the unit: with 1, the return carried the mix while nothing was sent and LP CUTOFF acted on MAIN
//                  OUT; with 0, the loop's click came back (16 ms) and MAIN OUT was unfiltered.
//   limiter        GPIO5_IO06 "LMT_xOFF/ON": written 1 (ON) once, at the first switch, as the stock firmware does at
//                  its start (the kernel boots it at 0); filter_limit=0 leaves it alone.
//   HP CUTOFF      a 12-bit multiplying DAC (DAC7811 type): two bytes little-endian per change of the HP CUTOFF pot, the
//                  code in bits 0–11 (the driver adds the load-and-update command itself), kept inside the factory span
//                  4…3276 with the law 4 + 3272·(i/1023)³. The vendor driver returns 0 from a successful write.
//
// LP CUTOFF and RESONANCE are analog pots on that board that no microcontroller reads: they act on whatever passes the
// filter. A device that does not open fails once, is reported in one log line and is not retried. Thread-safe by split:
// setInsert() runs on the message thread, setHpCutoff() on the panel reader thread; each owns its descriptor and state.
#pragma once

#include <atomic>

namespace platform::imx6::panel
{
class AnalogBoard
{
public:
    static constexpr int kSelectLine = 4 * 32 + 5; // GPIO5_IO05 SEL_xDSF/THRU
    static constexpr int kLimitLine = 4 * 32 + 6;  // GPIO5_IO06 LMT_xOFF/ON
    static constexpr int kDacMin = 4, kDacMax = 3276;
    static constexpr int kDacRetries = 50;

    AnalogBoard() = default;
    ~AnalogBoard();
    AnalogBoard(const AnalogBoard&) = delete;
    AnalogBoard& operator=(const AnalogBoard&) = delete;

    void setEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); } // before the sources start
    void setLimit(bool on) noexcept { limit_.store(on, std::memory_order_relaxed); }     // write LMT ON at the first switch
    void setInsert(bool mainThrough) noexcept; // message thread: the select line (1 = main through the filter), on change only
    void setHpCutoff(int value1023) noexcept; // panel reader thread: the DAC code, on change only

    // The 12-bit DAC code of an afs.hpCutoff value 0–1023 (kDacMin…kDacMax; also for the tests).
    static unsigned dacCode(int value1023) noexcept;
    // The vendor driver's synchronous SPI write returns 0 on success, a generic driver the byte count: only −1 fails.
    static constexpr bool dacWriteOk(long result) noexcept { return result == 0 || result == 2; }
    // The select line's level.
    static constexpr unsigned char selectLevel(bool mainThrough) noexcept { return mainThrough ? 1 : 0; }

private:
    bool writeLine(int line, unsigned char level, const char* what) noexcept;
    std::atomic<bool> enabled_ {false}; // Imx6Platform::configure turns it on with the filter loop
    std::atomic<bool> limit_ {true};
    int gpioFd_ = -1, dacFd_ = -1;
    bool gpioFailed_ = false, dacFailed_ = false, limitDone_ = false;
    int lastActive_ = -1, lastCode_ = -1, dacErrors_ = 0;
};
} // namespace platform::imx6::panel
