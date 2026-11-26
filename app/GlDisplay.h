// The screen output: OpenGL ES 2 through EGL, on the unit's framebuffer ("fb") or in an X11 window ("x11": a desktop).
//
// Each frame:
//   1. JUCE's software renderer repaints the parts of the UI component that called repaint() into an image
//      (a CachedComponentImage on the root collects those areas; an idle screen costs nothing),
//   2. the changed rows go to a texture,
//   3. GL clears to the background, draws the scene's rectangles and glyphs (the pattern rain: thousands of characters
//      a frame, from a glyph atlas built once from the app's font), and the UI texture on top,
//   4. eglSwapBuffers waits for the display (60 Hz), which paces the app's main loop.
//
// The app links no GL library: every EGL/GL call goes through hw/common/GlDispatch, bound at start to one driver
// flavour (the unit ships its GPU driver in -fb and -x11 flavours that must never be mixed in one process).
#pragma once

#include "app/Screen.h"

#include "Platform.h"

#include <memory>
#include <string>
#include <string_view>

namespace app
{
class GlDisplay
{
public:
    GlDisplay();
    ~GlDisplay();

    // flavour "fb" (the unit) or "x11" (a window). Shows `ui` (800 × 480) from now on. False + reason on failure.
    bool open(const platform::RendererEnv&, std::string_view flavour, juce::Component& ui, std::string& error);
    // Draws and presents one frame. False when the surface is lost.
    bool draw(const SceneInput&);
    std::string describe() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace app
