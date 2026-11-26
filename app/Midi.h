// MIDI in and out through ALSA raw MIDI (hw/common/MidiPort.h): the unit's DIN MIDI and USB MIDI ports.
//
//   in    every input port is opened, each on its own reader thread; note on/off go straight to the player (they play
//         the selected instrument), every message also to the UI's monitor (a lock-free queue drained by poll())
//   out   the player's clock and notes arrive timestamped from the audio thread (MidiScheduler::schedule); a sender
//         thread writes each at its time to the output ports `outputs` selects ("din", "usb", "all": by port name);
//         send() writes at once (UI thread)
//   hot-plug  refresh() (UI thread, once a second) opens new ports and drops vanished ones — on the unit the USB port
//         disappears while the unit is in USB storage mode
#pragma once

#include "app/Player.h"
#include "app/Queue.h"

#include "common/MidiPort.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace app
{
struct MidiMessageIn
{
    std::uint8_t bytes[3];
    std::uint8_t size;
    std::uint8_t port; // index into ports()
};

class Midi final : public MidiScheduler
{
public:
    explicit Midi(Player& player, std::string outputs = "all");
    ~Midi() override;

    void refresh(std::string* log = nullptr);           // UI thread: hot-plug pass; `log` gets "MIDI …" lines
    void poll(std::vector<MidiMessageIn>& out);         // UI thread: messages since the last call
    void send(const std::uint8_t* bytes, int size);     // UI thread: to every selected output, now
    bool schedule(std::uint64_t dueNs, const std::uint8_t* bytes, int size) noexcept override; // audio thread
    int sentCount() const noexcept { return static_cast<int>(sent_.load(std::memory_order_relaxed)); }
    void stop();

    struct PortState
    {
        std::string name;
        bool input = false, output = false;
    };
    const std::vector<PortState>& ports() const noexcept { return ports_; }
    int inputCount() const noexcept;
    int outputCount() const noexcept;

private:
    struct Input
    {
        std::string key;
        std::uint8_t index = 0;
        std::unique_ptr<io::midi::MidiInput> port;
        std::thread thread;
        std::atomic<bool> gone {false};
    };
    struct Output
    {
        std::string key;
        std::unique_ptr<io::midi::MidiOutput> port;
    };
    struct Timed
    {
        std::uint64_t dueNs;
        std::uint8_t bytes[3];
        std::uint8_t size;
    };
    void read(Input& in);
    void sender();
    bool selected(const std::string& name) const;
    void write(const std::uint8_t* bytes, int size); // with outMutex_ held
    std::uint8_t indexOf(const std::string& name, bool input, bool output);

    Player& player_;
    std::string selection_;
    std::vector<std::unique_ptr<io::midi::PortProvider>> providers_;
    std::vector<std::unique_ptr<Input>> inputs_;
    std::vector<Output> outputs_;
    std::vector<PortState> ports_;
    MpscQueue<MidiMessageIn, 1024> toUi_; // reader threads → UI
    std::atomic<bool> quit_ {false};
    std::mutex outMutex_; // outputs_: UI (refresh, send) and the sender thread
    MpscQueue<Timed, 2048> timed_;
    std::thread senderThread_;
    std::atomic<std::uint64_t> sent_ {0};
};
} // namespace app
