// Disk cache for translated GCN -> SPIR-V modules (Phase 5, H7 part 1).
// See shader_cache.h for the design overview.
#include "shader_cache.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace GPU {
namespace {

// Bump whenever the key composition or the file layout changes; old
// entries become misses and are overwritten on the next store.
constexpr u32 kCacheVersion = 2; // 2: image bindings carry is_arrayed
constexpr u32 kFileMagic    = 0x56353550u; // 'P5SV' little-endian

constexpr u64 kFnvOffsetA = 0xCBF29CE484222325ull;
constexpr u64 kFnvOffsetB = 0x84222325CBF29CE4ull;
constexpr u64 kFnvPrime   = 0x100000001B3ull;

struct KeyHasher {
    u64 lo = kFnvOffsetA;
    u64 hi = kFnvOffsetB;

    void Bytes(const void* data, size_t size) {
        const auto* p = static_cast<const u8*>(data);
        for (size_t i = 0; i < size; ++i) {
            lo = (lo ^ p[i]) * kFnvPrime;
            hi = (hi ^ static_cast<u8>(p[i] + 0x9Du)) * kFnvPrime;
        }
    }

    void U32(u32 v) { Bytes(&v, sizeof(v)); }
    void U64(u64 v) { Bytes(&v, sizeof(v)); }
    void Vec32(const std::vector<u32>& v) {
        U64(static_cast<u64>(v.size()));
        if (!v.empty()) Bytes(v.data(), v.size() * sizeof(u32));
    }
};

std::string EntryPath(const std::string& directory,
                      const GcnShaderCacheKey& key) {
    return directory + "/" + GcnShaderCacheKeyToString(key) + ".spv";
}

} // namespace

std::string GcnShaderCacheKeyToString(const GcnShaderCacheKey& key) {
    char buffer[33];
    std::snprintf(buffer, sizeof(buffer), "%016llX%016llX",
                  static_cast<unsigned long long>(key.hi),
                  static_cast<unsigned long long>(key.lo));
    return buffer;
}

GcnShaderCacheKey GcnShaderCacheComputeKey(
    const void*                        gcn_words,
    size_t                             gcn_word_count,
    const Shader::GcnTranslateOptions& options) {
    using namespace Shader;
    KeyHasher h;
    h.U32(kCacheVersion);

    // Guest shader bytes.
    h.U64(static_cast<u64>(gcn_word_count));
    if (gcn_word_count != 0) {
        h.Bytes(gcn_words, gcn_word_count * sizeof(u32));
    }

    // Every GcnTranslateOptions field that influences the emitted module.
    h.U32(options.stage == GcnSpirvStage::Pixel ? 1u : 0u);
    h.Vec32(options.initial_scalar_registers);
    h.U32(static_cast<u32>(options.initial_scalar_buffer_index));
    for (const GcnSpirvImageBinding& binding : options.image_bindings) {
        h.U32(binding.pc);
        h.U32(binding.mip_level);
        h.U32(binding.is_storage ? 1u : 0u);
        h.U32(binding.is_arrayed ? 1u : 0u);
        h.U32(static_cast<u32>(binding.component_kind));
    }
    for (const GcnSpirvBufferBinding& binding : options.buffer_bindings) {
        h.U32(binding.scalar_address);
        h.Vec32(binding.instruction_pcs);
    }
    for (const GcnPixelOutputBinding& output : options.pixel_outputs) {
        h.U32(output.guest_slot);
        h.U32(output.host_location);
        h.U32(static_cast<u32>(output.kind));
    }
    h.U32(static_cast<u32>(options.required_vertex_output_count));
    h.U32(options.pixel_input_enable);
    h.U32(options.pixel_input_address);
    h.U32(options.max_dispatcher_steps);

    return GcnShaderCacheKey{h.lo, h.hi};
}

GcnShaderDiskCache::GcnShaderDiskCache(std::string directory)
    : directory_(std::move(directory)) {}

bool GcnShaderDiskCache::TryLoad(const GcnShaderCacheKey& key,
                                 std::vector<u32>&        out) {
    const std::string path = EntryPath(directory_, key);
    // One mutex per cache instance would serialize disk IO; use a static
    // process-wide lock instead so distinct cache dirs still don't race a
    // tmp/rename against a reader on the same path (tests hammer this).
    static std::mutex io_mutex;
    std::lock_guard<std::mutex> lock(io_mutex);

    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < 12) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    u32 header[3] = {};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!file || header[0] != kFileMagic || header[1] != kCacheVersion ||
        size != 12 + static_cast<u64>(header[2]) * sizeof(u32)) {
        file.close();
        // Corrupt or stale-format entry: drop it so a later store wins.
        std::filesystem::remove(path, ec);
        return false;
    }
    std::vector<u32> words(header[2]);
    if (!words.empty()) {
        file.read(reinterpret_cast<char*>(words.data()),
                  static_cast<std::streamsize>(words.size() * sizeof(u32)));
    }
    file.close();
    if (!words.empty() && !file) {
        std::filesystem::remove(path, ec);
        return false;
    }
    out = std::move(words);
    return true;
}

bool GcnShaderDiskCache::Store(const GcnShaderCacheKey&    key,
                               const std::vector<u32>&     words) {
    static std::mutex io_mutex;
    std::lock_guard<std::mutex> lock(io_mutex);

    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec) {
        return false;
    }
    const std::string path = EntryPath(directory_, key);
    const std::string tmp  = path + ".tmp";

    bool ok = false;
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (file) {
            const u32 header[3] = {kFileMagic, kCacheVersion,
                                   static_cast<u32>(words.size())};
            file.write(reinterpret_cast<const char*>(header), sizeof(header));
            if (!words.empty()) {
                file.write(reinterpret_cast<const char*>(words.data()),
                           static_cast<std::streamsize>(words.size() *
                                                        sizeof(u32)));
            }
            file.flush();
            ok = file.good();
        }
    }
    if (!ok) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    // Atomic publish.  rename() fails on Windows if the target exists, so
    // remove first — readers either see the old complete file or a miss,
    // never a partial one (the tmp file carries the new bytes throughout).
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::future<size_t> GcnShaderCacheWarmupAsync(
    GcnShaderDiskCache*                  cache,
    std::vector<GcnShaderCacheWarmupJob> jobs) {
    return std::async(std::launch::async,
                      [cache, jobs = std::move(jobs)]() mutable -> size_t {
        size_t stored = 0;
        for (GcnShaderCacheWarmupJob& job : jobs) {
            const std::vector<u32> words =
                Shader::GcnProgramFlattenWords(job.program);
            const GcnShaderCacheKey key = GcnShaderCacheComputeKey(
                words.data(), words.size(), job.options);
            std::vector<u32> spirv;
            if (cache->TryLoad(key, spirv)) {
                continue; // already warm
            }
            Shader::GcnSpirvShader shader;
            std::string            error;
            if (Shader::GcnTranslateToSpirv(job.program, job.options, shader,
                                            error) &&
                cache->Store(key, shader.words)) {
                ++stored;
            }
        }
        return stored;
    });
}

} // namespace GPU
