// I6.3: Golden frame comparison tool.
//
// Compares a captured frame PNG against a reference golden PNG using
// per-pixel diff with configurable tolerance.  Designed to be called from
// the PM4 replay golden-image path or standalone for regression checking.
//
// Build:
//   cl /EHsc /std:c++20 tools/golden_capture.cpp /I third_party /I src
//      /link stb_image.lib (or compile stb_image.c separately)
//
// Usage:
//   golden_capture <reference.png> <capture.png> [--tolerance <val>]
//   golden_capture --save-golden <output.png> <width> <height> <bgra_data>
//
// Exit codes:
//   0 = match (within tolerance)
//   1 = mismatch (exceeds tolerance)
//   2 = usage / file error

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma warning(push)
#pragma warning(disable : 4100 4189 4244 4245 4456 4457 4505 4701 4703 4996)
#include "stb_image.h"
#include "stb_image_write.h"
#pragma warning(pop)

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstdint>

namespace {

// ---------------------------------------------------------------------------
// Per-pixel comparison with configurable tolerance.  Returns the fraction of
// pixels that exceed the per-channel tolerance (0.0 = perfect match).
// ---------------------------------------------------------------------------
double CompareFrames(const uint8_t* ref, const uint8_t* cap,
                     int w, int h, int channels,
                     int per_channel_tolerance, double max_fraction) {
    if (!ref || !cap || w <= 0 || h <= 0) return 1.0;

    const int stride = w * channels;
    int bad_pixels = 0;
    int total_pixels = w * h;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int ri = y * stride + x * channels;
            int exceeding = 0;
            for (int c = 0; c < channels && c < 4; ++c) {
                int diff = abs(static_cast<int>(ref[ri + c]) -
                               static_cast<int>(cap[ri + c]));
                if (diff > per_channel_tolerance) {
                    exceeding++;
                }
            }
            if (exceeding > 2) { // more than 2 channels exceed tolerance
                bad_pixels++;
            }
        }
    }

    double fraction = static_cast<double>(bad_pixels) / total_pixels;
    return fraction;
}

// ---------------------------------------------------------------------------
// Compute a simple perceptual hash (average hash) of an image for quick
// comparison.  Returns 64-bit hash or 0 on error.
// ---------------------------------------------------------------------------
uint64_t ComputeAverageHash(const uint8_t* pixels, int w, int h, int channels) {
    if (!pixels || w <= 0 || h <= 0) return 0;

    // Convert to grayscale and compute average.
    const int size = 8; // 8x8 hash
    const int step_x = w / size;
    const int step_y = h / size;

    double avg = 0.0;
    uint8_t gray[64] = {};
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int px = x * step_x + step_x / 2;
            int py = y * step_y + step_y / 2;
            int idx = (py * w + px) * channels;
            int r = pixels[idx + 0];
            int g = pixels[idx + 1];
            int b = pixels[idx + 2];
            gray[y * size + x] = static_cast<uint8_t>((r * 77 + g * 151 + b * 28) >> 8);
            avg += gray[y * size + x];
        }
    }
    avg /= 64.0;

    // Compute hash bits.
    uint64_t hash = 0;
    for (int i = 0; i < 64; ++i) {
        if (gray[i] > avg) {
            hash |= (1ULL << i);
        }
    }
    return hash;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage:\n"
            "  golden_capture <reference.png> <capture.png> [--tolerance <n>] [--max-fraction <f>]\n"
            "  golden_capture --save-golden <output.png> <width> <height> <bgra_file>\n"
            "  golden_capture --hash <image.png>   (print 64-bit perceptual hash)\n");
        return 2;
    }

    if (std::strcmp(argv[1], "--save-golden") == 0) {
        // --save-golden <output.png> <w> <h> <bgra_file>
        if (argc < 6) { std::fprintf(stderr, "Usage: --save-golden <out.png> <w> <h> <bgra_file>\n"); return 2; }
        const char* out_path = argv[2];
        int w = std::atoi(argv[3]);
        int h = std::atoi(argv[4]);
        const char* data_path = argv[5];

        // Read raw BGRA data from file.
        FILE* f = std::fopen(data_path, "rb");
        if (!f) { std::fprintf(stderr, "ERROR: cannot open %s\n", data_path); return 2; }
        std::fseek(f, 0, SEEK_END);
        long fsize = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        size_t expected = static_cast<size_t>(w) * h * 4;
        if (static_cast<size_t>(fsize) != expected) {
            std::fprintf(stderr, "ERROR: %s size mismatch: got %ld, expected %zu\n",
                         data_path, fsize, expected);
            std::fclose(f);
            return 2;
        }
        auto* data = new uint8_t[expected];
        std::fread(data, 1, expected, f);
        std::fclose(f);

        // Write PNG (stb expects RGBA; our data is BGRA — swap R and B).
        auto* rgba = new uint8_t[expected];
        for (size_t i = 0; i < expected; i += 4) {
            rgba[i + 0] = data[i + 2]; // B -> R
            rgba[i + 1] = data[i + 1]; // G -> G
            rgba[i + 2] = data[i + 0]; // R -> B
            rgba[i + 3] = data[i + 3]; // A -> A
        }

        int ok = stbi_write_png(out_path, w, h, 4, rgba, w * 4);
        delete[] rgba;
        delete[] data;
        if (!ok) { std::fprintf(stderr, "ERROR: write %s failed\n", out_path); return 2; }
        std::fprintf(stdout, "Saved golden: %s (%dx%d)\n", out_path, w, h);
        return 0;
    }

    if (std::strcmp(argv[1], "--hash") == 0) {
        if (argc < 3) { std::fprintf(stderr, "Usage: --hash <image.png>\n"); return 2; }
        int w, h, channels;
        uint8_t* pixels = stbi_load(argv[2], &w, &h, &channels, 4);
        if (!pixels) { std::fprintf(stderr, "ERROR: cannot load %s\n", argv[2]); return 2; }
        uint64_t hash = ComputeAverageHash(pixels, w, h, 4);
        stbi_image_free(pixels);
        std::fprintf(stdout, "0x%016llX\n", static_cast<unsigned long long>(hash));
        return 0;
    }

    // Compare mode: <reference.png> <capture.png> [options]
    if (argc < 3) {
        std::fprintf(stderr, "Usage: golden_capture <ref.png> <cap.png> [--tolerance <n>] [--max-fraction <f>]\n");
        return 2;
    }

    const char* ref_path = argv[1];
    const char* cap_path = argv[2];
    int per_channel_tolerance = 8;    // default: 8/255 per channel
    double max_fraction = 0.005;      // default: 0.5% of pixels

    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tolerance") == 0 && i + 1 < argc) {
            per_channel_tolerance = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--max-fraction") == 0 && i + 1 < argc) {
            max_fraction = std::atof(argv[++i]);
        }
    }

    int ref_w, ref_h, ref_ch;
    uint8_t* ref_pixels = stbi_load(ref_path, &ref_w, &ref_h, &ref_ch, 4);
    if (!ref_pixels) { std::fprintf(stderr, "ERROR: cannot load reference: %s\n", ref_path); return 2; }

    int cap_w, cap_h, cap_ch;
    uint8_t* cap_pixels = stbi_load(cap_path, &cap_w, &cap_h, &cap_ch, 4);
    if (!cap_pixels) { std::fprintf(stderr, "ERROR: cannot load capture: %s\n", cap_path); return 2; }

    // Dimensions must match.
    if (ref_w != cap_w || ref_h != cap_h) {
        std::fprintf(stderr, "ERROR: dimension mismatch: ref=%dx%d cap=%dx%d\n",
                     ref_w, ref_h, cap_w, cap_h);
        stbi_image_free(ref_pixels);
        stbi_image_free(cap_pixels);
        return 1;
    }

    double fraction = CompareFrames(ref_pixels, cap_pixels, ref_w, ref_h, 4,
                                     per_channel_tolerance, max_fraction);
    uint64_t ref_hash = ComputeAverageHash(ref_pixels, ref_w, ref_h, 4);
    uint64_t cap_hash = ComputeAverageHash(cap_pixels, cap_w, cap_h, 4);
    int hamming = 0;
    uint64_t xor_hash = ref_hash ^ cap_hash;
    while (xor_hash) { hamming += xor_hash & 1; xor_hash >>= 1; }

    std::fprintf(stdout,
        "Comparison: %s vs %s\n"
        "  Dimensions:     %dx%d\n"
        "  Bad pixel fraction: %.6f (limit: %.4f)\n"
        "  Hash ref:       0x%016llX\n"
        "  Hash cap:       0x%016llX\n"
        "  Hamming dist:   %d bits\n"
        "  Verdict:        %s\n",
        ref_path, cap_path,
        ref_w, ref_h,
        fraction, max_fraction,
        static_cast<unsigned long long>(ref_hash),
        static_cast<unsigned long long>(cap_hash),
        hamming,
        fraction <= max_fraction ? "PASS" : "FAIL");

    stbi_image_free(ref_pixels);
    stbi_image_free(cap_pixels);

    return fraction <= max_fraction ? 0 : 1;
}
