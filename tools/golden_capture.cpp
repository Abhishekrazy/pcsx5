// I6.3: Golden frame comparison tool.
//
// Compares a captured frame PNG against a reference golden PNG using
// per-pixel diff with configurable tolerance.  Designed to be called from
// the PM4 replay golden-image path or standalone for regression checking.
//
// Also supports perceptual-hash comparison and visual-diff output.
//
// Usage:
//   golden_capture <reference.png> <capture.png> [options]
//   golden_capture --hash <image.png>
//   golden_capture --save-golden <output.png> <width> <height> <bgra_file>
//   golden_capture --diff <ref.png> <cap.png> --output-diff <diff.png>
//
// Options for compare mode:
//   --tolerance <n>       Per-channel tolerance (0-255, default 8)
//   --max-fraction <f>    Max fraction of bad pixels (default 0.005)
//   --hamming-threshold <n>  Max Hamming distance for hash (default 10)
//   --output-diff <path>  Save a visual diff image highlighting differences
//   --hash-only           Compare using perceptual hash, skip pixel diff
//
// Exit codes:
//   0 = match
//   1 = mismatch (exceeds tolerance)
//   2 = usage / file error
//
// Build: standalone, stb_image compiled in.

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
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Per-pixel comparison with configurable tolerance.
//
// Returns the fraction of pixels where at least one channel exceeds the
// per-channel tolerance (0.0 = perfect match).
// ---------------------------------------------------------------------------
double ComparePixels(const uint8_t* ref, const uint8_t* cap,
                     int w, int h, int channels,
                     int per_channel_tolerance) {
    if (!ref || !cap || w <= 0 || h <= 0) return 1.0;

    const int stride = w * channels;
    int bad_pixels = 0;
    const int total_pixels = w * h;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int ri = y * stride + x * channels;
            for (int c = 0; c < channels; ++c) {
                const int diff = abs(static_cast<int>(ref[ri + c]) -
                                     static_cast<int>(cap[ri + c]));
                if (diff > per_channel_tolerance) {
                    ++bad_pixels;
                    break; // count pixel once regardless of how many channels
                }
            }
        }
    }

    return static_cast<double>(bad_pixels) / total_pixels;
}

// ---------------------------------------------------------------------------
// Build a visual diff image: each pixel shows the per-channel absolute
// difference amplified so it's easy to spot.  Returns the same dimensions
// as the inputs.  Channels are always 4 (RGBA).
// ---------------------------------------------------------------------------
std::vector<uint8_t> BuildDiffImage(const uint8_t* ref, const uint8_t* cap,
                                    int w, int h, int channels,
                                    int scale_factor) {
    const int stride = w * channels;
    std::vector<uint8_t> diff(static_cast<size_t>(w) * h * 4, 0);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int ri = y * stride + x * channels;
            const int di = (y * w + x) * 4;
            int max_diff = 0;
            for (int c = 0; c < channels && c < 4; ++c) {
                const int d = abs(static_cast<int>(ref[ri + c]) -
                                  static_cast<int>(cap[ri + c]));
                if (d > max_diff) max_diff = d;
                diff[di + c] = static_cast<uint8_t>(
                    std::min(255, d * scale_factor));
            }
            // Alpha channel: show as opaque with max-diff marker
            if (channels >= 4) {
                const int d = abs(static_cast<int>(ref[ri + 3]) -
                                  static_cast<int>(cap[ri + 3]));
                if (d > max_diff) max_diff = d;
                diff[di + 3] = 255;
            } else {
                diff[di + 3] = 255;
            }
            // Mark pixels that differ significantly with a hot color tint.
            if (max_diff > 8) {
                // Reddish tint for changed pixels.
                diff[di + 0] = static_cast<uint8_t>(
                    std::min(255, diff[di + 0] + 40));
                diff[di + 2] = static_cast<uint8_t>(
                    std::max(0, diff[di + 2] - 20));
            }
        }
    }
    return diff;
}

// ---------------------------------------------------------------------------
// Perceptual hash (average hash) -- 64-bit hash of an 8x8 grayscale
// representation.  Returns 0 on error.
// ---------------------------------------------------------------------------
uint64_t ComputeAverageHash(const uint8_t* pixels, int w, int h, int channels) {
    if (!pixels || w <= 0 || h <= 0) return 0;

    constexpr int kSize = 8;
    const int step_x = std::max(1, w / kSize);
    const int step_y = std::max(1, h / kSize);

    double avg = 0.0;
    uint8_t gray[64] = {};
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            // Sample the center of each tile.
            const int px = std::min(x * step_x + step_x / 2, w - 1);
            const int py = std::min(y * step_y + step_y / 2, h - 1);
            const int idx = (py * w + px) * channels;
            const int r = pixels[idx + 0];
            const int g = pixels[idx + 1];
            const int b = pixels[idx + 2];
            gray[y * kSize + x] = static_cast<uint8_t>(
                (r * 77 + g * 151 + b * 28) >> 8); // BT.601 luminance
            avg += gray[y * kSize + x];
        }
    }
    avg /= 64.0;

    // Compute hash: 1 if pixel above average, 0 if below.
    uint64_t hash = 0;
    for (int i = 0; i < 64; ++i) {
        if (gray[i] > avg) {
            hash |= (1ULL << i);
        }
    }
    return hash;
}

// ---------------------------------------------------------------------------
// Hamming distance between two 64-bit hashes.
// ---------------------------------------------------------------------------
int HammingDistance(uint64_t a, uint64_t b) {
    uint64_t x = a ^ b;
    int dist = 0;
    while (x) {
        dist += x & 1;
        x >>= 1;
    }
    return dist;
}

// ---------------------------------------------------------------------------
// CLI argument helpers.
// ---------------------------------------------------------------------------
const char* FindArg(int argc, char* argv[], const char* short_name,
                    const char* long_name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (short_name && std::strcmp(argv[i], short_name) == 0)
            return argv[i + 1];
        if (long_name && std::strcmp(argv[i], long_name) == 0)
            return argv[i + 1];
    }
    return nullptr;
}

bool HasFlag(int argc, char* argv[], const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return true;
    }
    return false;
}

void PrintUsage() {
    std::fprintf(stderr,
        "Usage:\n"
        "  golden_capture <reference.png> <capture.png> [options]\n"
        "  golden_capture --hash <image.png>\n"
        "  golden_capture --save-golden <output.png> <w> <h> <bgra_file>\n"
        "  golden_capture --diff <ref.png> <cap.png> --output-diff <diff.png>\n"
        "\n"
        "Options:\n"
        "  --tolerance <n>        Per-channel tolerance (0-255, default 8)\n"
        "  --max-fraction <f>     Max bad pixel fraction (default 0.005)\n"
        "  --hamming-threshold <n> Max Hamming distance (default 10)\n"
        "  --output-diff <path>   Save visual diff image\n"
        "  --hash-only            Use hash comparison, skip pixel diff\n"
        "\n"
        "Exit codes: 0 = match, 1 = mismatch, 2 = error\n");
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    if (argc < 2) {
        PrintUsage();
        return 2;
    }

    // -----------------------------------------------------------------------
    // --hash <image.png>
    // -----------------------------------------------------------------------
    if (std::strcmp(argv[1], "--hash") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "ERROR: --hash requires an image path\n");
            return 2;
        }
        int w, h, channels;
        uint8_t* pixels = stbi_load(argv[2], &w, &h, &channels, 4);
        if (!pixels) {
            std::fprintf(stderr, "ERROR: cannot load %s\n", argv[2]);
            return 2;
        }
        const uint64_t hash = ComputeAverageHash(pixels, w, h, 4);
        stbi_image_free(pixels);
        std::fprintf(stdout, "0x%016llX\n",
                     static_cast<unsigned long long>(hash));
        return 0;
    }

    // -----------------------------------------------------------------------
    // --save-golden <output.png> <w> <h> <bgra_file>
    // -----------------------------------------------------------------------
    if (std::strcmp(argv[1], "--save-golden") == 0) {
        if (argc < 6) {
            std::fprintf(stderr,
                "ERROR: --save-golden <out.png> <w> <h> <bgra_file>\n");
            return 2;
        }
        const char* out_path = argv[2];
        const int w = std::atoi(argv[3]);
        const int h = std::atoi(argv[4]);
        const char* data_path = argv[5];

        if (w <= 0 || h <= 0) {
            std::fprintf(stderr, "ERROR: invalid dimensions %dx%d\n", w, h);
            return 2;
        }

        // Read raw BGRA data from file.
        FILE* f = std::fopen(data_path, "rb");
        if (!f) {
            std::fprintf(stderr, "ERROR: cannot open %s\n", data_path);
            return 2;
        }
        std::fseek(f, 0, SEEK_END);
        const long fsize = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        const size_t expected = static_cast<size_t>(w) * h * 4;
        if (static_cast<size_t>(fsize) != expected) {
            std::fprintf(stderr,
                "ERROR: %s size mismatch: got %ld, expected %zu\n",
                data_path, fsize, expected);
            std::fclose(f);
            return 2;
        }
        std::vector<uint8_t> data(expected);
        if (std::fread(data.data(), 1, expected, f) != expected) {
            std::fprintf(stderr, "ERROR: short read from %s\n", data_path);
            std::fclose(f);
            return 2;
        }
        std::fclose(f);

        // BGRA -> RGBA for PNG (swap R and B channels).
        std::vector<uint8_t> rgba(expected);
        for (size_t i = 0; i < expected; i += 4) {
            rgba[i + 0] = data[i + 2]; // B -> R
            rgba[i + 1] = data[i + 1]; // G -> G
            rgba[i + 2] = data[i + 0]; // R -> B
            rgba[i + 3] = data[i + 3]; // A -> A
        }

        if (!stbi_write_png(out_path, w, h, 4, rgba.data(), w * 4)) {
            std::fprintf(stderr, "ERROR: write %s failed\n", out_path);
            return 2;
        }
        std::fprintf(stdout, "Saved golden: %s (%dx%d)\n", out_path, w, h);
        return 0;
    }

    // -----------------------------------------------------------------------
    // --diff <ref.png> <cap.png> --output-diff <diff.png>
    // -----------------------------------------------------------------------
    if (std::strcmp(argv[1], "--diff") == 0) {
        if (argc < 4) {
            std::fprintf(stderr, "ERROR: --diff <ref.png> <cap.png>\n");
            return 2;
        }
        const char* ref_path = argv[2];
        const char* cap_path = argv[3];
        const char* diff_path = FindArg(argc, argv, nullptr, "--output-diff");
        if (!diff_path) {
            std::fprintf(stderr, "ERROR: --diff requires --output-diff <path>\n");
            return 2;
        }

        int ref_w, ref_h, ref_ch;
        uint8_t* ref_pixels = stbi_load(ref_path, &ref_w, &ref_h, &ref_ch, 4);
        if (!ref_pixels) {
            std::fprintf(stderr, "ERROR: cannot load %s\n", ref_path);
            return 2;
        }

        int cap_w, cap_h, cap_ch;
        uint8_t* cap_pixels = stbi_load(cap_path, &cap_w, &cap_h, &cap_ch, 4);
        if (!cap_pixels) {
            std::fprintf(stderr, "ERROR: cannot load %s\n", cap_path);
            stbi_image_free(ref_pixels);
            return 2;
        }

        if (ref_w != cap_w || ref_h != cap_h) {
            std::fprintf(stderr,
                "ERROR: dimension mismatch: ref=%dx%d cap=%dx%d\n",
                ref_w, ref_h, cap_w, cap_h);
            stbi_image_free(ref_pixels);
            stbi_image_free(cap_pixels);
            return 1;
        }

        const auto diff_img = BuildDiffImage(ref_pixels, cap_pixels,
                                             ref_w, ref_h, 4, 4);
        if (!stbi_write_png(diff_path, ref_w, ref_h, 4, diff_img.data(),
                            ref_w * 4)) {
            std::fprintf(stderr, "ERROR: write %s failed\n", diff_path);
            stbi_image_free(ref_pixels);
            stbi_image_free(cap_pixels);
            return 2;
        }

        std::fprintf(stdout, "Diff image saved: %s (%dx%d)\n",
                     diff_path, ref_w, ref_h);
        stbi_image_free(ref_pixels);
        stbi_image_free(cap_pixels);
        return 0;
    }

    // -----------------------------------------------------------------------
    // Compare mode: <reference.png> <capture.png> [options]
    // -----------------------------------------------------------------------
    const char* ref_path = argv[1];
    const char* cap_path = argv[2];

    int per_channel_tolerance = 8;
    double max_fraction = 0.005;
    int hamming_threshold = 10;
    bool hash_only = false;

    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tolerance") == 0 && i + 1 < argc) {
            per_channel_tolerance = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--max-fraction") == 0 && i + 1 < argc) {
            max_fraction = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--hamming-threshold") == 0 && i + 1 < argc) {
            hamming_threshold = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--output-diff") == 0 && i + 1 < argc) {
            ++i; // consumed by FindArg below
        } else if (std::strcmp(argv[i], "--hash-only") == 0) {
            hash_only = true;
        }
    }

    // Find --output-diff separately (it might be anywhere).
    const char* diff_path = FindArg(argc, argv, nullptr, "--output-diff");

    int ref_w, ref_h, ref_ch;
    uint8_t* ref_pixels = stbi_load(ref_path, &ref_w, &ref_h, &ref_ch, 4);
    if (!ref_pixels) {
        std::fprintf(stderr, "ERROR: cannot load reference: %s\n", ref_path);
        return 2;
    }

    int cap_w, cap_h, cap_ch;
    uint8_t* cap_pixels = stbi_load(cap_path, &cap_w, &cap_h, &cap_ch, 4);
    if (!cap_pixels) {
        std::fprintf(stderr, "ERROR: cannot load capture: %s\n", cap_path);
        stbi_image_free(ref_pixels);
        return 2;
    }

    // Dimensions must match for pixel comparison.
    const bool dims_match = (ref_w == cap_w && ref_h == cap_h);
    if (!dims_match && !hash_only) {
        std::fprintf(stderr,
            "ERROR: dimension mismatch: ref=%dx%d cap=%dx%d "
            "(use --hash-only to compare by hash only)\n",
            ref_w, ref_h, cap_w, cap_h);
        stbi_image_free(ref_pixels);
        stbi_image_free(cap_pixels);
        return 1;
    }

    // Compute perceptual hashes of both images.
    const uint64_t ref_hash = ComputeAverageHash(ref_pixels, ref_w, ref_h, 4);
    const uint64_t cap_hash = ComputeAverageHash(cap_pixels, cap_w, cap_h, 4);
    const int hamming = HammingDistance(ref_hash, cap_hash);

    double fraction = 1.0;
    if (!hash_only && dims_match) {
        fraction = ComparePixels(ref_pixels, cap_pixels, ref_w, ref_h, 4,
                                 per_channel_tolerance);
    }

    const bool pixel_pass = hash_only || !dims_match || (fraction <= max_fraction);
    const bool hash_pass  = (hamming <= hamming_threshold);
    const bool match = pixel_pass && hash_pass;

    // Print results.
    std::fprintf(stdout,
        "Comparison: %s vs %s\n"
        "  Dimensions:        ref=%dx%d  cap=%dx%d%s\n"
        "  Pixel diff:        %.6f (limit: %.4f, tol: %d/255) %s\n"
        "  Hash ref:          0x%016llX\n"
        "  Hash cap:          0x%016llX\n"
        "  Hamming distance:  %d (threshold: %d) %s\n"
        "  Verdict:           %s\n",
        ref_path, cap_path,
        ref_w, ref_h, cap_w, cap_h,
        dims_match ? "" : " [HASH-ONLY: dims differ]",
        fraction, max_fraction, per_channel_tolerance,
        pixel_pass ? "(ok)" : "(EXCEEDS)",
        static_cast<unsigned long long>(ref_hash),
        static_cast<unsigned long long>(cap_hash),
        hamming, hamming_threshold,
        hash_pass ? "(ok)" : "(DIFFERENT)",
        match ? "PASS" : "FAIL");

    // Save diff image if requested.
    if (diff_path && dims_match) {
        const auto diff_img = BuildDiffImage(ref_pixels, cap_pixels,
                                             ref_w, ref_h, 4, 4);
        if (!stbi_write_png(diff_path, ref_w, ref_h, 4, diff_img.data(),
                            ref_w * 4)) {
            std::fprintf(stderr, "WARNING: could not write diff: %s\n",
                         diff_path);
        } else {
            std::fprintf(stdout, "  Diff image:        %s\n", diff_path);
        }
    }

    stbi_image_free(ref_pixels);
    stbi_image_free(cap_pixels);

    return match ? 0 : 1;
}
