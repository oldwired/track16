// Track16: start the hardware, then one loop per display frame (60 Hz).
//
//   1. the platform and the settings; all CPU cores (the boot menu starts apps pinned to core 0)
//   2. JUCE (no windows: see app/Theme.h), the player on the codec, the panel / strip / touch, the lamps, MIDI, the
//      screen; then the module library on the stick
//   3. every frame: control events → screen, MIDI → screen, screen.frame(), the lamps, GlDisplay::draw()
//   4. quit on SHIFT + BACK or SIGTERM (the boot menu stops apps that way); exit code 0 brings the boot menu back
//
// Settings: key=value lines in <stick>/track16.conf (the boot menu exports them as TRACK16_<KEY>) or in the data
// folder's track16.conf. Track16 reads:
//   audio=0|1  midi=0|1  stat=<seconds>
//   modules=<folder>            where to look for modules (default: the stick)
//   autoplay=0|1                play the first module at start
//   stereo=<0…200>  interp=1|2|4|8   libopenmpt's stereo separation (%) and interpolation taps
//   rows_per_beat=<n>           for the MIDI clock and the beat quantize (default 4)
//   midi_out=all|din|usb|none   midi_clock=0|1  midi_notes=0|1  midi_offset_ms=<ms>
//   filter_latency=<frames>     the analog filter loop's latency (default: measured at start)
// and passes the rest to the platform (audio_period, audio_periods, filter_loop, …: hw/imx6/Imx6Platform.cpp).
#include "app/GlDisplay.h"
#include "app/Log.h"
#include "app/Midi.h"
#include "app/Player.h"
#include "app/Screen.h"
#include "app/Theme.h"

#include "Platform.h"
#include "ProjectIdentity.h"

#include <libopenmpt/libopenmpt.hpp>

#include <sched.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>

extern char** environ;

namespace app
{
namespace
{
std::atomic<bool> stopSignal {false};

std::string trim(std::string s)
{
    const auto b = s.find_first_not_of(" \t\r"), e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// Defaults, then the file's key=value lines, then <PREFIX>_<KEY> variables (keys are lower case).
platform::Settings loadSettings(const std::string& file)
{
    platform::Settings s {{"audio", "1"}, {"midi", "1"}, {"stat", "10"}, {"filter_loop", "1"}, {"audio_period", "128"},
                          {"audio_periods", "3"}, {"stereo", "100"}, {"interp", "4"}, {"rows_per_beat", "4"},
                          {"midi_out", "all"}, {"midi_clock", "1"}, {"midi_notes", "0"}, {"midi_offset_ms", "0"},
                          {"autoplay", "0"}, {"modules", ""}, {"filter_latency", "0"}};
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);)
        if (const auto eq = line.find('='); eq != std::string::npos && trim(line).rfind('#', 0) != 0)
        {
            auto key = trim(line.substr(0, eq));
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            s[key] = trim(line.substr(eq + 1));
        }
    const std::string prefix = std::string(identity::envPrefix) + "_";
    for (char** e = environ; *e != nullptr; ++e)
    {
        const std::string var = *e;
        const auto eq = var.find('=');
        if (var.rfind(prefix, 0) != 0 || eq == std::string::npos)
            continue;
        auto key = var.substr(prefix.size(), eq - prefix.size());
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        s[key] = var.substr(eq + 1);
    }
    return s;
}

void logMidi(const std::string& lines)
{
    for (std::size_t a = 0, b; a < lines.size(); a = b + 1)
        logLine("%s", lines.substr(a, (b = lines.find('\n', a)) - a).c_str());
}

int number(const platform::Settings& s, const char* key, int fallback)
{
    const auto it = s.find(key);
    return it == s.end() || it->second.empty() ? fallback : std::atoi(it->second.c_str());
}

// Every core for this process (the boot menu's runner inherits init's mask 0x1); the audio thread then pins itself
// to the last core (app/Player.cpp).
void useAllCores()
{
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    cpu_set_t set;
    CPU_ZERO(&set);
    for (long i = 0; i < cpus && i < CPU_SETSIZE; ++i)
        CPU_SET(static_cast<int>(i), &set);
    const bool ok = sched_setaffinity(0, sizeof set, &set) == 0;
    logLine("CPU %ld cores, affinity %s", cpus, ok ? "all" : "unchanged");
}

int run()
{
    installCrashLog();
    nameThisThread("app-main");
    signal(SIGTERM, [](int) { stopSignal = true; });
    signal(SIGINT, [](int) { stopSignal = true; });
    signal(SIGPIPE, SIG_IGN);
    const double start = nowMs();

    // 1. The platform and the settings.
    auto machine = platform::createPlatform();
    auto settings = loadSettings(machine->paths().settingsFile);
    machine->configure(settings);
    logLine("START %s %s (%s) pid=%d", identity::displayName, identity::version, identity::id, static_cast<int>(getpid()));
    logLine("LIBOPENMPT %s", openmpt::string::get("library_version").c_str());
    logLine("PLATFORM %s", machine->describe().c_str());
    logLine("SETTINGS file=%s", machine->paths().settingsFile.c_str());
    useAllCores();

    // 2. JUCE and the parts.
    juce::ScopedJuceInitialiser_GUI juce; // this thread is JUCE's message thread
    theme::installFonts();
    struct FontsRelease { ~FontsRelease() { theme::releaseFonts(); } } fontsRelease; // after the screen is gone

    Player player;
    player.stereoSeparation = number(settings, "stereo", 100);
    player.interpolation = number(settings, "interp", 4);
    player.rowsPerBeat = number(settings, "rows_per_beat", 4);
    player.midiClock = settings["midi_clock"] == "1";
    player.midiNotes = settings["midi_notes"] == "1";
    player.midiOffsetMs = number(settings, "midi_offset_ms", 0);

    // Before the audio starts (the audio thread reads them from its first period).
    Midi midi(player, settings["midi_out"]);
    player.setMidi(&midi);
    if (const int latency = number(settings, "filter_latency", 0); latency > 0)
        player.setFixedLatency(latency);

    std::vector<std::unique_ptr<platform::AudioDevice>> audioDevices;
    platform::AudioDevice* audio = nullptr;
    if (settings["audio"] == "1")
    {
        audioDevices = machine->audioDevices();
        for (auto& device : audioDevices)
        {
            std::string info;
            if (device->start(platform::AudioConfig {}, player, info))
            {
                logLine("AUDIO ok %s", info.c_str());
                audio = device.get();
                const auto st = device->stats();
                player.audioLatencyFrames = static_cast<int>(st.periodFrames * static_cast<unsigned>(number(settings, "audio_periods", 3)));
                break;
            }
            logLine("AUDIO %s unavailable: %s", std::string(device->id()).c_str(), info.c_str());
        }
    }

    auto inputs = machine->controlSources({&player}); // pad hits and strip moves go straight to the player
    for (auto& input : inputs)
    {
        std::string info;
        const bool ok = input->start(info);
        logLine("INPUT %s %s %s", std::string(input->id()).c_str(), ok ? "ok" : "FAILED", info.c_str());
    }
    auto lamps = machine->lamps(); // null on a desktop

    if (settings["midi"] == "1")
    {
        std::string log;
        midi.refresh(&log);
        logMidi(log);
    }

    ScreenConfig screenConfig;
    if (! settings["modules"].empty())
        screenConfig.roots.push_back(settings["modules"]);
    if (char cwd[512]; getcwd(cwd, sizeof cwd) != nullptr) // the boot menu starts apps in the stick's root
        screenConfig.roots.push_back(cwd);
    for (const auto& m : machine->paths().usbMounts)
        screenConfig.roots.push_back(m);
    screenConfig.roots.push_back("/media/usb1/sda1");
    screenConfig.autoplay = settings["autoplay"] == "1";
    screenConfig.stereoSeparation = player.stereoSeparation.load();
    screenConfig.interpolation = player.interpolation.load();
    Screen screen(player, midi, screenConfig);
    screen.onQuit = [] { platform::requestQuit(); };
    std::string error;
    auto display = machine->display(error); // the platform's display handles
    GlDisplay gl;                           // declared after the screen: destroyed before it
    const char* flavour = machine->id() == "imx6" ? "fb" : "x11";
    if (! gl.open(display->rendererEnv(), flavour, screen, error))
    {
        logLine("FATAL no display (%s): %s", flavour, error.c_str());
        return 2;
    }
    logLine("DISPLAY %s", gl.describe().c_str());
    screen.setStatus({std::string(machine->id()), std::string("gl-") + flavour,
                      audio != nullptr ? std::string(audio->id()) : std::string("no audio")});
    screen.start();

    // 3. The frame loop, paced by the display (eglSwapBuffers waits for vsync).
    std::vector<io::ControlEvent> events;
    std::vector<MidiMessageIn> midiIn;
    io::LampFrame lampFrame;
    const double statMs = 1000.0 * std::atof(settings["stat"].c_str());
    double lastStat = start, lastMidiScan = start, fpsFrom = start, lastRemeasure = start;
    std::uint64_t lastLoopDrops = ~0ull;
    long frames = 0, fpsFrames = 0;
    float fps = 0;
    while (! stopSignal && ! platform::quitRequested())
    {
        const double now = nowMs();
        events.clear();
        for (auto& input : inputs)
            input->poll(events);
        for (const auto& e : events)
            screen.handle(e, now);
        if (settings["midi"] == "1")
        {
            if (now - lastMidiScan >= 1000.0) // MIDI hot-plug
            {
                std::string log;
                midi.refresh(&log);
                logMidi(log);
                lastMidiScan = now;
            }
            midiIn.clear();
            midi.poll(midiIn);
            for (const auto& m : midiIn)
                screen.midiIn(m, now);
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil(0); // juce::Timer, AsyncUpdater, callAsync

        screen.frame(now, fps);
        if (lamps)
        {
            screen.lamps(lampFrame, now);
            io::apply(lampFrame, now, io::LampBrightness::Normal, *lamps); // sent on change (+ a 4 Hz heartbeat)
        }
        // The filter loop's latency: measured once the lamps have shown ACTIVE for a few frames (the lamp drives the
        // filter board's select line), and again after the loop dropped periods (at most every 10 s, when no route is on).
        if (frames == 3 || (! lamps && frames == 0))
            player.armMeasure();
        if (audio != nullptr && player.measurements() > 0)
        {
            const auto drops = audio->stats().loopDrops;
            if (drops != lastLoopDrops && now - lastRemeasure >= 10000.0)
            {
                if (lastLoopDrops != ~0ull)
                {
                    logLine("FILTER LOOP %llu dropped periods: measuring again", static_cast<unsigned long long>(drops - lastLoopDrops));
                    player.remeasure();
                }
                lastLoopDrops = drops;
                lastRemeasure = now;
            }
        }
        if (! gl.draw(screen.scene(now)))
        {
            logLine("FATAL the display surface is gone");
            break;
        }

        // Never faster than 60 Hz: the swap waits for vsync on the unit, but not on every desktop (Xvfb, some drivers).
        if (const double left = 1000.0 / 60.0 - (nowMs() - now); left > 1.0)
            usleep(static_cast<useconds_t>(left * 1000.0));

        if (++frames == 1 || frames == 60)
            logLine("FRAME %ld ok (%.0f ms after start)", frames, nowMs() - start);
        ++fpsFrames;
        if (nowMs() - fpsFrom >= 1000.0)
        {
            fps = static_cast<float>(1000.0 * fpsFrames / (nowMs() - fpsFrom));
            fpsFrom = nowMs();
            fpsFrames = 0;
        }
        if (statMs > 0 && now - lastStat >= statMs)
        {
            const auto st = audio != nullptr ? audio->stats() : platform::AudioStats {};
            const auto pos = player.position();
            logLine("STAT fps=%.0f dsp=%.1f%% dsp_max=%.1f%% xruns=%llu playing=%d order=%d row=%d route=%d loop_latency=%d%s midi_in=%d midi_out=%d midi_sent=%d",
                    static_cast<double>(fps), static_cast<double>(player.loadAverage() * 100), static_cast<double>(player.takeLoadMax() * 100),
                    static_cast<unsigned long long>(st.xruns), player.playing() ? 1 : 0, pos.order, pos.row, static_cast<int>(player.route()),
                    player.latencyFrames(), player.stems() ? (player.latencyMeasured() ? " (measured)" : " (fallback)") : " (no loop)",
                    midi.inputCount(), midi.outputCount(), midi.sentCount());
            lastStat = now;
        }
    }

    // 4. Shut down: lamps off, then audio, MIDI, inputs.
    logLine("STOP after %.1f s, %ld frames", (nowMs() - start) / 1000.0, frames);
    if (lamps)
        io::apply(io::LampFrame {}, nowMs(), io::LampBrightness::Normal, *lamps);
    // MIDI: the player sends Stop and its note-offs, the sender writes them; then all notes off on every channel
    // (whatever else may hang on the followers), before the ports close.
    player.stop();
    usleep(100 * 1000);
    for (auto& device : audioDevices)
        device->stop();
    for (int c = 0; c < 16; ++c)
    {
        const std::uint8_t allOff[3] = {static_cast<std::uint8_t>(0xB0 | c), 123, 0};
        midi.send(allOff, 3);
    }
    midi.stop();
    for (auto& input : inputs)
        input->stop();
    return 0;
}
} // namespace
} // namespace app

int main() { return app::run(); }
