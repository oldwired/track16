#include "app/Midi.h"

#include "app/Log.h"

#include <pthread.h>
#include <sched.h>
#include <time.h>

#include <algorithm>
#include <cctype>

namespace app
{
namespace
{
std::uint64_t nowNs() noexcept
{
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

std::string upper(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

Midi::Midi(Player& player, std::string outputs) : player_(player), selection_(upper(std::move(outputs)))
{
    providers_.push_back(io::midi::makeAlsaRawMidi());
    senderThread_ = std::thread([this] { sender(); });
}

bool Midi::selected(const std::string& name) const
{
    if (selection_.empty() || selection_ == "ALL")
        return true;
    if (selection_ == "NONE")
        return false;
    return upper(name).find(selection_) != std::string::npos;
}

bool Midi::schedule(std::uint64_t dueNs, const std::uint8_t* bytes, int size) noexcept
{
    Timed t {dueNs, {0, 0, 0}, static_cast<std::uint8_t>(std::clamp(size, 1, 3))};
    for (int i = 0; i < t.size; ++i)
        t.bytes[i] = bytes[i];
    return timed_.push(t);
}

void Midi::write(const std::uint8_t* bytes, int size)
{
    // A failed write = the port is gone (unplugged): drop it, the next refresh() reopens it when it is back.
    std::erase_if(outputs_, [&](Output& o) { return ! o.port->write(bytes, size); });
    sent_.fetch_add(1, std::memory_order_relaxed);
}

// The sender: messages in due order (a stable order for equal times), each written when its time has come. Sleeps
// at most 1 ms, so new messages with near due times are never late by more than that.
void Midi::sender()
{
    nameThisThread("midi-out");
    sched_param p {};
    p.sched_priority = 60;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
    std::vector<std::pair<std::uint64_t, Timed>> pending; // (sequence-tagged due time, message)
    pending.reserve(4096);
    std::uint64_t sequence = 0;
    while (! quit_.load(std::memory_order_relaxed))
    {
        for (Timed t; timed_.pop(t);)
            pending.push_back({sequence++, t});
        std::stable_sort(pending.begin(), pending.end(), [](const auto& a, const auto& b) { return a.second.dueNs < b.second.dueNs; });
        const std::uint64_t now = nowNs();
        std::size_t done = 0;
        if (! pending.empty() && pending.front().second.dueNs <= now)
        {
            std::lock_guard<std::mutex> lock(outMutex_);
            while (done < pending.size() && pending[done].second.dueNs <= now)
            {
                write(pending[done].second.bytes, pending[done].second.size);
                ++done;
            }
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(done));
        std::uint64_t wake = now + 1000000;
        if (! pending.empty())
            wake = std::min(wake, std::max(now, pending.front().second.dueNs));
        timespec ts {static_cast<time_t>(wake / 1000000000ull), static_cast<long>(wake % 1000000000ull)};
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
    }
}

Midi::~Midi() { stop(); }

void Midi::stop()
{
    quit_ = true;
    if (senderThread_.joinable())
        senderThread_.join();
    for (auto& in : inputs_)
        if (in->thread.joinable())
            in->thread.join();
    inputs_.clear();
    outputs_.clear();
}

std::uint8_t Midi::indexOf(const std::string& name, bool input, bool output)
{
    for (std::size_t i = 0; i < ports_.size(); ++i)
        if (ports_[i].name == name)
        {
            ports_[i].input = ports_[i].input || input;
            ports_[i].output = ports_[i].output || output;
            return static_cast<std::uint8_t>(i);
        }
    ports_.push_back({name, input, output});
    return static_cast<std::uint8_t>(ports_.size() - 1);
}

int Midi::inputCount() const noexcept { return static_cast<int>(inputs_.size()); }
int Midi::outputCount() const noexcept { return static_cast<int>(outputs_.size()); } // UI thread: a snapshot

void Midi::refresh(std::string* log)
{
    auto say = [&](const std::string& line) {
        if (log != nullptr)
            *log += line + "\n";
    };
    // Drop inputs whose reader saw the port vanish.
    for (auto it = inputs_.begin(); it != inputs_.end();)
    {
        if ((*it)->gone.load())
        {
            if ((*it)->thread.joinable())
                (*it)->thread.join();
            say("MIDI in closed: " + (*it)->key);
            ports_[(*it)->index].input = false;
            it = inputs_.erase(it);
        }
        else
            ++it;
    }
    for (auto& provider : providers_)
    {
        const auto list = provider->list();
        for (const auto& info : list)
        {
            const std::string key = std::string(provider->id()) + ":" + info.name;
            if (info.input && std::none_of(inputs_.begin(), inputs_.end(), [&](const auto& in) { return in->key == key; }))
            {
                std::string error;
                if (auto port = provider->openInput(info, error))
                {
                    auto in = std::make_unique<Input>();
                    in->key = key;
                    in->index = indexOf(info.name, true, false);
                    in->port = std::move(port);
                    Input* raw = in.get();
                    in->thread = std::thread([this, raw] { read(*raw); });
                    inputs_.push_back(std::move(in));
                    say("MIDI in opened: " + key + " (" + info.id + ")");
                }
                else
                    say("MIDI in FAILED: " + key + ": " + error);
            }
            bool known = false;
            {
                std::lock_guard<std::mutex> lock(outMutex_);
                known = std::any_of(outputs_.begin(), outputs_.end(), [&](const auto& o) { return o.key == key; });
            }
            if (info.output && selected(info.name) && ! known)
            {
                std::string error;
                if (auto port = provider->openOutput(info, error))
                {
                    indexOf(info.name, false, true);
                    std::lock_guard<std::mutex> lock(outMutex_);
                    outputs_.push_back({key, std::move(port)});
                    say("MIDI out opened: " + key + " (" + info.id + ")");
                }
                else
                    say("MIDI out FAILED: " + key + ": " + error);
            }
        }
    }
}

// Reader thread: a running-status parser for channel messages; system exclusive is skipped, real-time bytes
// (clock, active sensing) are ignored.
void Midi::read(Input& in)
{
    nameThisThread("midi-in");
    std::uint8_t buffer[256];
    std::uint8_t status = 0, data[2] = {}, have = 0;
    bool sysex = false;
    while (! quit_.load(std::memory_order_relaxed))
    {
        const int n = in.port->read(buffer, sizeof buffer, 100);
        if (n < 0)
        {
            in.gone = true;
            return;
        }
        for (int i = 0; i < n; ++i)
        {
            const std::uint8_t b = buffer[i];
            if (b >= 0xF8)
                continue; // real-time
            if (b & 0x80)
            {
                sysex = b == 0xF0;
                status = b < 0xF0 ? b : 0;
                have = 0;
                continue;
            }
            if (sysex || status == 0)
                continue;
            data[have++] = b;
            const std::uint8_t type = status & 0xF0;
            const std::uint8_t need = (type == 0xC0 || type == 0xD0) ? 1 : 2;
            if (have < need)
                continue;
            have = 0;
            switch (type)
            {
            case 0x90: player_.midiNote(data[0], data[1]); break;
            case 0x80: player_.midiNote(data[0], 0); break;
            default: break;
            }
            toUi_.push({{status, data[0], data[1]}, need == 1 ? std::uint8_t {2} : std::uint8_t {3}, in.index});
        }
    }
}

void Midi::poll(std::vector<MidiMessageIn>& out)
{
    for (MidiMessageIn m; toUi_.pop(m);)
        out.push_back(m);
}

void Midi::send(const std::uint8_t* bytes, int size)
{
    std::lock_guard<std::mutex> lock(outMutex_);
    write(bytes, size);
}
} // namespace app
