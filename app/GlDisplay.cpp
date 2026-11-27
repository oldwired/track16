#include "app/GlDisplay.h"

#include "app/GlyphAtlas.h"
#include "app/Log.h"
#include "app/Theme.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace app
{
namespace
{
constexpr float kW = Screen::kWidth, kH = Screen::kHeight;

// Coloured triangles in screen pixels (0,0 = top left).
const char* const kVertexPixels = R"(
attribute vec2 aPos;
attribute vec4 aColour;
uniform vec2 uScreen;
varying vec4 vColour;
void main() {
    vColour = aColour;
    gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);
}
)";
const char* const kFragmentFlat = R"(
precision mediump float;
varying vec4 vColour;
void main() { gl_FragColor = vColour; }
)";

// Glyphs from the atlas (white on transparent, premultiplied): the vertex colour times the atlas coverage.
const char* const kVertexGlyph = R"(
attribute vec2 aPos;
attribute vec4 aColour;
attribute vec2 aUv;
uniform vec2 uScreen;
varying vec4 vColour;
varying vec2 vUv;
void main() {
    vColour = aColour;
    vUv = aUv;
    gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);
}
)";
const char* const kFragmentGlyph = R"(
precision mediump float;
varying vec4 vColour;
varying vec2 vUv;
uniform sampler2D uAtlas;
void main() { gl_FragColor = vColour * texture2D(uAtlas, vUv).a; }
)";

// The UI texture over the whole screen. It holds JUCE's premultiplied ARGB words, i.e. bytes B, G, R, A: swizzled.
const char* const kVertexFull = R"(
attribute vec2 aPos;
varying vec2 vUv;
void main() {
    vUv = vec2(aPos.x * 0.5 + 0.5, 0.5 - aPos.y * 0.5);
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";
const char* const kFragmentUi = R"(
precision mediump float;
varying vec2 vUv;
uniform sampler2D uUi;
void main() { gl_FragColor = texture2D(uUi, vUv).bgra; }
)";

GLuint compile(GLenum type, const char* source, std::string& error)
{
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (! ok)
    {
        char log[512] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        error = std::string("shader: ") + log;
    }
    return s;
}

GLuint link(const char* vs, const char* fs, std::string& error)
{
    const GLuint v = compile(GL_VERTEX_SHADER, vs, error), f = compile(GL_FRAGMENT_SHADER, fs, error);
    const GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aColour");
    glBindAttribLocation(p, 2, "aUv");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (! ok && error.empty())
        error = "the shader program does not link";
    return ok ? p : 0;
}

struct Vertex
{
    float x, y, r, g, b, a;
};
struct GlyphVertex
{
    float x, y, r, g, b, a, u, v;
};

constexpr int kCellW = kGlyphW, kCellH = kGlyphH;

// After the app quits, the framebuffer may still show its second buffer: pan back so whatever runs next (the boot menu)
// starts from a known state.
void panFramebufferBack()
{
    const int fd = ::open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return;
    fb_var_screeninfo v {};
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && (v.xoffset != 0 || v.yoffset != 0))
    {
        v.xoffset = v.yoffset = 0;
        ioctl(fd, FBIOPAN_DISPLAY, &v);
    }
    ::close(fd);
}
} // namespace

// The root component's repaint() areas, collected without a desktop window (the hook juce::OpenGLContext uses too).
struct DirtyAreas final : juce::CachedComponentImage
{
    explicit DirtyAreas(juce::Component& c) : root(c) {}
    void paint(juce::Graphics&) override {}
    bool invalidateAll() override { return invalidate(root.getLocalBounds()); }
    bool invalidate(const juce::Rectangle<int>& area) override
    {
        areas.add(area.getIntersection(root.getLocalBounds()));
        return false;
    }
    void releaseResources() override {}
    juce::Component& root;
    juce::RectangleList<int> areas;
};

struct GlDisplay::Impl
{
    platform::RendererEnv env;
    std::shared_ptr<void> window; // keeps the native window open
    std::string flavour, info;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLint width = 0, height = 0;
    GLuint flat = 0, ui = 0, texture = 0, glyphs = 0, atlas = 0;
    GLint flatScreen = -1, uiSampler = -1, glyphScreen = -1, glyphSampler = -1;
    std::vector<GlyphVertex> glyphVertices;
    std::vector<std::uint16_t> quadIndices;
    juce::Component* root = nullptr;
    DirtyAreas* dirty = nullptr; // owned by the root component
    juce::Image image {juce::Image::ARGB, Screen::kWidth, Screen::kHeight, true, juce::SoftwareImageType()};
    std::vector<Vertex> vertices;
    std::string capturePath; // the next frame goes there as a PNG (capture())

    // The frame just drawn (before the swap), as the panel shows it: read back, flipped, written as a PNG.
    void writeCapture(int x, int y, int w, int h)
    {
        const std::string path = std::move(capturePath);
        capturePath.clear();
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w * h * 4));
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        juce::Image shot(juce::Image::RGB, w, h, false, juce::SoftwareImageType());
        {
            juce::Image::BitmapData bits(shot, juce::Image::BitmapData::writeOnly);
            for (int row = 0; row < h; ++row)
            {
                const std::uint8_t* src = rgba.data() + static_cast<std::size_t>((h - 1 - row) * w * 4); // GL rows go up
                for (int col = 0; col < w; ++col, src += 4)
                    bits.setPixelColour(col, row, juce::Colour(src[0], src[1], src[2]));
            }
        }
        const juce::File file(path);
        file.deleteFile();
        juce::FileOutputStream out(file);
        juce::PNGImageFormat png;
        const bool ok = out.openedOk() && png.writeImageToStream(shot, out);
        logLine("SCREENSHOT %s %s (%dx%d)", ok ? "written" : "FAILED", path.c_str(), w, h);
    }

    bool fail(std::string& error, const char* what)
    {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s failed (EGL error 0x%x)", what, static_cast<unsigned>(eglGetError()));
        error = buf;
        return false;
    }

    // 1 + 2: repaint the dirty areas into the image and send the changed rows (full-width bands: GLES2 cannot upload
    // a sub-rectangle of a wider image) to the texture.
    void updateUi()
    {
        auto& areas = dirty->areas;
        if (areas.isEmpty())
            return;
        areas.consolidate();
        if (areas.getNumRectangles() > 8) // many small areas cost more in clipping than one box
            areas = juce::RectangleList<int>(areas.getBounds());
        for (const auto& r : areas)
            image.clear(r);
        {
            juce::Graphics g(image);
            g.reduceClipRegion(areas);
            root->paintEntireComponent(g, true);
        }
        const juce::Image::BitmapData bits(image, juce::Image::BitmapData::readOnly);
        juce::RectangleList<int> rows;
        for (const auto& r : areas)
            rows.add({0, r.getY(), Screen::kWidth, r.getHeight()});
        rows.consolidate();
        glBindTexture(GL_TEXTURE_2D, texture);
        for (const auto& r : rows)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, r.getY(), Screen::kWidth, r.getHeight(), GL_RGBA, GL_UNSIGNED_BYTE,
                            bits.getLinePointer(r.getY())); // rows are contiguous: lineStride = width × 4
        areas.clear();
    }

    static void unpack(std::uint32_t argb, float& r, float& g, float& b, float& a) // → premultiplied
    {
        a = static_cast<float>((argb >> 24) & 255) / 255.0f;
        r = static_cast<float>((argb >> 16) & 255) / 255.0f * a;
        g = static_cast<float>((argb >> 8) & 255) / 255.0f * a;
        b = static_cast<float>(argb & 255) / 255.0f * a;
    }

    // 3a: flat rectangles (two triangles each).
    void drawRects(const std::vector<SceneRect>& rects)
    {
        if (rects.empty())
            return;
        vertices.clear();
        for (const auto& q : rects)
        {
            float r, g, b, a;
            unpack(q.argb, r, g, b, a);
            const Vertex v[4] = {{q.x, q.y, r, g, b, a}, {q.x + q.w, q.y, r, g, b, a}, {q.x, q.y + q.h, r, g, b, a}, {q.x + q.w, q.y + q.h, r, g, b, a}};
            for (int i : {0, 1, 2, 1, 3, 2})
                vertices.push_back(v[i]);
        }
        glUseProgram(flat);
        glUniform2f(flatScreen, kW, kH);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), &vertices[0].x);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), &vertices[0].r);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
        glDisableVertexAttribArray(1);
    }

    // 3b: glyphs from the atlas, four vertices per glyph and a shared index list, in batches of ≤ 16384 glyphs.
    void drawGlyphs(const std::vector<SceneGlyph>& list)
    {
        if (list.empty() || glyphs == 0)
            return;
        glUseProgram(glyphs);
        glUniform2f(glyphScreen, kW, kH);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, atlas);
        glUniform1i(glyphSampler, 0);
        constexpr std::size_t kBatch = 16384;
        for (std::size_t from = 0; from < list.size(); from += kBatch)
        {
            const std::size_t count = std::min(kBatch, list.size() - from);
            glyphVertices.clear();
            for (std::size_t k = from; k < from + count; ++k)
            {
                const auto& q = list[k];
                const auto cell = glyphCell(q.ch);
                const float u0 = static_cast<float>(cell.x) / kAtlasW, v0 = static_cast<float>(cell.y) / kAtlasH;
                const float u1 = u0 + static_cast<float>(kCellW) / kAtlasW, v1 = v0 + static_cast<float>(kCellH) / kAtlasH;
                float r, g, b, a;
                unpack(q.argb, r, g, b, a);
                glyphVertices.push_back({q.x, q.y, r, g, b, a, u0, v0});
                glyphVertices.push_back({q.x + q.w, q.y, r, g, b, a, u1, v0});
                glyphVertices.push_back({q.x, q.y + q.h, r, g, b, a, u0, v1});
                glyphVertices.push_back({q.x + q.w, q.y + q.h, r, g, b, a, u1, v1});
            }
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVertex), &glyphVertices[0].x);
            glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(GlyphVertex), &glyphVertices[0].r);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GlyphVertex), &glyphVertices[0].u);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glEnableVertexAttribArray(2);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(count * 6), GL_UNSIGNED_SHORT, quadIndices.data());
        }
        glDisableVertexAttribArray(1);
        glDisableVertexAttribArray(2);
    }

    // The glyph atlas (app/GlyphAtlas.h), uploaded once.
    void buildAtlas()
    {
        const juce::Image img = glyphAtlas();
        const juce::Image::BitmapData bits(img, juce::Image::BitmapData::readOnly);
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kAtlasW * kAtlasH * 4));
        for (int y = 0; y < kAtlasH; ++y)
            for (int x = 0; x < kAtlasW; ++x)
            {
                const auto px = bits.getPixelColour(x, y);
                const auto o = static_cast<std::size_t>((y * kAtlasW + x) * 4);
                rgba[o] = rgba[o + 1] = rgba[o + 2] = 255;
                rgba[o + 3] = px.getAlpha();
            }
        glGenTextures(1, &atlas);
        glBindTexture(GL_TEXTURE_2D, atlas);
        for (auto [k, v] : {std::pair {GL_TEXTURE_MIN_FILTER, GL_LINEAR}, std::pair {GL_TEXTURE_MAG_FILTER, GL_LINEAR},
                            std::pair {GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE}, std::pair {GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE}})
            glTexParameteri(GL_TEXTURE_2D, k, v);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kAtlasW, kAtlasH, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        quadIndices.clear();
        for (std::uint16_t q = 0; q < 16384; ++q)
            for (int k : {0, 1, 2, 1, 3, 2})
                quadIndices.push_back(static_cast<std::uint16_t>(q * 4 + k));
    }
};

GlDisplay::GlDisplay() : impl_(std::make_unique<Impl>()) {}

GlDisplay::~GlDisplay()
{
    auto& d = *impl_;
    if (d.root != nullptr)
        d.root->setCachedComponentImage(nullptr);
    if (d.display != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(d.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (d.context != EGL_NO_CONTEXT)
            eglDestroyContext(d.display, d.context);
        if (d.surface != EGL_NO_SURFACE)
            eglDestroySurface(d.display, d.surface);
        eglTerminate(d.display);
    }
    if (d.flavour == "fb")
        panFramebufferBack();
}

bool GlDisplay::open(const platform::RendererEnv& env, std::string_view flavour, juce::Component& ui, std::string& error)
{
    auto& d = *impl_;
    d.env = env;
    d.flavour = flavour;
    if (! env.loadGl || ! env.loadGl(flavour, error))
        return error = error.empty() ? "no GL on this platform" : error, false;
    const auto window = env.nativeWindow(flavour, error);
    if (! window)
        return false;
    d.window = window->owner;

    // EGL: an RGB565-or-better window surface, an OpenGL ES 2 context, vsync.
    EGLint major = 0, minor = 0;
    d.display = eglGetDisplay((EGLNativeDisplayType) window->display);
    if (d.display == EGL_NO_DISPLAY || ! eglInitialize(d.display, &major, &minor))
        return d.fail(error, "eglInitialize");
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 5,
                                         EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (! eglChooseConfig(d.display, configAttrs, &config, 1, &count) || count < 1)
        return d.fail(error, "eglChooseConfig");
    // EGLNativeWindowType is an integer (X11 Window) or a pointer (the Vivante fb window), depending on the headers.
    d.surface = eglCreateWindowSurface(d.display, config, (EGLNativeWindowType) window->window, nullptr);
    if (d.surface == EGL_NO_SURFACE)
        return d.fail(error, "eglCreateWindowSurface");
    static const EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    d.context = eglCreateContext(d.display, config, EGL_NO_CONTEXT, contextAttrs);
    if (d.context == EGL_NO_CONTEXT || ! eglMakeCurrent(d.display, d.surface, d.surface, d.context))
        return d.fail(error, "eglCreateContext");
    eglSwapInterval(d.display, 1);
    eglQuerySurface(d.display, d.surface, EGL_WIDTH, &d.width);
    eglQuerySurface(d.display, d.surface, EGL_HEIGHT, &d.height);

    // Shaders and the UI texture (non-power-of-two is fine with clamp-to-edge and no mipmaps).
    d.flat = link(kVertexPixels, kFragmentFlat, error);
    d.ui = link(kVertexFull, kFragmentUi, error);
    d.glyphs = link(kVertexGlyph, kFragmentGlyph, error);
    if (d.flat == 0 || d.ui == 0 || d.glyphs == 0)
        return false;
    d.glyphScreen = glGetUniformLocation(d.glyphs, "uScreen");
    d.glyphSampler = glGetUniformLocation(d.glyphs, "uAtlas");
    d.buildAtlas();
    d.flatScreen = glGetUniformLocation(d.flat, "uScreen");
    d.uiSampler = glGetUniformLocation(d.ui, "uUi");
    glGenTextures(1, &d.texture);
    glBindTexture(GL_TEXTURE_2D, d.texture);
    for (auto [k, v] : {std::pair {GL_TEXTURE_MIN_FILTER, GL_LINEAR}, std::pair {GL_TEXTURE_MAG_FILTER, GL_LINEAR},
                        std::pair {GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE}, std::pair {GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE}})
        glTexParameteri(GL_TEXTURE_2D, k, v);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, Screen::kWidth, Screen::kHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    d.vertices.reserve(1024);

    // The UI: shown without a desktop window. It takes no JUCE mouse clicks (touch comes from the platform), which
    // also keeps JUCE from creating juce::Desktop, whose X11 windowing crashes on the unit.
    d.root = &ui;
    ui.setInterceptsMouseClicks(false, false);
    ui.setVisible(true); // repaint() stops at an invisible component
    d.dirty = new DirtyAreas(ui);
    ui.setCachedComponentImage(d.dirty);
    d.dirty->invalidateAll();

    char text[160];
    std::snprintf(text, sizeof text, "%s EGL %d.%d %s, surface %dx%d, %s, %s", d.flavour.c_str(), major, minor,
                  eglQueryString(d.display, EGL_VENDOR), d.width, d.height, reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                  window->info.c_str());
    d.info = text;
    return true;
}

bool GlDisplay::draw(const SceneInput& s)
{
    auto& d = *impl_;
    d.updateUi();
    // The 800 × 480 picture, scaled to fit the surface (the unit: 1:1; a desktop window may be larger).
    const float k = std::min(static_cast<float>(d.width) / kW, static_cast<float>(d.height) / kH);
    const int w = static_cast<int>(kW * k), h = static_cast<int>(kH * k);
    glViewport(0, 0, d.width, d.height);
    glClearColor(s.background.getFloatRed(), s.background.getFloatGreen(), s.background.getFloatBlue(), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport((d.width - w) / 2, (d.height - h) / 2, w, h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // everything is premultiplied
    if (s.rects != nullptr)
        d.drawRects(*s.rects);
    if (s.glyphs != nullptr)
        d.drawGlyphs(*s.glyphs);
    if (s.overlay != nullptr)
        d.drawRects(*s.overlay);
    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glUseProgram(d.ui);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d.texture);
    glUniform1i(d.uiSampler, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (! d.capturePath.empty())
        d.writeCapture((d.width - w) / 2, (d.height - h) / 2, w, h);
    return eglSwapBuffers(d.display, d.surface) != 0; // waits for vsync (swap interval 1)
}

void GlDisplay::capture(const std::string& pngPath) { impl_->capturePath = pngPath; }

std::string GlDisplay::describe() const
{
    return impl_->info + " | " + (impl_->env.glReport ? impl_->env.glReport() : std::string());
}
} // namespace app
