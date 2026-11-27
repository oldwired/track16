// Track16's screen, in freesp16's design (app/Theme.h): a juce::Component tree on freesp16's 1280 × 768 lpx canvas,
// scaled to the 800 × 480 LCD, plus a GL scene (the pattern "digital rain") drawn under it.
//
//   status row    TRACK16 › PLAYER | LIBRARY · the last event · fps, DSP · MIDI IN OUT · FILTER · NOTES · PLAY
//   context bar   ↩ · ORDER | ROW · SPEED | BPM · title · the four pad modes (FILTER, MUTE, LOOP, PLAY/KIT)
//   body          PLAYER: the rain (GL, see buildRain), or the OUTS page in the OUTS pad mode · LIBRARY: the module
//                 list on the stick
//   knob strip    TEMPO, PITCH, STEREO, INTERP, GAIN, VIEW (knobs 1–6)
//
// Controls (handle()): see the mapping in the README. Main thread only, except the loader thread, which reads and
// parses modules (loadSong) so the screen and the pad releases never wait for a load.
#pragma once

#include "app/Library.h"
#include "app/Midi.h"
#include "app/Player.h"

#include "controls/LampRules.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace app
{
// The GL scene, in LCD pixels (800 × 480), premultiplied ARGB colours.
struct SceneRect
{
    float x, y, w, h;
    std::uint32_t argb;
};
// A character of the glyph atlas (printable ASCII in JetBrains Mono, kGlyphW × kGlyphH texels, drawn 1:1 at whole
// pixels): ch | kBold for the bold face.
constexpr int kGlyphW = 8, kGlyphH = 15;
constexpr unsigned char kBold = 0x80;
struct SceneGlyph
{
    float x, y, w, h;
    char ch;
    std::uint32_t argb;
};
struct SceneInput
{
    double nowMs = 0;
    juce::Colour background;
    const std::vector<SceneRect>* rects = nullptr;   // under the glyphs
    const std::vector<SceneGlyph>* glyphs = nullptr;
    const std::vector<SceneRect>* overlay = nullptr; // over the glyphs
};

struct ScreenConfig
{
    std::vector<std::string> roots; // where to look for modules
    bool autoplay = false;          // play the first module at start
    int stereoSeparation = 100, interpolation = 4;
    int outStems = 4;               // instances for the single outs (out_stems)
    std::string outsDefault;        // the single outs' layout for modules without their own (outs=, app/Outs.h)
    std::string dataDir;            // per-module layouts go to <dataDir>/outs/
    int rowsPerBeat = 0;            // rows per MIDI beat for modules without their own choice; 0: auto (beatAuto)
};

class Screen final : public juce::Component
{
public:
    static constexpr int kWidth = 800, kHeight = 480;

    Screen(Player& player, Midi& midi, ScreenConfig config);
    ~Screen() override;

    struct Status
    {
        std::string platform, renderer, audio;
    };
    void setStatus(const Status&);

    bool handle(const io::ControlEvent& e, double nowMs);
    void midiIn(const MidiMessageIn&, double nowMs);
    void frame(double nowMs, float fps);            // once per frame, before the renderer draws
    void lamps(io::LampFrame&, double nowMs) const; // the panel's lamps for this frame
    SceneInput scene(double nowMs) const;
    void start();                                   // after audio started: scan the stick, autoplay

    std::function<void()> onQuit; // SHIFT + BACK, or ↩ (long) on the screen
    std::function<void(const std::string&)> onScreenshot; // SHIFT + HOME: the next frame to this PNG file

private:
    struct Canvas;
    struct StatusRow;
    struct ContextBar;
    struct Browser;
    struct KnobStrip;
    struct OutsPage;
    enum class View { Player, Library };
    enum class Density { Auto, Full, Wide, Medium, Compact, Rain }; // the rain: 14, 10, 6, 3, 1 characters a column

    void setView(View);
    void loadEntry(int index, bool play); // asks the loader thread; frame() takes the result
    void loaderThread();
    void proberThread(std::vector<std::string> paths); // the library's format, channels and length, in the background
    void takeProbed();
    void takeLoaded();
    void setPadMode(PadMode mode, bool samePressed);
    int pageCount() const;
    void padPress(int pad0, int velocity, bool fromReader, double nowMs);
    void midiControl(int cc, int value, double nowMs); // a controller on the MIDI control channel
    void padRelease(int pad0, double nowMs);
    void event(const juce::String& text);
    // the single outs: the layout of the module that plays, its file, editing it
    void loadLayout();
    void saveLayout();
    void changeLayout(const juce::String& what, double nowMs);
    void setChannelDest(int channel, std::uint8_t dest);
    void outsStep(int step0, double nowMs); // a STEP key in the OUTS pad mode
    void outsTouch(juce::Point<float> at, double nowMs);
    bool outsVisible() const;
    // rows per MIDI beat: auto from the module's first row, or the one chosen with TEMPO (saved per module in
    // <dataDir>/rows_per_beat.txt)
    int beatAuto(const SongData&) const;
    void applyBeat(const SongData&);
    void stepBeat(int dir, bool backToAuto);
    bool beatKnob() const; // TEMPO sets the rows per beat: following a clock, or one comes in
    void loadBeats();
    void saveBeats();
    void buildRain(double nowMs);
    void screenshot(); // SHIFT + HOME: <dataDir>/screenshots/track16-NNN.png

    Player& player_;
    Midi& midi_;
    ScreenConfig config_;
    Library library_;
    std::unique_ptr<Canvas> canvas_;
    StatusRow* status_ = nullptr;
    ContextBar* context_ = nullptr;
    Browser* browser_ = nullptr;
    KnobStrip* knobs_ = nullptr;
    OutsPage* outsPage_ = nullptr;

    View view_ = View::Library;
    Density density_ = Density::Auto;
    std::shared_ptr<const SongData> song_;
    std::unique_ptr<Song> pending_; // waiting for the player to take it
    int playingIndex_ = -1;
    // the loader thread
    struct LoadJob
    {
        int index = -1;
        std::string path;
        bool play = false, stems = false;
        int sampleRate = 44100, outStems = 0;
    };
    struct Loaded
    {
        LoadJob job;
        std::unique_ptr<Song> song;
        std::string error;
        double ms = 0;
    };
    std::thread loader_;
    std::mutex loadMutex_;
    std::condition_variable loadWake_;
    bool loadQuit_ = false, loadWanted_ = false;
    LoadJob loadJob_;
    std::unique_ptr<Loaded> loaded_;
    int lastMeasurements_ = 0;
    // the prober thread
    std::thread prober_;
    std::atomic<bool> probeQuit_ {false};
    std::mutex probeMutex_;
    std::vector<std::pair<int, ModuleInfo>> probed_; // done since the last frame
    std::array<std::uint16_t, 16> padHits_ {}; // presses per pad, as the player counts them (Player::push)
    bool shift_ = false;
    bool driveSeen_ = false, stripHold_ = false;
    int stripMode_ = 0; // Player::StripPitch
    int orderPage_ = 0, touchRow_ = -1;
    std::uint64_t mutes_ = 0, solos_ = 0;
    std::uint64_t padsHeld_ = 0;
    std::uint32_t stepsHeld_ = 0; // STEP keys down (bit 0 = STEP 1)
    outs::Layout layout_, outsShown_;
    std::string layoutFile_;
    bool layoutDirty_ = false;
    std::map<std::string, int> beats_; // module file name → rows per beat chosen with TEMPO
    bool beatsDirty_ = false;
    double beatsSaveMs_ = 0;
    double layoutSaveMs_ = 0, outsMeterMs_ = 0;
    double lastFrameMs_ = 0, backDownMs_ = -1;
    static constexpr double kEventMs = 4000; // how long a message stays in the status row
    double eventUntilMs_ = 0;
    juce::String statusBase_;
    std::array<double, 16> padHitMs_ {};
    std::array<float, kMaxChannels> peakHold_ {}; // the rain's level peaks, and when each was reached
    std::array<double, kMaxChannels> peakHeldMs_ {};
    double lastRainMs_ = 0;
    // the scene
    std::vector<SceneRect> rects_, overlay_;
    std::vector<SceneGlyph> glyphs_;
};
} // namespace app
