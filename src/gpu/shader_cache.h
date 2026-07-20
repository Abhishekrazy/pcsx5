// Disk cache for translated GCN -> SPIR-V modules (Phase 5, H7 part 1).
//
// The draw path (libagc) translates guest shaders with
// GcnTranslateToSpirv on every boot; translations are deterministic in
// (GCN shader bytes, GcnTranslateOptions), so the emitted SPIR-V can be
// persisted across runs.  This header provides:
//
//   * GcnShaderCacheKey / GcnShaderCacheComputeKey — a 128-bit content
//     key over the shader bytes plus every GcnTranslateOptions field that
//     influences the emitted module (buffer-binding layout, image/sampler
//     pcs, initial-scalar-binding index, stage interface, ...).
//   * GcnShaderDiskCache — thread-safe store/load under a cache dir
//     (default "Cache/Shaders", alongside the existing Cache/ patterns).
//     Writes are atomic (tmp file + rename) and loads are
//     corruption-tolerant (magic + version + size header).
//   * GcnShaderCacheWarmupAsync — async pipeline-compilation prep: a
//     worker thread that pre-populates the disk cache from a list of
//     (program, options) jobs.  Actual asynchronous VkPipeline creation
//     belongs to vk_draw and is deferred (see docs note in the .cpp).
//
// Callers looking for a translate-with-cache entry point should use
// GcnTranslateWithCache (gcn_translate.h), which composes these pieces.
#pragma once

#include "../common/types.h"
#include "shader/gcn_translate.h"

#include <future>
#include <string>
#include <vector>

namespace GPU {

// 128-bit cache key (two 64-bit FNV-1a passes with distinct seeds).
struct GcnShaderCacheKey {
    u64 lo = 0;
    u64 hi = 0;

    bool operator==(const GcnShaderCacheKey&) const = default;
};

// Computes the cache key for a translation: FNV-1a over a canonical byte
// stream of (cache format version, GCN shader dword stream, every
// GcnTranslateOptions field that affects the emitted module).  Two inputs
// with different keys never collide in practice; equal inputs always
// produce equal keys.
GcnShaderCacheKey GcnShaderCacheComputeKey(
    const void*                          gcn_words,
    size_t                               gcn_word_count,
    const Shader::GcnTranslateOptions&   options);

// Hex filename form ("<hi:016x><lo:016x>") used for the on-disk entry.
std::string GcnShaderCacheKeyToString(const GcnShaderCacheKey& key);

// Thread-safe disk cache of SPIR-V word streams, one file per key under
// `directory` (created on first store).  File layout:
//   u32 magic ('P5SV'), u32 format version, u32 word count, u32 words[].
// A file failing any header/size check is treated as a miss (and removed),
// never as an error.
class GcnShaderDiskCache {
public:
    // `directory` is e.g. "Cache/Shaders".  It is created lazily on Store.
    explicit GcnShaderDiskCache(std::string directory);

    // Loads the cached SPIR-V for `key` into `out`.  Returns false on miss
    // or on any corruption (bad magic/version/size, truncated payload).
    bool TryLoad(const GcnShaderCacheKey& key, std::vector<u32>& out);

    // Persists `words` under `key`.  The write is atomic with respect to
    // concurrent readers (tmp file + rename); concurrent stores of the
    // same key are serialized and idempotent (same input -> same bytes).
    bool Store(const GcnShaderCacheKey& key, const std::vector<u32>& words);

    const std::string& directory() const { return directory_; }

private:
    std::string directory_;
};

// One warm-up job: translate `program` with `options` and store the result
// in the shared disk cache (skipping entries already cached).
struct GcnShaderCacheWarmupJob {
    Shader::GcnProgram          program;
    Shader::GcnTranslateOptions options;
};

// Async pipeline-compilation prep (H7 part 1: cache warm-up only).
//
// Design note: the expensive half of pipeline creation is GCN->SPIR-V
// translation; the VkPipeline half needs the Vulkan device and the render
// pass/format state owned by vk_draw, so async VkPipeline creation is
// deferred to that agent.  What this layer can do today is run translations
// off the critical path so the first draw that needs a shader finds its
// SPIR-V already on disk.  vk_draw's async pipeline worker should consume
// keys produced by GcnShaderCacheComputeKey / GcnTranslateWithCache so the
// disk cache and the (spirv digest, state) pipeline cache stay coherent.
//
// Spawns one worker thread that translates every job whose key is not yet
// cached and stores the results in `cache`.  Returns a future holding the
// number of modules successfully stored (already-cached jobs are skipped,
// not counted).  Translation failures are skipped silently — the draw path
// re-translates synchronously and reports the error there.
std::future<size_t> GcnShaderCacheWarmupAsync(
    GcnShaderDiskCache*                      cache,
    std::vector<GcnShaderCacheWarmupJob>     jobs);

} // namespace GPU
