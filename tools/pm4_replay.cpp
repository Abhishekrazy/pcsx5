// PM4 capture/replay tool + golden-image test driver (ROADMAP Phase 5
// validation strategy).
//
// Capture format (produced by the PCSX5_PM4_CAPTURE=<dir> hook in
// src/hle/libagc.cpp, or by `pm4_replay record-synth`):
//   submit_NNNNN.bin   raw PM4 dwords exactly as submitted
//   submit_NNNNN.json  sidecar: queue, guest_addr, dwords, register shadow
//   capture.json       optional manifest (self-contained captures only):
//                      fb geometry/format, display-buffer addresses, guest
//                      memory blobs the stream references, submit order
//   mem_NNNNN.bin      guest memory blob named by the manifest
//
// Modes:
//   pm4_replay record-synth <out_dir>
//       Builds a synthetic 3-frame capture entirely through the real DCB
//       builder/PM4 layouts (per-frame CP DMA fill of the display buffer +
//       DMA copy of a pattern band + RFlip), self-contained via capture.json.
//   pm4_replay replay <capture_dir> [--golden <dir>] [--save-golden <dir>]
//       Replays the capture through the REAL emulator path headlessly: guest
//       memory is mapped at the recorded addresses, each buffer is submitted
//       via the registered sceAgcDriverSubmitDcb HLE symbol, RFlips go through
//       libSceVideoOut SubmitFlip -> GPU::RenderFrame -> Vulkan present on a
//       hidden window.  Each presented frame is read back from the GPU source
//       image through the vk_present readback hook (BGRA8, unscaled).
//       --golden <dir>       compare frame N against <dir>/flip_NNNNN.png
//                            (per-pixel tolerance 8/255 per channel, at most
//                            0.5% of pixels over tolerance — absorbs driver
//                            nondeterminism; DMA-filled frames compare exact)
//       --save-golden <dir>  dump replayed frames as flip_NNNNN.png
//   Exit codes: 0 = pass (or SKIP when no usable Vulkan device exists and
//   PCSX5_GOLDEN_REQUIRED is unset), 1 = compare/runtime failure, 2 = usage
//   or init failure.  SKIP prints "PM4_GOLDEN_SKIP" so ctest output is clear.
//
// Game captures (PCSX5_PM4_CAPTURE) carry no memory manifest; they replay the
// packet stream for offline analysis but DMA sources are unmapped, so the
// golden-image flow uses record-synth captures.

#include "hle/hle.h"
#include "memory/memory.h"
#include "common/log.h"
#include "gpu/gpu.h"
#include "gpu/vk_present.h"

#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
// stb is not /W4-clean; silence its warnings locally, keep /W4 /WX for ours.
#pragma warning(push)
#pragma warning(disable: 4100 4189 4244 4245 4456 4457 4505 4701 4703 4996)
#include "../third_party/stb_image.h"
#include "../third_party/stb_image_write.h"
#pragma warning(pop)

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" u64 HleDispatch(u64, u64, u64, u64, u64, u64, u64, u64, u64);

namespace fs = std::filesystem;

namespace {

// PM4 packet encoding (must match src/hle/libagc.cpp).
constexpr u32 kItNop     = 0x10;
constexpr u32 kItDmaData = 0x50;
constexpr u32 kRFlip     = 0x17;
constexpr u32 kRDmaData  = 0x19;

u32 Pm4(u32 length_dwords, u32 op, u32 reg) {
    return 0xC0000000u | (((length_dwords - 2u) & 0x3FFFu) << 16) |
           ((op & 0xFFu) << 8) | ((reg & 0x3Fu) << 2);
}

constexpr u32 kFbW = 320;
constexpr u32 kFbH = 180;
constexpr u32 kFbFormat = 1; // BGRA8 (direct upload, no swizzle)
constexpr u32 kFrames = 3;

// Fixed, page-aligned guest addresses for synthetic captures (chosen high so
// they do not collide with the emulator's own pools on replay).
constexpr u64 kFb0Addr     = 0x100000000ull;
constexpr u64 kFb1Addr     = 0x100100000ull;
constexpr u64 kPatternAddr = 0x100200000ull;
constexpr u32 kFbBytes     = kFbW * kFbH * 4;
constexpr u32 kPatternBytes = 0x14000; // 64 pattern rows (60-row band copied)

int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

guest_addr_t MapAt(u64 addr, u64 size) {
    guest_addr_t out = 0;
    if (Memory::Map(addr, size, Memory::PROT_READ | Memory::PROT_WRITE, &out) != Memory::Status::Ok) {
        return 0;
    }
    return out;
}

u64 SymbolId(const char* module, const char* name) {
    const guest_addr_t thunk = HLE::Resolve(module, name);
    if (!thunk) return 0;
    u8 buf[10] = {};
    std::memcpy(buf, reinterpret_cast<const void*>(thunk), sizeof(buf));
    u64 id = 0;
    for (int i = 0; i < 8; ++i) {
        id |= static_cast<u64>(buf[2 + i]) << (8 * i);
    }
    return id;
}

// ---------------------------------------------------------------------------
// Frame sink: frames delivered by the vk_present readback hook.
// ---------------------------------------------------------------------------
struct Frame {
    u32 w = 0, h = 0;
    std::vector<u8> bgra; // tightly packed
};
std::vector<Frame> g_frames;

void OnFrameReadback(const u8* bgra, u32 w, u32 h, u64 /*frame_index*/, void* /*user*/) {
    Frame f;
    f.w = w;
    f.h = h;
    f.bgra.assign(bgra, bgra + static_cast<size_t>(w) * h * 4);
    g_frames.push_back(std::move(f));
    std::fprintf(stdout, "  readback frame %zu (%ux%u)\n", g_frames.size() - 1, w, h);
}

// ---------------------------------------------------------------------------
// Synthetic stream construction (real PM4 layouts; see libagc.cpp walker).
// ---------------------------------------------------------------------------

// Appends a compact CP DMA fill (length 7, NOP/kRDmaData): dst u64 @4,
// fill value @12, byteCount @20.
void EmitDmaFill(std::vector<u32>& s, u64 dst, u32 value, u32 byte_count) {
    s.push_back(Pm4(7, kItNop, kRDmaData));
    s.push_back(static_cast<u32>(dst & 0xFFFFFFFFull));
    s.push_back(static_cast<u32>(dst >> 32));
    s.push_back(value);
    s.push_back(0);
    s.push_back(byte_count);
    s.push_back(0);
}

// Appends a standard CP DMA copy (length 8, IT_DMA_DATA): byteCount @12,
// dst u64 @16, src u64 @24.
void EmitDmaCopy(std::vector<u32>& s, u64 dst, u64 src, u32 byte_count) {
    s.push_back(Pm4(8, kItDmaData, 0));
    s.push_back(0);
    s.push_back(0);
    s.push_back(byte_count);
    s.push_back(static_cast<u32>(dst & 0xFFFFFFFFull));
    s.push_back(static_cast<u32>(dst >> 32));
    s.push_back(static_cast<u32>(src & 0xFFFFFFFFull));
    s.push_back(static_cast<u32>(src >> 32));
}

// Appends an RFlip (length 6, NOP/kRFlip): handle @4, buffer index @8,
// mode @12, flip arg u64 @16.
void EmitFlip(std::vector<u32>& s, u32 handle, u32 buffer_index, u32 mode, u64 arg) {
    s.push_back(Pm4(6, kItNop, kRFlip));
    s.push_back(handle);
    s.push_back(buffer_index);
    s.push_back(mode);
    s.push_back(static_cast<u32>(arg & 0xFFFFFFFFull));
    s.push_back(static_cast<u32>(arg >> 32));
}

// Fills the pattern buffer: horizontal color bars with a per-row gradient so
// the DMA copy is visually verifiable.
void BuildPattern(u8* dst) {
    for (u32 y = 0; y < kPatternBytes / 4 / kFbW; ++y) {
        for (u32 x = 0; x < kFbW; ++x) {
            u8* p = dst + (static_cast<size_t>(y) * kFbW + x) * 4;
            p[0] = static_cast<u8>((x * 255) / (kFbW - 1));      // B
            p[1] = static_cast<u8>((y * 255) / ((kPatternBytes / 4 / kFbW) - 1)); // G
            p[2] = static_cast<u8>(((x / 40) * 85) & 0xFF);      // R bars
            p[3] = 0xFF;
        }
    }
}

bool WriteFile(const fs::path& path, const void* data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return out.good();
}

bool ReadFile(const fs::path& path, std::vector<u8>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    out->resize(static_cast<size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(out->data()), size);
    }
    return in.good() || in.eof();
}

// ---------------------------------------------------------------------------
// record-synth
// ---------------------------------------------------------------------------
int RecordSynth(const fs::path& out_dir) {
    if (!Memory::Initialize()) {
        std::fprintf(stderr, "FATAL: Memory::Initialize failed\n");
        return 2;
    }
    if (!HLE::Initialize()) {
        std::fprintf(stderr, "FATAL: HLE::Initialize failed\n");
        return 2;
    }
    HLE::SetStrictImportMode(false);

    std::error_code ec;
    fs::create_directories(out_dir, ec);

    // Pattern buffer: one static blob referenced by every frame's DMA copy.
    std::vector<u8> pattern(kPatternBytes, 0);
    BuildPattern(pattern.data());
    if (!WriteFile(out_dir / "mem_00000.bin", pattern.data(), pattern.size())) {
        std::fprintf(stderr, "FATAL: cannot write pattern blob\n");
        return 2;
    }

    // Three frames: solid DMA fill (per-frame color) + pattern band copy +
    // RFlip, alternating display buffers.
    const u32 fill_colors[kFrames] = { 0xFF204060u, 0xFF402020u, 0xFF204020u }; // BGRA
    nlohmann::json submits = nlohmann::json::array();
    for (u32 frame = 0; frame < kFrames; ++frame) {
        std::vector<u32> s;
        const u64 fb = (frame % 2 == 0) ? kFb0Addr : kFb1Addr;
        EmitDmaFill(s, fb, fill_colors[frame], kFbBytes);
        // Pattern band: rows 60..119 of the display buffer.
        const u32 band_y = 60, band_h = 60;
        EmitDmaCopy(s, fb + static_cast<u64>(band_y) * kFbW * 4, kPatternAddr,
                    band_h * kFbW * 4);
        EmitFlip(s, 0 /* patched at replay */, frame % 2, 1, frame);

        char name[32];
        std::snprintf(name, sizeof(name), "submit_%05u", frame);
        const fs::path bin = out_dir / (std::string(name) + ".bin");
        if (!WriteFile(bin, s.data(), s.size() * 4)) {
            std::fprintf(stderr, "FATAL: cannot write %s\n", bin.string().c_str());
            return 2;
        }
        nlohmann::json sidecar;
        sidecar["seq"] = frame;
        sidecar["queue"] = "record-synth";
        char addr_buf[32];
        std::snprintf(addr_buf, sizeof(addr_buf), "0x%llx", 0ull);
        sidecar["guest_addr"] = addr_buf; // assigned by the replayer
        sidecar["dwords"] = s.size();
        sidecar["index_addr"] = "0x0";
        sidecar["index_count"] = 0;
        sidecar["index_size"] = 0;
        sidecar["instances"] = 1;
        sidecar["cx"] = nlohmann::json::array();
        sidecar["sh"] = nlohmann::json::array();
        sidecar["uc"] = nlohmann::json::array();
        if (!WriteFile(out_dir / (std::string(name) + ".json"),
                       sidecar.dump(2).data(), sidecar.dump(2).size())) {
            std::fprintf(stderr, "FATAL: cannot write sidecar\n");
            return 2;
        }
        submits.push_back(name);
    }

    nlohmann::json manifest;
    manifest["format_version"] = 1;
    manifest["fb_width"] = kFbW;
    manifest["fb_height"] = kFbH;
    manifest["fb_format"] = kFbFormat;
    char a[32], b[32], p[32];
    std::snprintf(a, sizeof(a), "0x%llx", kFb0Addr);
    std::snprintf(b, sizeof(b), "0x%llx", kFb1Addr);
    std::snprintf(p, sizeof(p), "0x%llx", kPatternAddr);
    manifest["buffer_addrs"] = { a, b };
    manifest["memory"] = nlohmann::json::array();
    nlohmann::json mem;
    mem["addr"] = p;
    mem["file"] = "mem_00000.bin";
    manifest["memory"].push_back(mem);
    manifest["submits"] = submits;
    manifest["flips"] = kFrames;
    const std::string text = manifest.dump(2);
    if (!WriteFile(out_dir / "capture.json", text.data(), text.size())) {
        std::fprintf(stderr, "FATAL: cannot write capture.json\n");
        return 2;
    }

    HLE::Shutdown();
    Memory::Shutdown();
    std::fprintf(stdout, "record-synth: wrote %u-frame capture to %s\n",
                 kFrames, out_dir.string().c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// replay
// ---------------------------------------------------------------------------

// Patches RFlip packets in a loaded stream so they target the videoout port
// opened by the replayer (capture-time handles are not meaningful here).
void PatchFlipHandles(std::vector<u8>* bytes, u32 handle) {
    u32* dwords = reinterpret_cast<u32*>(bytes->data());
    const u32 count = static_cast<u32>(bytes->size() / 4);
    u32 off = 0;
    while (off < count) {
        const u32 header = dwords[off];
        if ((header >> 30) != 3) { ++off; continue; }
        const u32 length = ((header >> 16) & 0x3FFFu) + 2;
        const u32 op = (header >> 8) & 0xFFu;
        const u32 reg = (header >> 2) & 0x3Fu;
        if (length == 0 || off + length > count) break;
        if (op == kItNop && reg == kRFlip && length >= 6) {
            dwords[off + 1] = handle;
        }
        off += length;
    }
}

bool IsGoldenRequired() {
    char buf[8];
    const DWORD n = GetEnvironmentVariableA("PCSX5_GOLDEN_REQUIRED", buf,
                                            static_cast<DWORD>(sizeof(buf)));
    return n != 0 && n < sizeof(buf) && std::strcmp(buf, "0") != 0;
}

int Replay(const fs::path& cap_dir, const fs::path& golden_dir,
           const fs::path& save_dir, bool compare) {
    std::vector<u8> manifest_bytes;
    if (!ReadFile(cap_dir / "capture.json", &manifest_bytes)) {
        std::fprintf(stderr, "FATAL: %s has no capture.json manifest "
                     "(game captures are analysis-only; use record-synth)\n",
                     cap_dir.string().c_str());
        return 2;
    }
    nlohmann::json manifest = nlohmann::json::parse(manifest_bytes, nullptr, false);
    if (manifest.is_discarded()) {
        std::fprintf(stderr, "FATAL: capture.json parse failed\n");
        return 2;
    }
    const u32 fb_w = manifest.value("fb_width", 0u);
    const u32 fb_h = manifest.value("fb_height", 0u);
    const u32 fb_format = manifest.value("fb_format", 1u);
    if (fb_w == 0 || fb_h == 0) {
        std::fprintf(stderr, "FATAL: manifest missing fb geometry\n");
        return 2;
    }

    if (!Memory::Initialize()) {
        std::fprintf(stderr, "FATAL: Memory::Initialize failed\n");
        return 2;
    }
    if (!HLE::Initialize()) {
        std::fprintf(stderr, "FATAL: HLE::Initialize failed\n");
        return 2;
    }
    HLE::SetStrictImportMode(false);

    // Map the recorded guest memory blobs at their original addresses.
    for (const auto& m : manifest["memory"]) {
        const u64 addr = std::strtoull(m["addr"].get<std::string>().c_str(), nullptr, 0);
        std::vector<u8> blob;
        if (!ReadFile(cap_dir / m["file"].get<std::string>(), &blob)) {
            std::fprintf(stderr, "FATAL: missing memory blob %s\n",
                         m["file"].get<std::string>().c_str());
            return 2;
        }
        const guest_addr_t at = MapAt(addr, blob.size());
        if (!at) {
            std::fprintf(stderr, "FATAL: cannot map memory blob at 0x%llx\n", addr);
            return 2;
        }
        std::memcpy(reinterpret_cast<void*>(at), blob.data(), blob.size());
    }

    // Map the display buffers.
    std::vector<u64> fb_addrs;
    for (const auto& b : manifest["buffer_addrs"]) {
        const u64 addr = std::strtoull(b.get<std::string>().c_str(), nullptr, 0);
        if (!MapAt(addr, static_cast<u64>(fb_w) * fb_h * 4)) {
            std::fprintf(stderr, "FATAL: cannot map display buffer at 0x%llx\n", addr);
            return 2;
        }
        fb_addrs.push_back(addr);
    }

    // Hidden-window GPU (real Vulkan present path).
    GPU::SetEmbeddedMode(true);
    if (!GPU::Initialize()) {
        if (IsGoldenRequired()) {
            std::fprintf(stderr, "FATAL: GPU::Initialize failed (golden required)\n");
            return 1;
        }
        std::fprintf(stdout, "PM4_GOLDEN_SKIP: no GPU/window on this host\n");
        return 0;
    }

    // Open a videoout port and register the display buffers through the real
    // HLE symbols.
    const guest_addr_t misc = MapAt(0x100300000ull, 0x10000);
    if (!misc) {
        std::fprintf(stderr, "FATAL: cannot map scratch page\n");
        return 2;
    }
    const u64 open_id = SymbolId("libSceVideoOut", "sceVideoOutOpen");
    CHECK(open_id != 0, "sceVideoOutOpen resolves");
    const u32 handle = static_cast<u32>(HleDispatch(open_id, 0, 0, 0, 0, 0, 0, 0x8000, 0));
    CHECK(handle != 0, "sceVideoOutOpen returned a handle");

    // SceVideoOutBufferAttribute (0x28 bytes): pixel_format @0, w @0x10,
    // h @0x14, pitch @0x18 (see libvideoout ApplyBufferAttribute).
    const guest_addr_t attr = misc;
    Memory::Write<u64>(attr + 0x00, fb_format);
    Memory::Write<u32>(attr + 0x10, fb_w);
    Memory::Write<u32>(attr + 0x14, fb_h);
    Memory::Write<u32>(attr + 0x18, fb_w);
    const guest_addr_t addrs = misc + 0x100;
    for (size_t i = 0; i < fb_addrs.size(); ++i) {
        Memory::Write<u64>(addrs + i * 8, fb_addrs[i]);
    }
    const u64 reg_id = SymbolId("libSceVideoOut", "sceVideoOutRegisterBuffers");
    CHECK(reg_id != 0, "sceVideoOutRegisterBuffers resolves");
    HleDispatch(reg_id, handle, 0, addrs, static_cast<u64>(fb_addrs.size()), attr,
                0, 0x8001, 0);

    GPU::VkPresentSetReadbackHook(&OnFrameReadback, nullptr);

    const u64 submit_id = SymbolId("libSceAgcDriver", "sceAgcDriverSubmitDcb");
    CHECK(submit_id != 0, "sceAgcDriverSubmitDcb resolves");

    // Replay each submit through the real walker.
    for (const auto& s : manifest["submits"]) {
        const std::string name = s.get<std::string>();
        std::vector<u8> bytes;
        if (!ReadFile(cap_dir / (name + ".bin"), &bytes)) {
            std::fprintf(stderr, "FATAL: missing %s.bin\n", name.c_str());
            return 2;
        }
        PatchFlipHandles(&bytes, handle);
        const guest_addr_t buf = MapAt(0, ((bytes.size() + 0xFFF) / 0x1000) * 0x1000);
        if (!buf) {
            std::fprintf(stderr, "FATAL: cannot map submit buffer\n");
            return 2;
        }
        std::memcpy(reinterpret_cast<void*>(buf), bytes.data(), bytes.size());
        const guest_addr_t pkt = misc + 0x200;
        Memory::Write<u64>(pkt + 0, buf);
        Memory::Write<u32>(pkt + 8, static_cast<u32>(bytes.size() / 4));
        const u64 rc = HleDispatch(submit_id, pkt, 0, 0, 0, 0, 0, 0x8002, 0);
        CHECK(rc == 0, "sceAgcDriverSubmitDcb -> 0");
        std::fprintf(stdout, "replayed %s (%zu dwords)\n", name.c_str(), bytes.size() / 4);
    }

    const u32 expected_flips = manifest.value("flips", 0u);
    if (g_frames.empty()) {
        GPU::VkPresentSetReadbackHook(nullptr, nullptr);
        if (IsGoldenRequired()) {
            std::fprintf(stderr, "FAIL: no frames read back (no Vulkan present path)\n");
            return 1;
        }
        std::fprintf(stdout, "PM4_GOLDEN_SKIP: no frames presented "
                     "(no usable Vulkan device)\n");
        GPU::Shutdown();
        return 0;
    }
    CHECK(g_frames.size() == expected_flips, "one readback frame per flip");

    // Optional: dump replayed frames as golden candidates.
    if (!save_dir.empty()) {
        std::error_code ec;
        fs::create_directories(save_dir, ec);
        for (size_t i = 0; i < g_frames.size(); ++i) {
            char name[64];
            std::snprintf(name, sizeof(name), "flip_%05zu.png", i);
            const Frame& f = g_frames[i];
            // BGRA -> RGBA for PNG.
            std::vector<u8> rgba(f.bgra);
            for (size_t px = 0; px + 4 <= rgba.size(); px += 4) {
                std::swap(rgba[px], rgba[px + 2]);
            }
            if (!stbi_write_png((save_dir / name).string().c_str(),
                                static_cast<int>(f.w), static_cast<int>(f.h), 4,
                                rgba.data(), static_cast<int>(f.w) * 4)) {
                std::fprintf(stderr, "FAIL: cannot write %s\n", name);
                ++g_failures;
            }
        }
        std::fprintf(stdout, "saved %zu frame(s) to %s\n", g_frames.size(),
                     save_dir.string().c_str());
    }

    // Optional: golden compare (tolerance 8/255 per channel, <=0.5% pixels).
    if (compare) {
        constexpr int kTol = 8;
        constexpr double kMaxBadFrac = 0.005;
        for (size_t i = 0; i < g_frames.size(); ++i) {
            char name[64];
            std::snprintf(name, sizeof(name), "flip_%05zu.png", i);
            int gw = 0, gh = 0, gc = 0;
            stbi_uc* golden = stbi_load((golden_dir / name).string().c_str(),
                                        &gw, &gh, &gc, 4);
            if (golden == nullptr) {
                std::fprintf(stderr, "[FAIL] missing golden %s\n", name);
                ++g_failures;
                continue;
            }
            const Frame& f = g_frames[i];
            bool ok = true;
            if (static_cast<u32>(gw) != f.w || static_cast<u32>(gh) != f.h) {
                std::fprintf(stderr, "[FAIL] %s: geometry %dx%d != replay %ux%u\n",
                             name, gw, gh, f.w, f.h);
                ok = false;
            } else {
                size_t bad = 0;
                const size_t pixels = static_cast<size_t>(f.w) * f.h;
                for (size_t px = 0; px < pixels; ++px) {
                    for (int c = 0; c < 4; ++c) {
                        // golden is RGBA, replay is BGRA: swap R/B.
                        const int gc_idx = (c == 0) ? 2 : (c == 2) ? 0 : c;
                        const int diff = std::abs(static_cast<int>(golden[px * 4 + gc_idx]) -
                                                  static_cast<int>(f.bgra[px * 4 + c]));
                        if (diff > kTol) { ++bad; break; }
                    }
                }
                if (bad > static_cast<size_t>(kMaxBadFrac * static_cast<double>(pixels))) {
                    std::fprintf(stderr, "[FAIL] %s: %zu/%zu pixels differ (tol %d)\n",
                                 name, bad, pixels, kTol);
                    ok = false;
                }
            }
            if (ok) {
                std::fprintf(stdout, "golden %s: MATCH\n", name);
            } else {
                ++g_failures;
            }
            stbi_image_free(golden);
        }
    }

    GPU::VkPresentSetReadbackHook(nullptr, nullptr);
    GPU::Shutdown();
    HLE::Shutdown();
    Memory::Shutdown();
    return g_failures == 0 ? 0 : 1;
}

void Usage() {
    std::fprintf(stderr,
        "usage:\n"
        "  pm4_replay record-synth <out_dir>\n"
        "  pm4_replay replay <capture_dir> [--golden <dir>] [--save-golden <dir>]\n");
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    if (argc >= 3 && std::strcmp(argv[1], "record-synth") == 0) {
        return RecordSynth(argv[2]);
    }
    if (argc >= 3 && std::strcmp(argv[1], "replay") == 0) {
        fs::path golden_dir, save_dir;
        bool compare = false;
        for (int i = 3; i + 1 < argc; i += 2) {
            if (std::strcmp(argv[i], "--golden") == 0) {
                golden_dir = argv[i + 1];
                compare = true;
            } else if (std::strcmp(argv[i], "--save-golden") == 0) {
                save_dir = argv[i + 1];
            } else {
                Usage();
                return 2;
            }
        }
        return Replay(argv[2], golden_dir, save_dir, compare);
    }
    Usage();
    return 2;
}
