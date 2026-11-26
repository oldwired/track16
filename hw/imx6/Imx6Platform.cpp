// The SP-16 unit as a Platform: NXP i.MX6Q (Cortex-A9 ×4, NEON), AK4458 codec over ALSA, the subucom SPI panel, the
// capacitive touch strip, the Vivante GC2000 GPU via common/GlDispatch.h, an X server without a window manager.
// Named after the SoC, not after the product.
#include "Platform.h"

#include "common/AlsaOutput.h"
#include "imx6/Imx6Display.h"
#include "imx6/AnalogBoard.h"
#include "imx6/PanelLamps.h"
#include "imx6/PanelLink.h"
#include "imx6/StripSource.h"

#include "ProjectIdentity.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>

namespace platform::imx6
{
namespace
{
constexpr const char* panelDevice = "/dev/subucom_spi0.1";

// The codec's logical outputs: 16 S16 channels in 8 two-channel DAC slots where only the even channel carries audio,
// MAIN = 12/14 (observed), OUT3-4 = 8/10 and OUT5-6 = 4/6 (inferred, not verified), OUT7-8 = 0/2; the odd channels
// are padding written as 0. PHONES is the separate PCM hw:ak4452audio,0 channels 0/1 (AlsaOutputConfig::auxDevice);
// the analog filter's send channel is unknown. The map is data: the setting audio_map (e.g. "out34:8/10,out56:4/6")
// corrects it without a rebuild.
constexpr common::AlsaPortMap kCodecMap {{{AudioPort::Main, 12, 14},
                                         {AudioPort::Out34, 8, 10},
                                         {AudioPort::Out56, 4, 6},
                                         {AudioPort::Out78, 0, 2},
                                         {AudioPort::Phones, -1, -1},
                                         {AudioPort::FilterSend, -1, -1}}};

class Imx6Platform final : public Platform
{
public:
    std::string_view id() const noexcept override { return "imx6"; }

    // `/dev/subucom_spi0.1` or the codec exists. On an emulated unit the device nodes may be served by an LD_PRELOAD
    // shim and not exist on disk, so the board's backlight node (harmless to open) is also probed by opening it.
    bool detect() const override { return !detectReason().empty(); }

    std::string describe() const override
    {
        const auto why = detectReason();
        return "i.MX6 board (" + (why.empty() ? std::string("not detected") : why) + "), audio " + audioDevice_
             + " (MAIN = ch 12/14 of 16; OUT3-4 8/10, OUT5-6 4/6, OUT7-8 0/2" + (mapNote_.empty() ? "" : "; " + mapNote_)
             + "), phones " + (phonesDevice_.empty() ? std::string("off") : phonesDevice_) + ", filter loop "
             + (loopDevice_.empty() ? std::string("off") : loopDevice_) + ", panel " + panelDevice
             + " (+ lamps, LCD touch), input trace " + (inputTrace_ ? "on" : "off")
             + ", strip /dev/cdc_spi2.0"
             + ", display X11 + Vivante GC2000 (gldispatch)";
    }

    Paths paths() const override
    {
        // Settings: key=value lines in /opt/sp16/dev-share/<name>.conf when that development share exists, otherwise
        // <data dir>/<name>.conf. The app reads the file on every start; <PREFIX>_<KEY> environment variables
        // override it.
        Paths p;
        p.dataDir = std::string("/root/data/") + identity::name; // on the unit's data volume
        p.cacheDir = p.dataDir + "/cache";
        const std::string overlay = std::string("/opt/sp16/dev-share/") + identity::name + ".conf";
        p.settingsFile = ::access(overlay.c_str(), F_OK) == 0 ? overlay : p.dataDir + "/" + identity::name + ".conf";
        return p;
    }

    void configure(const Settings& settings) override
    {
        if (auto it = settings.find("audio_device"); it != settings.end() && ! it->second.empty())
            audioDevice_ = it->second;
        if (auto it = settings.find("input_trace"); it != settings.end()) // INPUT log lines, default on
            inputTrace_ = it->second != "0";
        // phones_device=<PCM> (default hw:ak4452audio,0; "none" = no headphone stream) and
        // audio_map=<port>:<L>/<R>,… over the codec map (kCodecMap).
        if (auto it = settings.find("phones_device"); it != settings.end())
            phonesDevice_ = it->second == "none" ? std::string() : it->second;
        phonesS16_ = false; // phones_format=s16: a 16-bit stream (default S24_LE, as the stock firmware)
        if (auto it = settings.find("phones_format"); it != settings.end())
            phonesS16_ = it->second == "s16" || it->second == "S16_LE";
        if (auto it = settings.find("phones_gain"); it != settings.end()) // linear, default 0.5 (−6 dB)
            phonesGain_ = std::clamp(static_cast<float>(std::strtod(it->second.c_str(), nullptr)), 0.0f, 1.0f);
        // The analog filter's send/return loop: OPT-IN (filter_loop=1 = hw:ak4452audio,1 "Send & Return", or a PCM
        // name; default none). The first unit run with it on by default stopped right after the audio started, so it
        // stays off until proven on hardware. Its two controls (panel::AnalogBoard:
        // the filter-select GPIO line while ACTIVE is lit, the HP CUTOFF DAC word) follow the loop unless
        // filter_board=0/1 says otherwise.
        loopDevice_.clear();
        if (auto it = settings.find("filter_loop"); it != settings.end() && it->second != "none" && it->second != "0")
            loopDevice_ = it->second == "1" ? std::string("hw:ak4452audio,1") : it->second;
        bool board = ! loopDevice_.empty();
        if (auto it = settings.find("filter_board"); it != settings.end())
            board = it->second != "0";
        board_->setEnabled(board);
        if (auto it = settings.find("filter_limit"); it != settings.end()) // 0: leave the limiter line as booted
            board_->setLimit(it->second != "0");
        if (auto it = settings.find("main_gain"); it != settings.end()) // linear, default 0.5 (−6 dB)
            mainGain_ = std::clamp(static_cast<float>(std::strtod(it->second.c_str(), nullptr)), 0.1f, 1.0f);
        if (auto it = settings.find("filter_send_gain"); it != settings.end()) // linear, default 0.5 (−6 dB)
            loopGain_ = std::clamp(static_cast<float>(std::strtod(it->second.c_str(), nullptr)), 0.0f, 1.0f);
        map_ = kCodecMap;
        mapNote_.clear();
        if (auto it = settings.find("audio_map"); it != settings.end() && ! it->second.empty())
            mapNote_ = common::parsePortMap(it->second, map_) ? "audio_map " + it->second
                                                              : "audio_map " + it->second + " IGNORED (bad syntax)";
        // audio_period / audio_periods: the period size and count (<PREFIX>_AUDIO_PERIOD still wins in AlsaOutput).
        auto number = [&](const char* key) {
            auto it = settings.find(key);
            return it == settings.end() ? 0u : static_cast<unsigned>(std::strtoul(it->second.c_str(), nullptr, 10));
        };
        period_ = number("audio_period");
        periods_ = number("audio_periods");
    }

    std::vector<std::unique_ptr<AudioDevice>> audioDevices() override
    {
        std::vector<std::unique_ptr<AudioDevice>> devices;
        // Direct ALSA hw device: the codec's 16 channels (MAIN = 12/14, the other output pairs) + PHONES
        devices.push_back(std::make_unique<common::AlsaOutput>(
            common::AlsaOutputConfig {audioDevice_, 16, period_, periods_, map_, phonesDevice_, 2, AudioPort::Phones,
                                      phonesGain_, phonesS16_, loopDevice_, loopGain_, mainGain_}));
        return devices;
    }

    std::vector<std::unique_ptr<io::ControlSource>> controlSources(const ControlEnv& env) override
    {
        std::vector<std::unique_ptr<io::ControlSource>> sources;
        sources.push_back(std::make_unique<PanelLink>(env.triggers, lampBus_, inputTrace_, board_)); // + LCD touch
        sources.push_back(std::make_unique<StripSource>(lampBus_, inputTrace_, env.triggers)); // /dev/cdc_spi2.0
        return sources;
    }

    // The panel's TX lamp frames; the panel link sends them (on change + 4 Hz heartbeat, local echo until a flush).
    std::unique_ptr<io::LampSink> lamps() override
    {
        return std::make_unique<panel::PanelLamps>(lampBus_, board_);
    }

    std::unique_ptr<DisplayDevice> display(std::string& error) override { return makeDisplay(error); }

private:
    static std::string detectReason()
    {
        for (const char* path : {panelDevice, "/proc/asound/ak4458audio", "/dev/cdc_spi2.0"})
            if (::access(path, F_OK) == 0)
                return std::string(path) + " present";
        const int fd = ::open("/dev/backlight", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0)
        {
            ::close(fd);
            return "/dev/backlight opens";
        }
        return {};
    }

    std::string audioDevice_ = "hw:ak4458audio,0";
    std::string phonesDevice_ = "hw:ak4452audio,0"; // the headphone DAC
    float phonesGain_ = 0.5f;                       // a fixed −6 dB on the headphone DAC
    bool phonesS16_ = false;
    std::string loopDevice_;                        // the analog filter's send/return (opt-in: filter_loop=1)
    float loopGain_ = 0.5f;                         // a fixed −6 dB on the filter send (headroom for the drive)
    float mainGain_ = 0.5f;                         // a fixed −6 dB on the main DAC pairs (headroom, as the stock firmware)
    std::shared_ptr<panel::AnalogBoard> board_ = std::make_shared<panel::AnalogBoard>();
    common::AlsaPortMap map_ = kCodecMap;
    std::string mapNote_;
    bool inputTrace_ = true;
    unsigned period_ = 0, periods_ = 0; // 0 = AudioConfig's (256 × 3)
    std::shared_ptr<panel::LampBus> lampBus_ = std::make_shared<panel::LampBus>(); // panel link + lamps + strip echo
};

} // namespace
} // namespace platform::imx6

std::unique_ptr<platform::Platform> platform::createPlatform() { return std::make_unique<imx6::Imx6Platform>(); }
