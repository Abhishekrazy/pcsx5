// Cover-art thumbnail loader + texture cache for the game library.
//
// SharpEmu-style: each game has a cover image looked up by title id from
// a local covers directory (`./Covers/<TITLE_ID>.png`).  PNG / JPG / JPEG
// / WebP are supported via stb_image.  Loaded images are uploaded as
// OpenGL textures and cached for the lifetime of the UI process.
//
// Lookup order (case-insensitive):
//   1) <covers_dir>/<TITLE_ID>.png
//   2) <covers_dir>/<TITLE_ID>.jpg
//   3) <covers_dir>/<TITLE_ID>.jpeg
//   4) <covers_dir>/<TITLE_ID>.webp

#pragma once

#include "ui/ui.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Ui {

// Single OpenGL texture handle + dimensions.  When the cover is missing
// or the load failed, `valid()` is false and the texture id is 0.
struct Thumbnail {
    // ImTextureID is 64-bit on most platforms, so we use uint64_t to
    // avoid truncation when casting from a 32-bit GLuint.
    std::uint64_t texture = 0;
    int           width   = 0;
    int           height  = 0;
    bool          valid() const { return texture != 0 && width > 0 && height > 0; }
};

// Lookup-only helper: returns the on-disk path of the cover for
// `title_id`, or "" if no cover exists.  Re-exported here for callers
// that want to know about the path (e.g. to display a tooltip) without
// touching the texture cache.
std::string FindCoverFile(const std::string& title_id,
                          const std::string& covers_dir);

// Thread-safe texture cache.  Lookup is cheap; the first request for a
// given title triggers a synchronous stb_image decode + GL upload.
// All public methods are safe to call from the UI thread; loading is
// done lazily on the calling thread (cards don't block each other
// because ImGui yields between frames).
class ThumbnailCache {
public:
    ThumbnailCache();
    ~ThumbnailCache();

    // Re-bind the covers directory.  Existing cached textures stay valid;
    // future lookups will use the new directory.
    void SetCoversDir(const std::string& dir);

    // Look up a cover (loads on first request).  Returns an empty
    // Thumbnail if the cover file is missing or the load failed.
    Thumbnail Get(const std::string& title_id);

    // Load a cover from an explicit absolute path (e.g. the game's own
    // sce_sys/icon0.png).  Pass the title id as a cache key so a second
    // call with the same title hits the cache instead of re-decoding.
    Thumbnail GetFromPath(const std::string& cache_key,
                          const std::string& abs_path);

    // Discard every cached texture and free GL resources.  Called on
    // shutdown or covers-dir change.
    void Clear();

    // Total entries currently in the cache.
    std::size_t Size() const;

private:
    std::string covers_dir_;
    mutable std::mutex                                 mu_;
    std::unordered_map<std::string, Thumbnail>         cache_;
};

}  // namespace Ui
