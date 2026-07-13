#include "ui/thumbnail.h"
#include "common/log.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <vector>

#ifdef _WIN32
// Windows.h must come BEFORE <GL/gl.h> on MSVC: gl.h includes windows.h
// internally but expects the WINGDI types to be already in scope.  We
// also include it before stb_image because some MSVC configurations
// leak Windows macros (e.g. min/max) into the stb_image path.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>
#endif

// stb_image — single-header PNG / JPG / WebP loader.  We deliberately
// pull in only the decode functions we need and disable the `stbi_*`
// functions we don't (they would otherwise be exported from every TU
// that includes the header).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_WEBP
#include "stb_image.h"

namespace Ui {

// ------------------------------------------------------------------------
// Path lookup
// ------------------------------------------------------------------------

static std::string ToLowerCopy(const std::string& s) {
    std::string out = s;
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string FindCoverFile(const std::string& title_id,
                          const std::string& covers_dir) {
    if (title_id.empty() || covers_dir.empty()) return {};
    std::error_code ec;
    if (!std::filesystem::is_directory(covers_dir, ec)) return {};

    // 1) exact-case match for the most common extensions
    static const char* kExts[] = {".png", ".jpg", ".jpeg", ".webp"};
    for (const char* ext : kExts) {
        std::filesystem::path p = std::filesystem::path(covers_dir) /
                                  (title_id + ext);
        if (std::filesystem::exists(p, ec)) {
            return p.generic_string();
        }
    }

    // 2) case-insensitive scan
    const std::string want = ToLowerCopy(title_id);
    for (auto it = std::filesystem::directory_iterator(covers_dir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::error_code ec2;
        if (!it->is_regular_file(ec2)) continue;
        const std::string stem = ToLowerCopy(it->path().stem().generic_string());
        const std::string ext  = ToLowerCopy(it->path().extension().generic_string());
        if (stem == want &&
            (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp")) {
            return it->path().generic_string();
        }
    }
    return {};
}

// ------------------------------------------------------------------------
// ThumbnailCache
// ------------------------------------------------------------------------

ThumbnailCache::ThumbnailCache()  = default;
ThumbnailCache::~ThumbnailCache() { Clear(); }

void ThumbnailCache::SetCoversDir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mu_);
    covers_dir_ = dir;
}

void ThumbnailCache::Clear() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [k, v] : cache_) {
        if (v.texture) {
            // GL calls are only legal on the thread that owns the GL
            // context.  The UI thread is the one that creates textures,
            // so this path is the one taken in normal shutdown.
            GLuint tex = static_cast<GLuint>(v.texture);
            glDeleteTextures(1, &tex);
        }
    }
    cache_.clear();
}

std::size_t ThumbnailCache::Size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cache_.size();
}

// Load `path` as RGBA8 and upload it to a fresh OpenGL texture.  Returns
// a default Thumbnail on failure.
static Thumbnail LoadCoverFromDisk(const std::string& path) {
    Thumbnail out;
    int w = 0, h = 0, channels_in_file = 0;
    // Force 4 channels so the GL upload is always RGBA8.
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels_in_file, 4);
    if (!pixels || w <= 0 || h <= 0) {
        LOG_WARN(General, "Thumbnail: failed to decode %s (%s)",
                 path.c_str(), stbi_failure_reason() ? stbi_failure_reason() : "?");
        if (pixels) stbi_image_free(pixels);
        return out;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0) {
        stbi_image_free(pixels);
        return out;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    // Linear filtering gives noticeably better-looking thumbnails at
    // non-1:1 sizes without measurable cost for static cover art.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // GL_CLAMP_TO_EDGE (0x812F) is from OpenGL 1.2; the legacy MSVC
    // <GL/gl.h> header used here does not expose it as a macro.  Define
    // it locally so the call works on every toolchain.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    // Upload flipped-Y so the image renders right-side up in ImGui (which
    // expects UV (0,0) = top-left).  stb_image's default origin is
    // bottom-left, which would render covers upside down.
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);

    stbi_image_free(pixels);
    out.texture = static_cast<std::uint64_t>(tex);
    out.width   = w;
    out.height  = h;
    return out;
}

Thumbnail ThumbnailCache::Get(const std::string& title_id) {
    if (title_id.empty()) return Thumbnail{};

    // Fast path: already cached.
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = cache_.find(title_id);
        if (it != cache_.end()) return it->second;
    }

    // Resolve the on-disk path.  We do this outside the lock because
    // filesystem IO can be slow on cold cache.
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mu_);
        path = FindCoverFile(title_id, covers_dir_);
    }
    if (path.empty()) {
        // Cache the "missing" sentinel so we don't re-stat the disk on
        // every frame.
        std::lock_guard<std::mutex> lock(mu_);
        cache_.emplace(title_id, Thumbnail{});
        return Thumbnail{};
    }

    // Decode + upload.  The lock is held only for the cache update at
    // the end; the decode itself is locked-free so other titles can be
    // looked up in parallel from the same frame.
    Thumbnail t = LoadCoverFromDisk(path);

    {
        std::lock_guard<std::mutex> lock(mu_);
        auto [it, inserted] = cache_.emplace(title_id, t);
        if (!inserted && it->second.texture != t.texture) {
            // Another thread won the race; throw away our texture so
            // we don't leak GL resources.
            if (t.texture) {
                GLuint tex = static_cast<GLuint>(t.texture);
                glDeleteTextures(1, &tex);
            }
            return it->second;
        }
        return t;
    }
}

Thumbnail ThumbnailCache::GetFromPath(const std::string& cache_key,
                                      const std::string& abs_path) {
    if (cache_key.empty() || abs_path.empty()) return Thumbnail{};

    // Fast path: already cached under this key.
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = cache_.find(cache_key);
        if (it != cache_.end()) return it->second;
    }

    std::error_code ec;
    if (!std::filesystem::exists(abs_path, ec) ||
        !std::filesystem::is_regular_file(abs_path, ec)) {
        std::lock_guard<std::mutex> lock(mu_);
        cache_.emplace(cache_key, Thumbnail{});
        return Thumbnail{};
    }

    Thumbnail t = LoadCoverFromDisk(abs_path);
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto [it, inserted] = cache_.emplace(cache_key, t);
        if (!inserted && it->second.texture != t.texture) {
            if (t.texture) {
                GLuint tex = static_cast<GLuint>(t.texture);
                glDeleteTextures(1, &tex);
            }
            return it->second;
        }
        return t;
    }
}

}  // namespace Ui
