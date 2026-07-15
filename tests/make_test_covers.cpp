// Generate placeholder PS5-style cover art for known game titles so the
// UI has real thumbnails to display during development.  We do not
// depend on network fetch or a third-party font: this paints a clean,
// quiet, PS5-themed tile with a dark gradient, a "PS5" badge drawn
// from primitive shapes (no bitmap font), a stylised PS-circle motif
// in the centre, and a thin coloured accent derived from the title id.
//
// The actual title id is *not* rendered into the PNG; the UI overlays
// it as crisp ImGui text below the cover, which avoids the broken
// bitmap-font artefacts the previous version suffered from.
//
// Output: ./Covers/<TITLE_ID>.png — 512x512 RGBA8.

#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

constexpr int kW = 512;
constexpr int kH = 512;

struct Rgba {
    std::uint8_t r, g, b, a;
};

struct Image {
    std::vector<Rgba> px;

    Image() : px(static_cast<std::size_t>(kW) * static_cast<std::size_t>(kH)) {}

    Rgba& at(int x, int y) {
        return px[static_cast<std::size_t>(y) * kW + x];
    }
    const Rgba& at(int x, int y) const {
        return px[static_cast<std::size_t>(y) * kW + x];
    }

    // Blend `c` over the existing pixel with alpha `a` (0..255).
    void blend(int x, int y, Rgba c) {
        if (x < 0 || y < 0 || x >= kW || y >= kH) return;
        if (c.a == 0) return;
        Rgba& d = at(x, y);
        if (c.a == 255) {
            d = c;
            return;
        }
        const std::uint32_t sa = c.a;
        const std::uint32_t inv = 255u - sa;
        d.r = static_cast<std::uint8_t>((c.r * sa + d.r * inv) / 255u);
        d.g = static_cast<std::uint8_t>((c.g * sa + d.g * inv) / 255u);
        d.b = static_cast<std::uint8_t>((c.b * sa + d.b * inv) / 255u);
        d.a = static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(d.a) + sa > 255u)
                ? 255u
                : static_cast<std::uint32_t>(d.a) + sa);
    }
};

// -------------------- small drawing helpers --------------------

static void FillRect(Image& img, int x0, int y0, int x1, int y1, Rgba c) {
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(kW, x1);
    y1 = std::min(kH, y1);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            img.at(x, y) = c;
        }
    }
}

static void FillRoundRect(Image& img, float x0f, float y0f, float x1f,
                          float y1f, float radius, Rgba c) {
    int x0 = std::max(0, static_cast<int>(x0f));
    int y0 = std::max(0, static_cast<int>(y0f));
    int x1 = std::min(kW, static_cast<int>(x1f));
    int y1 = std::min(kH, static_cast<int>(y1f));
    const float r = radius;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float dx = (x + 0.5f) - (x0f + r);
            const float dy = (y + 0.5f) - (y0f + r);
            const float dx2 = (x + 0.5f) - (x1f - r - 1.0f);
            const float dy2 = (y + 0.5f) - (y1f - r - 1.0f);
            // Reject corners outside the rounded radius.
            const bool tl = (x < x0 + r && y < y0 + r) &&
                            (dx * dx + dy * dy > r * r);
            const bool tr = (x >= x1 - r && y < y0 + r) &&
                            (dx2 * dx2 + dy * dy > r * r);
            const bool bl = (x < x0 + r && y >= y1 - r) &&
                            (dx * dx + dy2 * dy2 > r * r);
            const bool br = (x >= x1 - r && y >= y1 - r) &&
                            (dx2 * dx2 + dy2 * dy2 > r * r);
            if (tl || tr || bl || br) continue;
            img.blend(x, y, c);
        }
    }
}

static void StrokeRoundRect(Image& img, float x0f, float y0f, float x1f,
                            float y1f, float radius, float thickness,
                            Rgba c) {
    // Sample the inner region with a half-thickness check.
    const int x0 = std::max(0, static_cast<int>(x0f));
    const int y0 = std::max(0, static_cast<int>(y0f));
    const int x1 = std::min(kW, static_cast<int>(x1f));
    const int y1 = std::min(kH, static_cast<int>(y1f));
    const float r = radius;
    const float t = thickness;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            // distance to the rounded-rect outline
            const float cx = std::min(std::max(x + 0.5f, x0f + r), x1f - r - 1.0f);
            const float cy = std::min(std::max(y + 0.5f, y0f + r), y1f - r - 1.0f);
            const float dx = (x + 0.5f) - cx;
            const float dy = (y + 0.5f) - cy;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d <= r && d >= r - t) {
                img.blend(x, y, c);
            }
        }
    }
}

static void FillCircle(Image& img, float cxf, float cyf, float radius,
                       Rgba c) {
    const int x0 = std::max(0, static_cast<int>(cxf - radius - 1));
    const int y0 = std::max(0, static_cast<int>(cyf - radius - 1));
    const int x1 = std::min(kW, static_cast<int>(cxf + radius + 2));
    const int y1 = std::min(kH, static_cast<int>(cyf + radius + 2));
    const float r = radius;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float dx = (x + 0.5f) - cxf;
            const float dy = (y + 0.5f) - cyf;
            if (dx * dx + dy * dy <= r * r) {
                img.blend(x, y, c);
            }
        }
    }
}

static void StrokeCircle(Image& img, float cxf, float cyf, float radius,
                         float thickness, Rgba c) {
    const int x0 = std::max(0, static_cast<int>(cxf - radius - 2));
    const int y0 = std::max(0, static_cast<int>(cyf - radius - 2));
    const int x1 = std::min(kW, static_cast<int>(cxf + radius + 3));
    const int y1 = std::min(kH, static_cast<int>(cyf + radius + 3));
    const float r = radius;
    const float t = thickness;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float dx = (x + 0.5f) - cxf;
            const float dy = (y + 0.5f) - cyf;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d <= r && d >= r - t) {
                img.blend(x, y, c);
            }
        }
    }
}

// -------------------- "PS5" badge drawn from primitives --------------------
// We avoid the broken 5x7 bitmap font from the previous version by drawing
// the two glyphs "PS" with vector primitives, and a small numeric "5"
// rectangle next to it.  Result is consistently legible at 512px.

// Glyph descriptor: list of filled rounded-rect strokes that draw a letter.
struct Stroke {
    float x0, y0, x1, y1, r;
    bool  filled;   // true = fill, false = outline
};

static void DrawPS5Badge(Image& img, float x0f, float y0f, float height) {
    // All coordinates relative to (x0f, y0f), with `height` ~= 70px.
    const float h = height;
    const float w = h * 2.4f;        // wider than tall
    const float t = h * 0.16f;       // stroke thickness
    const float r = t * 0.45f;       // corner radius

    // P is at the left half; "5" is the right half.
    const float pg_w = w * 0.55f;
    const float s5_x0 = x0f + pg_w + w * 0.05f;
    const float s5_w  = w - pg_w - w * 0.05f;

    // ---- Letter "P" ----
    // Vertical bar
    FillRect(img, x0f, y0f, x0f + t, y0f + h, Rgba{235, 240, 245, 255});
    // Top bowl: outer rounded rect minus inner cutout
    FillRoundRect(img, x0f, y0f, x0f + pg_w, y0f + h * 0.55f, r,
                  Rgba{235, 240, 245, 255});
    // Cut hole in the bowl
    FillRoundRect(img, x0f + t, y0f + t, x0f + pg_w - t, y0f + h * 0.55f - t,
                  r * 0.6f, Rgba{0, 0, 0, 0});
    // Re-establish the bottom of the P (where the bowl meets the stem)
    FillRect(img, x0f, y0f + h * 0.45f, x0f + pg_w, y0f + h * 0.55f,
             Rgba{235, 240, 245, 255});
    // The "P" would normally have an enclosed bowl; the cutout above
    // makes the centre transparent so we can re-draw a clean bottom
    // edge of the bowl.  Done.

    // ---- Letter "S" (5 in our PS5 logo is rendered as a stylised S) ----
    // We draw a simple thick "5" using three horizontal bars and a curve
    // approximated with a few small rounded rects.
    const float y5 = y0f;
    // Top bar
    FillRect(img, s5_x0, y5, s5_x0 + s5_w, y5 + t, Rgba{235, 240, 245, 255});
    // Left vertical of the top half
    FillRect(img, s5_x0, y5, s5_x0 + t, y5 + h * 0.5f,
             Rgba{235, 240, 245, 255});
    // Middle bar
    FillRect(img, s5_x0, y5 + h * 0.45f, s5_x0 + s5_w,
             y5 + h * 0.45f + t, Rgba{235, 240, 245, 255});
    // Right vertical of the bottom half
    FillRect(img, s5_x0 + s5_w - t, y5 + h * 0.45f, s5_x0 + s5_w, y5 + h,
             Rgba{235, 240, 245, 255});
    // Bottom bar
    FillRect(img, s5_x0, y5 + h - t, s5_x0 + s5_w, y5 + h,
             Rgba{235, 240, 245, 255});
}

// -------------------- minimal 3x5 bitmap font --------------------
// Hand-crafted 3-wide x 5-tall glyphs for the ASCII characters that
// appear in PS5 title ids (uppercase letters A, P, S and digits 0-9).
// The previous version used a generic 5x7 font whose glyphs were
// inconsistent (e.g. 'P' was a flat bar with a tiny dot) and produced
// garbled text.  This minimal set is hand-verified to render correctly
// and is enough to print the title id in the bottom strip.
struct Glyph35 { std::uint8_t rows[5]; };

// Look up the glyph for a single ASCII character.  Returns false if
// the character is unmapped (DrawChar3x5 will then skip it).
static bool GetGlyph35(char c, Glyph35& out) {
    switch (c) {
        case '0': out.rows[0]=0x7; out.rows[1]=0x5; out.rows[2]=0x5;
                  out.rows[3]=0x5; out.rows[4]=0x7; return true;
        case '1': out.rows[0]=0x2; out.rows[1]=0x6; out.rows[2]=0x2;
                  out.rows[3]=0x2; out.rows[4]=0x7; return true;
        case '2': out.rows[0]=0x7; out.rows[1]=0x1; out.rows[2]=0x7;
                  out.rows[3]=0x4; out.rows[4]=0x7; return true;
        case '3': out.rows[0]=0x7; out.rows[1]=0x1; out.rows[2]=0x7;
                  out.rows[3]=0x1; out.rows[4]=0x7; return true;
        case '4': out.rows[0]=0x5; out.rows[1]=0x5; out.rows[2]=0x7;
                  out.rows[3]=0x1; out.rows[4]=0x1; return true;
        case '5': out.rows[0]=0x7; out.rows[1]=0x4; out.rows[2]=0x7;
                  out.rows[3]=0x1; out.rows[4]=0x7; return true;
        case '6': out.rows[0]=0x7; out.rows[1]=0x4; out.rows[2]=0x7;
                  out.rows[3]=0x5; out.rows[4]=0x7; return true;
        case '7': out.rows[0]=0x7; out.rows[1]=0x1; out.rows[2]=0x1;
                  out.rows[3]=0x1; out.rows[4]=0x1; return true;
        case '8': out.rows[0]=0x7; out.rows[1]=0x5; out.rows[2]=0x7;
                  out.rows[3]=0x5; out.rows[4]=0x7; return true;
        case '9': out.rows[0]=0x7; out.rows[1]=0x5; out.rows[2]=0x7;
                  out.rows[3]=0x1; out.rows[4]=0x7; return true;
        case 'A': out.rows[0]=0x2; out.rows[1]=0x5; out.rows[2]=0x7;
                  out.rows[3]=0x5; out.rows[4]=0x5; return true;
        case 'P': out.rows[0]=0x6; out.rows[1]=0x5; out.rows[2]=0x6;
                  out.rows[3]=0x4; out.rows[4]=0x4; return true;
        case 'S': out.rows[0]=0x3; out.rows[1]=0x4; out.rows[2]=0x2;
                  out.rows[3]=0x1; out.rows[4]=0x6; return true;
        default: return false;
    }
}

// Render a single glyph at integer coordinates (no anti-aliasing — the
// rectangles are large enough at scale 4+ that the result still looks
// crisp when the cover is downscaled to ~160px in the UI).
static void DrawChar3x5(Image& img, int x, int y, int scale, char c,
                        Rgba col) {
    Glyph35 g;
    if (!GetGlyph35(c, g)) return;
    for (int row = 0; row < 5; ++row) {
        const std::uint8_t bits = g.rows[row];
        for (int cx = 0; cx < 3; ++cx) {
            if (!(bits & (1 << (2 - cx)))) continue;
            FillRect(img,
                     x + cx * scale, y + row * scale,
                     x + cx * scale + scale, y + row * scale + scale,
                     col);
        }
    }
}

static void DrawText3x5(Image& img, int x, int y, int scale,
                        const std::string& s, Rgba col) {
    // 3 px glyph + 1 px gap per char.
    const int char_w = 3 * scale + scale;
    for (size_t k = 0; k < s.size(); ++k) {
        DrawChar3x5(img, x + static_cast<int>(k) * char_w, y, scale,
                    s[k], col);
    }
}

// -------------------- colour helpers --------------------

static std::uint32_t Hash32(const std::string& s) {
    std::uint32_t h = 2166136261u;
    for (char c : s) {
        h ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        h *= 16777619u;
    }
    return h;
}

static Rgba HashAccent(const std::string& title_id) {
    // Pick a hue from a curated palette of accent colours that pair
    // well with the dark navy base; deliberately NOT random rainbow
    // which the previous version produced.
    static const Rgba kPalette[] = {
        {  0, 168, 252, 255},   // PS5 cyan
        { 90, 200, 255, 255},   // sky blue
        {255, 138,  61, 255},   // amber
        {220,  60, 120, 255},   // magenta
        {120, 220, 110, 255},   // green
        {200, 130, 255, 255},   // violet
    };
    const std::uint32_t h = Hash32(title_id);
    return kPalette[h % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

static Rgba LerpRgba(Rgba a, Rgba b, float t) {
    auto m = [&](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(x * (1.0f - t) + y * t);
    };
    return Rgba{m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), 255};
}

// -------------------- the actual cover --------------------

static int MakeCover(const std::string& title_id,
                     const std::string& out_path) {
    Image img;

    // 1) Vertical dark gradient: top = deep navy, bottom = black.
    //    Two-stop is plenty and avoids the cheap "rainbow" look of the
    //    hash-derived HSV version.
    const Rgba top = { 18,  28,  48, 255};
    const Rgba bot = {  6,  10,  18, 255};
    for (int y = 0; y < kH; ++y) {
        const float t = static_cast<float>(y) / static_cast<float>(kH - 1);
        const Rgba c = LerpRgba(top, bot, t);
        FillRect(img, 0, y, kW, y + 1, c);
    }

    // 2) Subtle radial vignette light spot near the top so the cover
    //    does not feel flat.  Implemented as a soft circle.
    Rgba glow = HashAccent(title_id);
    glow.a = 70;          // very soft
    FillCircle(img, kW * 0.5f, kH * 0.18f, kW * 0.55f, glow);

    // 3) Decorative concentric arc rings in the lower half.  Two thin
    //    cyan rings hint at a disc / disc-less motif without looking
    //    like the broken diagonal stripes from the previous version.
    const Rgba ring = {  0, 168, 252, 90};
    StrokeCircle(img, kW * 0.5f, kH * 0.78f, 170.0f, 2.0f, ring);
    StrokeCircle(img, kW * 0.5f, kH * 0.78f, 210.0f, 1.5f,
                 Rgba{0, 168, 252, 60});
    StrokeCircle(img, kW * 0.5f, kH * 0.78f, 250.0f, 1.5f,
                 Rgba{0, 168, 252, 35});

    // 4) Central PS-circle motif (filled disc with a soft accent dot).
    const float cx = kW * 0.5f;
    const float cy = kH * 0.46f;
    Rgba accent = HashAccent(title_id);
    // Outer ring
    StrokeCircle(img, cx, cy, 110.0f, 3.0f, accent);
    // Inner disc with a vertical gradient feel using two stops
    FillCircle(img, cx, cy - 8.0f,  90.0f,
               Rgba{static_cast<std::uint8_t>(accent.r / 2),
                    static_cast<std::uint8_t>(accent.g / 2),
                    static_cast<std::uint8_t>(accent.b / 2), 255});
    FillCircle(img, cx, cy + 12.0f, 90.0f,
               Rgba{static_cast<std::uint8_t>(accent.r * 3 / 4),
                    static_cast<std::uint8_t>(accent.g * 3 / 4),
                    static_cast<std::uint8_t>(accent.b * 3 / 4), 255});
    // A clean cyan dot in the middle.
    FillCircle(img, cx, cy, 6.0f, Rgba{255, 255, 255, 230});

    // 5) "PS5" badge in the top-left corner.  Drawn from primitives
    //    so it is always crisp and never depends on a bitmap font.
    const float badge_h = 56.0f;
    const float badge_w = badge_h * 2.0f;
    // Rounded background pill (slightly darker than the page).
    FillRoundRect(img, 24.0f, 24.0f, 24.0f + badge_w, 24.0f + badge_h,
                  10.0f, Rgba{8, 12, 22, 220});
    StrokeRoundRect(img, 24.0f, 24.0f, 24.0f + badge_w, 24.0f + badge_h,
                    10.0f, 1.5f, Rgba{0, 168, 252, 220});
    DrawPS5Badge(img, 24.0f + 12.0f, 24.0f + (badge_h - 36.0f) * 0.5f, 36.0f);

    // 6) Title-id strip at the bottom: filled rounded rect + the title
    //    id rendered with the minimal 3x5 font (legible at 512px and
    //    still crisp when downscaled to ~160px in the UI).
    const float strip_h = 56.0f;
    FillRoundRect(img, 24.0f, kH - strip_h - 24.0f,
                  kW - 24.0f, kH - 24.0f, 12.0f,
                  Rgba{10, 14, 26, 200});
    StrokeRoundRect(img, 24.0f, kH - strip_h - 24.0f,
                    kW - 24.0f, kH - 24.0f, 12.0f, 1.5f,
                    Rgba{255, 255, 255, 30});
    // A small accent square on the left of the strip.
    FillRoundRect(img, 40.0f, kH - strip_h - 8.0f, 56.0f, kH - 32.0f,
                  4.0f, accent);
    // Title id text.  3x5 font at scale 4: 3*4 = 12 px tall, 9 chars
    // * 16 px (3+1 * 4) = 144 px wide.  Center it horizontally to the
    // right of the accent square.
    const int text_scale = 4;
    const int char_w_px  = 3 * text_scale + text_scale;  // 16 px
    const int text_w_px  = char_w_px * static_cast<int>(title_id.size())
                           - text_scale;
    const int text_x     = (kW - text_w_px) / 2;
    const int text_y     = static_cast<int>(kH - strip_h - 24.0f)
                           + (static_cast<int>(strip_h) - 5 * text_scale) / 2;
    DrawText3x5(img, text_x, text_y, text_scale, title_id,
                Rgba{232, 238, 244, 235});

    // 7) Outer rounded border for the whole cover, gives it a card feel.
    StrokeRoundRect(img, 1.0f, 1.0f, kW - 1.0f, kH - 1.0f, 18.0f, 1.0f,
                    Rgba{255, 255, 255, 25});

    // Write PNG.
    std::vector<std::uint8_t> raw(kW * kH * 4);
    for (int i = 0; i < kW * kH; ++i) {
        raw[i * 4 + 0] = img.px[i].r;
        raw[i * 4 + 1] = img.px[i].g;
        raw[i * 4 + 2] = img.px[i].b;
        raw[i * 4 + 3] = img.px[i].a;
    }
    if (!stbi_write_png(out_path.c_str(), kW, kH, 4, raw.data(), kW * 4)) {
        std::fprintf(stderr, "Failed to write %s\n", out_path.c_str());
        return 1;
    }
    return 0;
}

}  // namespace

int main() {
    const std::vector<std::string> titles = {
        "PPSA01668", "PPSA02929", "PPSA10112", "PPSA23885"
    };
    std::filesystem::create_directories("./Covers");
    int fails = 0;
    for (const auto& t : titles) {
        const std::string out = "./Covers/" + t + ".png";
        std::printf("Generate %s -> %s\n", t.c_str(), out.c_str());
        if (MakeCover(t, out) != 0) ++fails;
    }
    if (fails) {
        std::fprintf(stderr, "%d failures\n", fails);
        return 1;
    }
    std::printf("All covers generated.\n");
    return 0;
}
