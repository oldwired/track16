// track16_preview: renders the screen to PNG files on a build machine, the way the unit shows it, to check the look
// without a unit. The real Screen and Player run headless (no audio device, no panel): the tool loads modules through
// the library like a user (SELECT, PUSH, PLAY), runs the player faster than real time, and composites what GlDisplay
// draws (background, the scene's rectangles and glyphs from the same atlas, the overlay, the UI on top), reduced to
// the panel's 16-bit colour (RGB565).
//
//   track16_preview <modules folder> <out folder> [module file name …]
//   → library.png, then per module player-<name>.png, -mute.png (MUTE pads: one muted, one solo) and -outs.png (the
//     OUTS page with three channels routed)
//
// Build and run with scripts/preview.sh (inside the toolchain container, natively).
#include "app/GlyphAtlas.h"
#include "app/Library.h"
#include "app/Midi.h"
#include "app/Player.h"
#include "app/Screen.h"
#include "app/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace app;

namespace
{
constexpr int kRate = 44100, kPeriod = 128;
using C = io::Control;
using K = io::ControlEvent::Kind;

struct Rig
{
    Player& player;
    Screen& screen;
    std::vector<std::vector<float>> out = std::vector<std::vector<float>>(12, std::vector<float>(kPeriod));
    float* outPtr[12] {};
    double audioMs = 0; // the audio's time
    double nextFrameMs = 0;

    Rig(Player& p, Screen& s) : player(p), screen(s)
    {
        for (int i = 0; i < 12; ++i)
            outPtr[i] = out[static_cast<std::size_t>(i)].data();
        for (int i = 2; i < 12; ++i) // MAIN only
            outPtr[i] = nullptr;
    }
    static std::uint64_t now()
    {
        timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
    }
    // Audio for `seconds`, a screen frame every 1/60 s of it.
    void run(double seconds)
    {
        const long periods = static_cast<long>(seconds * kRate / kPeriod);
        for (long k = 0; k < periods; ++k)
        {
            platform::AudioIO io {outPtr, 6, nullptr, 0, kPeriod, now()};
            player.process(io);
            audioMs += 1000.0 * kPeriod / kRate;
            if (audioMs >= nextFrameMs)
            {
                screen.frame(audioMs, 60);
                nextFrameMs = audioMs + 1000.0 / 60;
            }
        }
    }
    void wait(double ms) // real time passes (the loader and prober threads), the screen goes on
    {
        for (double t = 0; t < ms; t += 20)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            run(0.02);
        }
    }
    void press(C control, int value = 0)
    {
        screen.handle({K::Press, control, static_cast<std::int16_t>(value), 0, audioMs}, audioMs);
        screen.handle({K::Release, control, 0, 0, audioMs}, audioMs);
    }
    void hold(C control, bool down) { screen.handle({down ? K::Press : K::Release, control, 0, 0, audioMs}, audioMs); }
    void turn(C control, int detents) { screen.handle({K::Turn, control, static_cast<std::int16_t>(detents), 0, audioMs}, audioMs); }
};

// What GlDisplay draws, in software: premultiplied colour, blended "one, one minus source alpha".
struct Canvas
{
    int w = Screen::kWidth, h = Screen::kHeight;
    std::vector<float> rgb = std::vector<float>(static_cast<std::size_t>(w * h * 3));
    void clear(juce::Colour c)
    {
        for (std::size_t i = 0; i < rgb.size(); i += 3)
            rgb[i] = c.getFloatRed(), rgb[i + 1] = c.getFloatGreen(), rgb[i + 2] = c.getFloatBlue();
    }
    void blend(int x, int y, float r, float g, float b, float a) // r, g, b premultiplied
    {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return;
        float* p = &rgb[static_cast<std::size_t>((y * w + x) * 3)];
        p[0] = r + p[0] * (1 - a), p[1] = g + p[1] * (1 - a), p[2] = b + p[2] * (1 - a);
    }
    static void unpack(std::uint32_t argb, float& r, float& g, float& b, float& a)
    {
        a = static_cast<float>((argb >> 24) & 255) / 255.0f;
        r = static_cast<float>((argb >> 16) & 255) / 255.0f * a;
        g = static_cast<float>((argb >> 8) & 255) / 255.0f * a;
        b = static_cast<float>(argb & 255) / 255.0f * a;
    }
    void rects(const std::vector<SceneRect>* list) // pixels whose centre is inside, as GL rasterises
    {
        if (list == nullptr)
            return;
        for (const auto& q : *list)
        {
            float r, g, b, a;
            unpack(q.argb, r, g, b, a);
            const int x0 = static_cast<int>(std::ceil(q.x - 0.5f)), x1 = static_cast<int>(std::ceil(q.x + q.w - 0.5f));
            const int y0 = static_cast<int>(std::ceil(q.y - 0.5f)), y1 = static_cast<int>(std::ceil(q.y + q.h - 0.5f));
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                    blend(x, y, r, g, b, a);
        }
    }
    void glyphs(const std::vector<SceneGlyph>* list, const juce::Image& atlas)
    {
        if (list == nullptr)
            return;
        const juce::Image::BitmapData bits(atlas, juce::Image::BitmapData::readOnly);
        for (const auto& q : *list)
        {
            float r, g, b, a;
            unpack(q.argb, r, g, b, a);
            const auto cell = glyphCell(q.ch);
            const int gx = static_cast<int>(std::lround(q.x)), gy = static_cast<int>(std::lround(q.y));
            for (int j = 0; j < kGlyphH; ++j)
                for (int i = 0; i < kGlyphW; ++i)
                {
                    const float t = bits.getPixelColour(cell.x + i, cell.y + j).getFloatAlpha();
                    if (t > 0)
                        blend(gx + i, gy + j, r * t, g * t, b * t, a * t);
                }
        }
    }
    void ui(const juce::Image& img)
    {
        const juce::Image::BitmapData bits(img, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const auto c = bits.getPixelColour(x, y);
                const float a = c.getFloatAlpha();
                if (a > 0)
                    blend(x, y, c.getFloatRed() * a, c.getFloatGreen() * a, c.getFloatBlue() * a, a);
            }
    }
    juce::Image image565() const
    {
        juce::Image img(juce::Image::RGB, w, h, false, juce::SoftwareImageType());
        juce::Image::BitmapData bits(img, juce::Image::BitmapData::writeOnly);
        const auto q = [](float v, float levels) {
            return static_cast<juce::uint8>(std::lround(std::round(std::clamp(v, 0.0f, 1.0f) * levels) / levels * 255.0f));
        };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const float* p = &rgb[static_cast<std::size_t>((y * w + x) * 3)];
                bits.setPixelColour(x, y, juce::Colour(q(p[0], 31), q(p[1], 63), q(p[2], 31)));
            }
        return img;
    }
};

void render(Rig& rig, const juce::Image& atlas, const std::string& path)
{
    const SceneInput s = rig.screen.scene(rig.audioMs);
    Canvas cv;
    cv.clear(s.background);
    cv.rects(s.rects);
    cv.glyphs(s.glyphs, atlas);
    cv.rects(s.overlay);
    juce::Image ui(juce::Image::ARGB, Screen::kWidth, Screen::kHeight, true, juce::SoftwareImageType());
    {
        juce::Graphics g(ui);
        rig.screen.paintEntireComponent(g, true);
    }
    cv.ui(ui);
    const juce::File file(path);
    file.deleteFile();
    juce::FileOutputStream out(file);
    juce::PNGImageFormat png;
    std::printf("%s %s\n", out.openedOk() && png.writeImageToStream(cv.image565(), out) ? "wrote" : "FAILED", path.c_str());
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: track16_preview <modules folder> <out folder> [module file name …]\n");
        return 2;
    }
    const std::string modules = argv[1], outDir = argv[2];
    juce::File(outDir).createDirectory();
    juce::ScopedJuceInitialiser_GUI juceInit;
    theme::installFonts();
    struct FontsRelease { ~FontsRelease() { theme::releaseFonts(); } } fontsRelease;
    {
        Player player;
        Midi midi(player, "none");
        player.setMidi(&midi);
        player.prepare(kRate, kPeriod);
        player.armMeasure();
        ScreenConfig config;
        config.roots = {modules};
        config.dataDir = outDir + "/data";
        Screen screen(player, midi, config);
        screen.setStatus({"preview", "software", "none"});
        const juce::Image atlas = glyphAtlas();
        Rig rig(player, screen);
        screen.start();
        rig.wait(3000); // the library's facts
        render(rig, atlas, outDir + "/library.png");

        Library library; // the same scan, for the entries' positions
        library.scan({modules});
        int selected = 0;
        for (int a = 3; a < argc; ++a)
        {
            const std::string name = argv[a];
            int index = -1;
            for (std::size_t i = 0; i < library.entries().size(); ++i)
                if (library.entries()[i].name == name)
                    index = static_cast<int>(i);
            if (index < 0)
            {
                std::fprintf(stderr, "not in the library: %s\n", name.c_str());
                continue;
            }
            rig.turn(C::Selector, index - selected);
            selected = index;
            rig.press(C::SelectorPush); // load, the player view
            rig.wait(1500);
            rig.press(C::Play);
            rig.run(12.0);
            render(rig, atlas, outDir + "/player-" + name + ".png");
            // MUTE pads: pad 2 muted, pad 3 solo (SHIFT), the filter on pad 1 held.
            rig.press(C::PadModeMute);
            rig.press(io::pad(2));
            rig.hold(C::Shift, true);
            rig.press(io::pad(3));
            rig.hold(C::Shift, false);
            rig.run(2.0);
            render(rig, atlas, outDir + "/player-" + name + "-mute.png");
            rig.hold(C::Shift, true); // the OUTS page: channel 1 to OUT 3, channels 2 and 3 to OUT 5-6
            rig.press(C::PadModeTrack);
            rig.hold(C::Shift, false);
            rig.hold(io::pad(1), true);
            rig.press(io::step(3));
            rig.hold(io::pad(1), false);
            rig.hold(io::pad(2), true);
            rig.hold(io::pad(3), true);
            rig.hold(io::step(5), true);
            rig.press(io::step(6));
            rig.hold(io::step(5), false);
            rig.hold(io::pad(2), false);
            rig.hold(io::pad(3), false);
            rig.run(1.0);
            render(rig, atlas, outDir + "/player-" + name + "-outs.png");
            rig.press(C::Stop);
            rig.hold(C::Shift, true); // SHIFT + STOP: mutes and solos back
            rig.press(C::Stop);
            rig.hold(C::Shift, false);
            rig.press(C::PadModeTrack);
            rig.press(C::Back); // to the library
            rig.run(0.1);
        }
    }
    return 0;
}
