#include "app/Theme.h"

#include "AppFonts.h" // the embedded Inter and JetBrains Mono faces (juce_add_binary_data in CMakeLists.txt)

#include <algorithm>
#include <cmath>

namespace app::theme
{
namespace
{
struct Faces
{
    juce::Typeface::Ptr regular, medium, semibold, mono, monoBold;
};
Faces& faces()
{
    static Faces f;
    return f;
}

class InterLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    juce::Typeface::Ptr getTypefaceForFont(const juce::Font& f) override { return f.isBold() ? faces().semibold : faces().regular; }
};
std::unique_ptr<InterLookAndFeel>& laf()
{
    static std::unique_ptr<InterLookAndFeel> l;
    return l;
}

juce::Font make(const juce::Typeface::Ptr& face, float size)
{
    // FontOptions(typeface), not withTypeface() on a default FontOptions (JUCE asserts its name/style are empty).
    return juce::Font(juce::FontOptions(face).withPointHeight(size).withFallbackEnabled(false));
}

struct Style
{
    float size, line;
    int weight;
    float tracking;
};
// tokens.json typography: caption 11/14 600 +0.06 em caps, label 13/16 600 +0.04 em caps, body 15/20, body-strong
// 15/20 600, title 17/22 600, value-s 17/22 500 tnum, value 24/28 500 tnum, value-l 32/36 500 tnum.
constexpr Style kStyles[] = {{11, 14, 600, 0.06f}, {13, 16, 600, 0.04f}, {15, 20, 400, 0}, {15, 20, 600, 0},
                             {17, 22, 600, 0},     {17, 22, 500, 0},     {24, 28, 500, 0}, {32, 36, 500, 0}};
const Style& style(Text t) { return kStyles[static_cast<int>(t)]; }
} // namespace

juce::Font mono(bool bold, float em) { return make(bold ? faces().monoBold : faces().mono, em); }

void installFonts()
{
    auto& f = faces();
    f.regular = juce::Typeface::createSystemTypefaceFor(AppFonts::InterRegular_subset_ttf, AppFonts::InterRegular_subset_ttfSize);
    f.medium = juce::Typeface::createSystemTypefaceFor(AppFonts::InterMedium_subset_ttf, AppFonts::InterMedium_subset_ttfSize);
    f.semibold = juce::Typeface::createSystemTypefaceFor(AppFonts::InterSemiBold_subset_ttf, AppFonts::InterSemiBold_subset_ttfSize);
    f.mono = juce::Typeface::createSystemTypefaceFor(AppFonts::JetBrainsMonoNLMedium_subset_ttf, AppFonts::JetBrainsMonoNLMedium_subset_ttfSize);
    f.monoBold = juce::Typeface::createSystemTypefaceFor(AppFonts::JetBrainsMonoNLBold_subset_ttf, AppFonts::JetBrainsMonoNLBold_subset_ttfSize);
    laf() = std::make_unique<InterLookAndFeel>();
}

void releaseFonts()
{
    laf().reset();
    faces() = {};
}

juce::LookAndFeel& lookAndFeel() { return *laf(); }

juce::Font font(Text t)
{
    const auto& s = style(t);
    auto f = make(s.weight >= 600 ? faces().semibold : s.weight >= 500 ? faces().medium : faces().regular, s.size);
    if (s.tracking != 0)
        f.setExtraKerningFactor(s.tracking);
    return f;
}

float lineHeight(Text t) { return style(t).line; }

float textWidth(Text t, const juce::String& s) { return juce::GlyphArrangement::getStringWidth(font(t), s); }

void text(juce::Graphics& g, float x, float cy, Align a, Text t, juce::Colour c, const juce::String& s)
{
    const auto f = font(t);
    const float w = juce::GlyphArrangement::getStringWidth(f, s) + 2.0f, h = lineHeight(t);
    const float left = a == Align::Left ? x : a == Align::Centre ? x - w / 2 : x - w;
    g.setFont(f);
    g.setColour(c);
    g.drawText(s, juce::Rectangle<float>(left, cy - h / 2, w, h), juce::Justification::centred, false);
}

float badge(juce::Graphics& g, float x, float y, const juce::String& s, juce::Colour fill, juce::Colour ink, float w)
{
    if (w <= 0)
        w = textWidth(Text::Caption, s) + 16;
    g.setColour(fill);
    g.fillRoundedRectangle(x, y, w, 22, radiusXs);
    text(g, x + w / 2, y + 12, Align::Centre, Text::Caption, ink, s);
    return x + w;
}

void hline(juce::Graphics& g, float x0, float x1, float y, juce::Colour c)
{
    g.setColour(c);
    g.fillRect(x0, y, x1 - x0, 1.0f);
}

void vline(juce::Graphics& g, float x, float y0, float y1, juce::Colour c)
{
    g.setColour(c);
    g.fillRect(x, y0, 1.0f, y1 - y0);
}

void glyph(juce::Graphics& g, Glyph kind, float x, float y, float size, juce::Colour c)
{
    const float k = size / 24.0f, w = std::max(1.5f, 2.0f * k);
    juce::Path p;
    auto line = [&](float x1, float y1, float x2, float y2) {
        p.startNewSubPath(x + x1 * k, y + y1 * k);
        p.lineTo(x + x2 * k, y + y2 * k);
    };
    auto box = [&](float bx, float by, float bw, float bh) {
        g.setColour(c);
        g.fillRoundedRectangle(x + bx * k, y + by * k, bw * k, bh * k, k);
    };
    switch (kind)
    {
    case Glyph::Back: // ↩ arrow, 32 × 28
        line(0, 20, 22, 20);
        p.startNewSubPath(x + 22 * k, y + 20 * k);
        p.addCentredArc(x + 22 * k, y + 10 * k, 10 * k, 10 * k, 0, juce::MathConstants<float>::pi, 0);
        line(22, 0, 8, 0);
        line(0, 20, 8, 12);
        line(0, 20, 8, 28);
        break;
    case Glyph::Project: // a document with a folded corner and two text lines, 24 × 30
        p.startNewSubPath(x, y);
        for (auto [px, py] : {std::pair {16.f, 0.f}, {24.f, 8.f}, {24.f, 30.f}, {0.f, 30.f}})
            p.lineTo(x + px * k, y + py * k);
        p.closeSubPath();
        line(16, 0, 16, 8);
        line(16, 8, 24, 8);
        line(4, 18, 20, 18);
        line(4, 24, 16, 24);
        break;
    case Glyph::Tracks:
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                box(2.0f + 6.0f * static_cast<float>(i), 2.0f + 6.0f * static_cast<float>(j), 4, 4);
        line(16, 22, 23, 15);
        break;
    case Glyph::Mixer:
    {
        static constexpr float caps[] = {14, 6, 11};
        for (int i = 0; i < 3; ++i)
        {
            const float fx = 5.0f + 7.0f * static_cast<float>(i);
            line(fx, 2, fx, 22);
            box(fx - 3, caps[i], 6, 3);
        }
        break;
    }
    case Glyph::Arranger:
        box(2, 3, 12, 4);
        box(6, 10, 14, 4);
        box(2, 17, 8, 4);
        break;
    case Glyph::Utility:
        p.addEllipse(x + 8 * k, y + 8 * k, 8 * k, 8 * k);
        for (int i = 0; i < 8; ++i)
        {
            const float a = static_cast<float>(i) * juce::MathConstants<float>::pi / 4;
            line(12 + 7 * std::cos(a), 12 + 7 * std::sin(a), 12 + 10 * std::cos(a), 12 + 10 * std::sin(a));
        }
        break;
    case Glyph::Play: // ▶ (status row), filled
        p.addTriangle(x, y, x, y + size, x + size * 0.85f, y + size / 2);
        g.setColour(c);
        g.fillPath(p);
        return;
    case Glyph::Dot: // ● (status row), filled
        g.setColour(c);
        g.fillEllipse(x, y, size, size);
        return;
    }
    g.setColour(c);
    g.strokePath(p, juce::PathStrokeType(w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

float meterY(float db, float top, float bottom)
{
    const float span = bottom - top - 12.0f;
    if (db <= -50.0f)
        return bottom - 12.0f + std::min(12.0f, (-50.0f - db) * 0.4f);
    return top + (3.0f - std::min(db, 3.0f)) * span / 53.0f;
}

void meterBars(juce::Graphics& g, float x, float top, float bottom, float pitch, const float db[2], const float peakDb[2],
               bool dim)
{
    const float op = dim ? 0.4f : 1.0f;
    for (int ch = 0; ch < 2; ++ch)
    {
        const float bx = x + pitch * static_cast<float>(ch);
        g.setColour(colour::surface3);
        g.fillRoundedRectangle(bx, top, 8, bottom - top, 2);
        const float d = std::clamp(db[ch], -120.0f, 3.0f);
        if (d > -119.0f)
        {
            const float yv = meterY(d, top, bottom), yMid = meterY(-6.0f, top, bottom), yHigh = meterY(-1.0f, top, bottom);
            const float lowTop = std::max(yv, yMid);
            g.setColour(colour::meterLow.withMultipliedAlpha(op));
            g.fillRoundedRectangle(bx, lowTop, 8, bottom - lowTop, 2);
            if (yv < yMid)
            {
                g.setColour(colour::meterMid.withMultipliedAlpha(op));
                g.fillRect(bx, std::max(yv, yHigh), 8.0f, yMid - std::max(yv, yHigh));
            }
            if (yv < yHigh)
            {
                g.setColour(colour::meterHigh.withMultipliedAlpha(op));
                g.fillRect(bx, yv, 8.0f, yHigh - yv);
            }
        }
        if (peakDb[ch] > -50.0f)
        {
            g.setColour(colour::text1.withMultipliedAlpha(op));
            g.fillRect(bx, meterY(peakDb[ch], top, bottom) - 1, 8.0f, 2.0f);
        }
    }
}
} // namespace app::theme
