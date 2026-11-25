// The app: start the hardware, then one loop per display frame (60 Hz).
//
//   1. the platform (the unit, or a Linux desktop) and the settings
//   2. JUCE (no windows: see app/Theme.h), the synth on the codec, the panel / strip / touch, the lamps, MIDI, the screen
//   3. every frame: control events → screen, MIDI → screen, screen.frame(), the lamps, GlDisplay::draw()
//   4. quit on SHIFT + BACK, ↩ on the screen, the desktop window's close button or SIGTERM (the boot menu stops apps
//      that way); exit code 0 brings the boot menu back
//
// Settings: key=value lines in the platform's settings file (the unit: <stick>/<name>-data/<name>/<name>.conf; a
// desktop: ~/.config/<name>/<name>.conf), overridden by <PREFIX>_<KEY> environment variables (the boot menu exports the
// lines of <stick>/<name>.conf that way). The app reads: audio=0|1, midi=0|1, tone=0|1, tone_port=main|out34|…,
// seq=0|1, stat=<seconds>; the platform reads its own (hw/imx6/Imx6Platform.cpp, hw/linux/LinuxPlatform.cpp).
#include "app/GlDisplay.h"
#include "app/Log.h"
#include "app/Midi.h"
#include "app/Screen.h"
#include "app/Synth.h"
#include "app/Theme.h"

#include "Platform.h"
#include "ProjectIdentity.h"

#include <libopenmpt/libopenmpt.hpp>

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
    platform::Settings s {{"audio", "1"}, {"midi", "1"}, {"tone", "0"}, {"tone_port", "main"}, {"seq", "0"}, {"stat", "10"}};
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

    // 2. JUCE and the parts.
    juce::ScopedJuceInitialiser_GUI juce; // this thread is JUCE's message thread
    theme::installFonts();
    struct FontsRelease { ~FontsRelease() { theme::releaseFonts(); } } fontsRelease; // after the screen is gone

    Synth synth;
    synth.toneOn = settings["tone"] == "1";
    synth.seqRunning = settings["seq"] == "1";
    static const char* const jacks[] = {"main", "out34", "out56", "out78", "phones"};
    for (int i = 0; i < 5; ++i)
        if (settings["tone_port"] == jacks[i])
            synth.tonePort = i;

    std::vector<std::unique_ptr<platform::AudioDevice>> audioDevices;
    platform::AudioDevice* audio = nullptr;
    if (settings["audio"] == "1")
    {
        audioDevices = machine->audioDevices();
        for (auto& device : audioDevices)
        {
            std::string info;
            if (device->start(platform::AudioConfig {}, synth, info))
            {
                logLine("AUDIO ok %s", info.c_str());
                audio = device.get();
                break;
            }
            logLine("AUDIO %s unavailable: %s", std::string(device->id()).c_str(), info.c_str());
        }
    }

    auto inputs = machine->controlSources({&synth}); // pad hits and strip moves go straight to the synth
    for (auto& input : inputs)
    {
        std::string info;
        const bool ok = input->start(info);
        logLine("INPUT %s %s %s", std::string(input->id()).c_str(), ok ? "ok" : "FAILED", info.c_str());
    }
    auto lamps = machine->lamps(); // null on a desktop

    Midi midi(synth);
    if (settings["midi"] == "1")
    {
        std::string log;
        midi.refresh(&log);
        logMidi(log);
    }

    Screen screen(synth, midi);
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

    // 3. The frame loop, paced by the display (eglSwapBuffers waits for vsync).
    std::vector<io::ControlEvent> events;
    std::vector<MidiMessageIn> midiIn;
    io::LampFrame lampFrame;
    float scope[512];
    const double statMs = 1000.0 * std::atof(settings["stat"].c_str());
    double lastStat = start, lastMidiScan = start, fpsFrom = start;
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
        synth.scope(scope, 512);
        if (! gl.draw(screen.scene(now), scope, 512))
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
            logLine("STAT fps=%.0f dsp=%.1f%% dsp_max=%.1f%% xruns=%llu voices=%d midi_in=%d midi_out=%d", static_cast<double>(fps),
                    static_cast<double>(synth.loadAverage() * 100), static_cast<double>(synth.takeLoadMax() * 100),
                    static_cast<unsigned long long>(st.xruns), synth.activeVoices(), midi.inputCount(), midi.outputCount());
            lastStat = now;
        }
    }

    // 4. Shut down: lamps off, then audio, MIDI, inputs.
    logLine("STOP after %.1f s, %ld frames", (nowMs() - start) / 1000.0, frames);
    if (lamps)
        io::apply(io::LampFrame {}, nowMs(), io::LampBrightness::Normal, *lamps);
    for (auto& device : audioDevices)
        device->stop();
    midi.stop();
    for (auto& input : inputs)
        input->stop();
    return 0;
}
} // namespace
} // namespace app

int main() { return app::run(); }
