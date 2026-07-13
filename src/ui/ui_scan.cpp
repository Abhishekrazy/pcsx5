// Helpers for the UI: scanning the games directory and locating the
// backend binary.  Split out from ui.cpp so unit tests can call them
// without colliding with the UI's own `main()`.

#include "ui/ui.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace Ui {

// Strip trailing "-appN" or "-patchN" from a directory name so the canonical
// title id is what users recognise (e.g. PPSA02929-app0 -> PPSA02929).
static std::string CanonicalizeTitleId(const std::string& dir_name) {
    for (const char* suffix : {"-app", "-patch"}) {
        const auto pos = dir_name.find(suffix);
        if (pos != std::string::npos) return dir_name.substr(0, pos);
    }
    return dir_name;
}

// Find the first eboot-like file inside `dir`.  Preference order:
//   decrypted/eboot.bin, eboot.bin.esbak, eboot.bin, eboot.elf
static std::string FindEboot(const std::filesystem::path& dir) {
    for (const char* name : {"decrypted/eboot.bin", "eboot.bin.esbak", "eboot.bin", "eboot.elf"}) {
        const auto p = dir / name;
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p.generic_string();
    }
    return {};
}

// Recursive walk that sums every regular file's size.  Skips reparse points
// and silently tolerates permission errors so a single unreadable file does
// not zero out the total.
static std::uint64_t ComputeDirectorySize(const std::filesystem::path& root) {
    std::uint64_t total = 0;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) ||
        !std::filesystem::is_directory(root, ec)) {
        return 0;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        std::error_code ec2;
        if (it->is_regular_file(ec2)) {
            total += it->file_size(ec2);
        }
    }
    return total;
}

// Read the human-readable title from the PKG's `sce_sys/param.json`.
// Falls back to "" when the file is missing or the JSON is malformed.
// We only look for the obvious "titleName" key inside
// "localizedParameters" -> "en-US" (or whichever language is marked
// "defaultLanguage").  The PKG param.json is simple enough that we can
// extract the field by scanning — no full JSON parser is needed.
static std::string ReadTitleFromParamJson(const std::filesystem::path& dir) {
    std::ifstream in(dir / "sce_sys" / "param.json");
    if (!in) return {};
    std::string text((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    if (text.empty()) return {};

    // Helper: extract a quoted string value following `key` (`"titleName"`).
    auto extract = [&](const std::string& key) -> std::string {
        const std::string needle = "\"" + key + "\"";
        const auto kpos = text.find(needle);
        if (kpos == std::string::npos) return {};
        const auto colon = text.find(':', kpos + needle.size());
        if (colon == std::string::npos) return {};
        const auto q1 = text.find('"', colon + 1);
        if (q1 == std::string::npos) return {};
        const auto q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos) return {};
        return text.substr(q1 + 1, q2 - q1 - 1);
    };

    // 1) preferred: localizedParameters -> <defaultLanguage> -> titleName
    const std::string lang = extract("defaultLanguage");
    if (!lang.empty()) {
        // Find the block for that language.  We re-scan to find the
        // first "{...titleName...}" block whose leading key matches.
        const std::string lang_key = "\"" + lang + "\"";
        const auto lpos = text.find(lang_key);
        if (lpos != std::string::npos) {
            const auto brace = text.find('{', lpos + lang_key.size());
            if (brace != std::string::npos) {
                // Find the matching closing brace (single-level — the
                // param.json structure is well-defined).
                int depth = 0;
                std::size_t end = brace;
                for (; end < text.size(); ++end) {
                    if (text[end] == '{') ++depth;
                    else if (text[end] == '}') { --depth; if (depth == 0) break; }
                }
                if (end < text.size()) {
                    const std::string block = text.substr(brace, end - brace);
                    const std::string name = [&]{
                        const std::string needle2 = "\"titleName\"";
                        const auto kp = block.find(needle2);
                        if (kp == std::string::npos) return std::string{};
                        const auto cl = block.find(':', kp + needle2.size());
                        if (cl == std::string::npos) return std::string{};
                        const auto qa = block.find('"', cl + 1);
                        if (qa == std::string::npos) return std::string{};
                        const auto qb = block.find('"', qa + 1);
                        if (qb == std::string::npos) return std::string{};
                        return block.substr(qa + 1, qb - qa - 1);
                    }();
                    if (!name.empty()) return name;
                }
            }
        }
    }
    // 2) fallback: any "titleName" field in the file.
    return extract("titleName");
}

// Look for a single file inside `dir` whose name matches `candidates`
// (case-insensitive, with or without extension).  Returns the absolute
// path or "" if nothing was found.  Used to find the icon, key art
// and default-music files that PS5 games ship under `sce_sys/`.
static std::string FindInDir(const std::filesystem::path& dir,
                             std::initializer_list<const char*> candidates) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return {};
    // Direct exact-case match.
    for (const char* name : candidates) {
        std::filesystem::path p = dir / name;
        if (std::filesystem::exists(p, ec)) {
            return std::filesystem::absolute(p, ec).generic_string();
        }
    }
    // Case-insensitive scan as a fallback (some dumps use e.g. "Icon0.PNG").
    auto lower = [](std::string s) {
        for (auto& c : s)
            c = static_cast<char>(std::tolower(
                static_cast<unsigned char>(c)));
        return s;
    };
    std::vector<std::string> want;
    want.reserve(candidates.size());
    for (const char* name : candidates) want.push_back(lower(name));
    for (auto it = std::filesystem::directory_iterator(dir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (lower(it->path().filename().generic_string())
            == want[0]) {
            return std::filesystem::absolute(it->path(), ec).generic_string();
        }
    }
    return {};
}

// Locate the cover image inside the game's own sce_sys folder.  PS5
// games ship icon0.png (square thumbnail) and pic0/1/2.png (16:9 key
// art).  icon0 is preferred because it is what the PS5 home screen
// uses for the game tile, exactly matching what SharpEmu shows.
static std::string ResolveInGameCover(const std::filesystem::path& game_dir) {
    std::string s = FindInDir(game_dir / "sce_sys", {"icon0.png", "icon0.jpg"});
    if (!s.empty()) return s;
    return {};
}

// Locate the largest background / hero image inside the game folder.
// PS5 ships pic0/1/2.png (often 1920x1080).  We prefer pic1, falling
// back to pic0 and pic2.
static std::string ResolveInGameBackground(const std::filesystem::path& game_dir) {
    const std::filesystem::path sce = game_dir / "sce_sys";
    for (const char* name : {"pic1.png", "pic1.jpg",
                             "pic0.png", "pic0.jpg",
                             "pic2.png", "pic2.jpg"}) {
        std::string s = FindInDir(sce, {name});
        if (!s.empty()) return s;
    }
    return {};
}

// Locate the game's default-music track.  PS5 stores this as
// sce_sys/snd0.at9 (ATRAC9 — Sony's lossy codec).  Some homebrew /
// debug builds use snd0.wav or snd0.mp3 instead.  We return the first
// existing file in the standard order; actual playback is left to a
// future ATRAC9 integration (SharpEmu uses vendored LibAtrac9 + winmm;
// we do not have a C++ ATRAC9 decoder yet, so we expose the path and
// can fall back to OGG/WAV from `media/` if the user wants sound
// without a decoder).
static std::string ResolveDefaultMusic(const std::filesystem::path& game_dir) {
    std::string s = FindInDir(game_dir / "sce_sys",
                              {"snd0.at9", "snd0.wav", "snd0.mp3",
                               "snd0.ogg", "snd0.flac"});
    if (!s.empty()) return s;
    // Fall back to the first audio file we can find in media/ — these
    // are typically the in-game music tracks and use playable formats.
    const std::filesystem::path media = game_dir / "media";
    std::error_code ec;
    if (!std::filesystem::is_directory(media, ec)) return {};
    for (auto it = std::filesystem::directory_iterator(media, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().generic_string();
        for (auto& c : ext)
            c = static_cast<char>(std::tolower(
                static_cast<unsigned char>(c)));
        if (ext == ".ogg" || ext == ".wav" || ext == ".mp3" ||
            ext == ".flac") {
            return std::filesystem::absolute(it->path(), ec).generic_string();
        }
    }
    return {};
}

std::vector<GameEntry> ScanGames(const std::string& games_dir,
                                 const std::string& covers_dir) {
    std::vector<GameEntry> out;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(games_dir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const auto& entry = *it;
        if (!entry.is_directory()) continue;
        const std::string dir_name = entry.path().filename().generic_string();
        if (dir_name.empty() || dir_name[0] == '.') continue;  // skip hidden
        std::string elf = FindEboot(entry.path());
        if (elf.empty()) continue;
        GameEntry g;
        g.title_id     = CanonicalizeTitleId(dir_name);
        // Prefer the human-readable name from param.json so the UI
        // shows "Super Monkey Ball Banana Mania" instead of "PPSA01668".
        // Falls back to the directory name when the JSON is missing
        // or does not contain a "titleName" field.
        const std::string from_json = ReadTitleFromParamJson(entry.path());
        g.display_name = !from_json.empty() ? from_json : dir_name;
        g.elf_path     = elf;
        g.dir_path     = std::filesystem::absolute(entry.path(), ec).generic_string();
        g.status_text  = "unknown";
        // Total install size across every file in the game dir.
        g.size_bytes   = ComputeDirectorySize(entry.path());
        // Cover art: in-game sce_sys/icon0.png wins, then a cover in
        // the global covers dir keyed by title_id, then nothing.  PS5
        // games always ship icon0.png so this should always hit.
        g.cover_path   = ResolveInGameCover(entry.path());
        if (g.cover_path.empty()) {
            g.cover_path = ResolveCoverPath(g.title_id, covers_dir);
        }
        // Background / key art (used for the large hero image in the
        // selected-game pane).  pic1 is the standard choice.
        g.background_path = ResolveInGameBackground(entry.path());
        // Default-music track.  Exposed to the UI; actual playback
        // needs an ATRAC9 decoder which is not yet integrated.
        g.music_path   = ResolveDefaultMusic(entry.path());
        out.push_back(std::move(g));
    }
    std::sort(out.begin(), out.end(),
              [](const GameEntry& a, const GameEntry& b) {
                  return a.title_id < b.title_id;
              });
    return out;
}

std::string ResolveCoverPath(const std::string& title_id,
                             const std::string& covers_dir) {
    if (covers_dir.empty() || title_id.empty()) return {};
    std::error_code ec;
    if (!std::filesystem::is_directory(covers_dir, ec)) return {};
    // 1) exact case match for the most common extensions
    for (const char* ext : {".png", ".jpg", ".jpeg", ".webp"}) {
        std::filesystem::path p = std::filesystem::path(covers_dir) /
                                  (title_id + ext);
        if (std::filesystem::exists(p, ec)) {
            return std::filesystem::absolute(p, ec).generic_string();
        }
    }
    // 2) case-insensitive scan (NTFS is case-preserving but case-insensitive;
    //    ext4 / APFS may differ).  Iterate the dir once and look for a
    //    filename whose lowercase stem equals the title_id.
    std::string want = title_id;
    for (auto& c : want) c = static_cast<char>(std::tolower(
                                static_cast<unsigned char>(c)));
    for (auto it = std::filesystem::directory_iterator(covers_dir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string stem = it->path().stem().generic_string();
        std::string ext  = it->path().extension().generic_string();
        for (auto& c : stem) c = static_cast<char>(std::tolower(
                                  static_cast<unsigned char>(c)));
        for (auto& c : ext)  c = static_cast<char>(std::tolower(
                                  static_cast<unsigned char>(c)));
        if (stem == want &&
            (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp")) {
            return std::filesystem::absolute(it->path(), ec).generic_string();
        }
    }
    return {};
}

std::string LocateBackend(const std::string& hint) {
#ifdef _WIN32
    const std::string exe_name = "pcsx5.exe";
#else
    const std::string exe_name = "pcsx5";
#endif
    // 1) explicit hint (e.g. user passed --backend)
    if (!hint.empty() && std::filesystem::exists(hint)) return hint;

    // Collect search roots: the directory the UI exe lives in, the working
    // directory, and "sibling-of-ui-exe" patterns like bin/Debug.  The
    // backend is built into <build>/bin/<Config>/ while the UI is built
    // into <build>/<Config>/, so a simple same-dir lookup will not find
    // it.  We also try bin/ relative to cwd for "run from source tree".
    std::vector<std::filesystem::path> roots;
    {
        char buf[MAX_PATH] = {};
        DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::filesystem::path ui_dir(buf);
            ui_dir = ui_dir.parent_path();
            roots.push_back(ui_dir);
            // 2a) ./bin/Debug, ./bin/Release under the UI exe dir
            roots.push_back(ui_dir / "bin" / "Debug");
            roots.push_back(ui_dir / "bin" / "Release");
            // 2b) The UI is typically in <build>/<Config>/ and the backend
            //     in <build>/bin/<Config>/.  Walk one up.
            if (ui_dir.has_parent_path()) {
                std::filesystem::path parent = ui_dir.parent_path();
                roots.push_back(parent / "bin" / "Debug");
                roots.push_back(parent / "bin" / "Release");
                // 2c) Config-suffixed sibling matching the UI exe's dir
                //     (e.g. UI in <build>/Debug, backend in <build>/bin/Debug).
                std::string cfg = ui_dir.filename().generic_string();
                if (cfg == "Debug" || cfg == "Release" || cfg == "RelWithDebInfo") {
                    roots.push_back(parent / "bin" / cfg);
                }
            }
        }
    }
    // 3) cwd-rooted candidates.
    {
        std::error_code ec;
        std::filesystem::path cwd = std::filesystem::current_path(ec);
        if (!ec) {
            roots.push_back(cwd);
            roots.push_back(cwd / "bin" / "Debug");
            roots.push_back(cwd / "bin" / "Release");
        }
    }
    // 4) cwd-relative bare name (PATH lookup).
    roots.push_back(exe_name);

    for (const auto& r : roots) {
        std::error_code ec;
        std::filesystem::path cand = r / exe_name;
        if (std::filesystem::exists(cand, ec)) {
            return std::filesystem::absolute(cand, ec).generic_string();
        }
    }
    return {};
}

}  // namespace Ui
