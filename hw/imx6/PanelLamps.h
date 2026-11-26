// The panel's lamps (io::LampSink) and the TX schedule. Device-independent part:
//
//   PanelLamps  (io::LampSink, message thread) — setLamp() writes the logical lamp into a TxFrame, flush() publishes it
//   LampBus     (shared) — the latest frame, the send schedule (on change, at most every kMinGapMs; heartbeat every
//               kHeartbeatMs = 4 Hz while nothing changes) and the local echo below; the panel link's reader thread
//               takes the frame to send (PanelLink.cpp)
//   local echo  — until the app first flushes the LampSink, the panel lights itself from its own input so the lamps
//               work with no app code: a pad hit flashes white and fades over 1 s, a held key lights white (red/green
//               for REC/PLAY), the strip LEDs follow the strip (PITCH mode), the PITCH mode indicator is on. The first
//               flush() ends the echo.
#pragma once

#include "Platform.h"
#include "controls/LampRules.h"
#include "imx6/AnalogBoard.h"
#include "imx6/PanelCodec.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace platform::imx6::panel
{
class LampBus
{
public:
    static constexpr double kHeartbeatMs = 250.0; // ≤ 4 Hz resend while nothing changes
    static constexpr double kMinGapMs = 10.0;     // changes are coalesced to ≤ 100 frames/s
    static constexpr double kEchoTickMs = 33.0;   // echo fades are recomputed at ~30 Hz while one runs

    LampBus();
    // Message thread (PanelLamps::flush): the app's frame replaces the echo for good.
    void publish(const TxFrame&);
    // Reader threads: input seen by the panel or the strip, for the echo (ignored once the app publishes).
    void echo(const io::ControlEvent&);
    // Sender (panel reader thread): the frame to send now, if one is due at `nowMs`. `waitMs` = when to ask again.
    bool take(double nowMs, TxFrame& out, double& waitMs);
    bool external() const noexcept { return external_.load(std::memory_order_acquire); }
    // Called after publish()/echo() so the sender wakes up (set by the sender: write to its wake pipe).
    void setWake(void (*wake)(void*), void* arg) noexcept;

private:
    void renderEcho(double nowMs); // under mutex_

    mutable std::mutex mutex_;
    TxFrame frame_;
    bool changed_ = true;
    double lastSentMs_ = -1e12;
    std::atomic<bool> external_ {false};
    // echo state
    io::LampFrame echo_ {};
    double echoAnimUntilMs_ = -1e12, lastEchoRenderMs_ = 0;
    bool echoDirty_ = false;
    void (*wake_)(void*) = nullptr;
    void* wakeArg_ = nullptr;
};

class PanelLamps final : public io::LampSink
{
public:
    // `board` (may be null): the analog filter's select line follows setFilterInsert() (AnalogBoard.h).
    explicit PanelLamps(std::shared_ptr<LampBus> bus, std::shared_ptr<AnalogBoard> board = {})
        : bus_(std::move(bus)), board_(std::move(board))
    {
    }
    void setLamp(io::Control c, std::uint32_t rgb, std::uint8_t level) override
    {
        frame_.setLamp(c, rgb, level);
    }
    void setFilterInsert(bool on) override { insert_ = on; }
    void flush() override;

private:
    std::shared_ptr<LampBus> bus_;
    std::shared_ptr<AnalogBoard> board_;
    bool insert_ = false;
    TxFrame frame_;
};
} // namespace platform::imx6::panel
