// Track16's screen, in freesp16's design (app/Theme.h): a juce::Component tree on freesp16's 1280 × 768 lpx canvas,
// scaled to the 800 × 480 LCD, plus a GL scene (the pattern "digital rain") drawn under it.
//
//   status row    TRACK16 › PLAYER | LIBRARY · the last event · fps, DSP · MIDI IN OUT · FILTER · NOTES · PLAY
//   context bar   ↩ · ORDER | ROW · SPEED | BPM · title · the four pad modes (FILTER, MUTE, LOOP, PLAY/KIT)
//   body          PLAYER: the rain (GL, see buildRain) · LIBRARY: the module list on the stick
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
#include <condition_variable>
#include <cstdint>
#include <functional>
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

private:
    struct Canvas;
    struct StatusRow;
    struct ContextBar;
    struct Browser;
    struct KnobStrip;
    enum class View { Player, Library };
    enum class Density { Auto, Full, Medium, Compact, Rain };

    void setView(View);
    void loadEntry(int index, bool play); // asks the loader thread; frame() takes the result
    void loaderThread();
    void takeLoaded();
    void setPadMode(PadMode mode, bool samePressed);
    int pageCount() const;
    void padPress(int pad0, int velocity, bool fromReader, double nowMs);
    void padRelease(int pad0, double nowMs);
    void event(const juce::String& text);
    void buildRain(double nowMs);

    Player& player_;
    Midi& midi_;
    ScreenConfig config_;
    Library library_;
    std::unique_ptr<Canvas> canvas_;
    StatusRow* status_ = nullptr;
    ContextBar* context_ = nullptr;
    Browser* browser_ = nullptr;
    KnobStrip* knobs_ = nullptr;

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
        int sampleRate = 44100;
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
    std::array<std::uint16_t, 16> padHits_ {}; // presses per pad, as the player counts them (Player::push)
    bool shift_ = false;
    bool driveSeen_ = false, stripHold_ = false;
    int stripMode_ = 0; // Player::StripPitch
    int orderPage_ = 0, touchRow_ = -1;
    std::uint64_t mutes_ = 0, solos_ = 0;
    std::uint64_t padsHeld_ = 0;
    double lastFrameMs_ = 0, backDownMs_ = -1;
    juce::String statusBase_;
    std::array<double, 16> padHitMs_ {};
    // the scene
    std::vector<SceneRect> rects_, overlay_;
    std::vector<SceneGlyph> glyphs_;
};
} // namespace app
