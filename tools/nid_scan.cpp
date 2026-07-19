// nid_scan — scan dumped PS5 PRX/SPRX modules and emit nid_db.txt lines.
//
// Usage:
//   nid_scan <dir-or-prx> [more paths...] > out.txt
//
// Directories are searched recursively for *.prx / *.sprx modules.  Each
// module is loaded through the standard Loader::Load / Loader::LoadSelf
// path (fake-signed plaintext SELFs only; retail-encrypted modules warn to
// stderr and are skipped).  For every export symbol we emit one
// nid_db.txt line:
//
//     NID<TAB>module<TAB>name
//
// Two export-name forms are handled:
//   - NID strings ("<11 base64 chars>" or "<11 base64>#T#T"): the NID is
//     taken verbatim; a name is emitted only when the built-in table /
//     assets/nid_db.txt already resolves it.
//   - human-readable names ("sceFoo" or "sceFoo#T#T"): the Sony NID is
//     derived with the documented PS5 formula
//         NID = base64_sony( reverse( SHA1(name || salt)[0..8) ) )
//     with salt 518D64A635DED8C1E6B039B1C3E55230 (the same formula used
//     by the public aerolib / prospero tooling).
//
// Output: a '#' comment header with provenance counts, then all lines
// sorted and deduplicated.  Modules that fail to load warn to stderr and
// do not abort the scan.
//
// Self-contained: the SHA-1 core lives in this file (Common::Crypto only
// provides SHA-256 and adding SHA-1 there just for this tool is not
// warranted).

#include "loader/elf.h"
#include "memory/memory.h"
#include "common/log.h"
#include "common/nid.h"
#include "common/types.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Minimal SHA-1 (FIPS 180-1), one-shot.  Needed only for the PS5 NID formula.
// ---------------------------------------------------------------------------

class Sha1 {
public:
    Sha1() { Reset(); }

    void Reset() {
        state_ = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                  0xC3D2E1F0u};
        total_len_ = 0;
        block_len_ = 0;
    }

    void Update(const u8* data, size_t size) {
        total_len_ += size;
        while (size > 0) {
            const size_t take = std::min(size, sizeof(block_) - block_len_);
            std::memcpy(block_.data() + block_len_, data, take);
            block_len_ += take;
            data += take;
            size -= take;
            if (block_len_ == sizeof(block_)) {
                ProcessBlock(block_.data());
                block_len_ = 0;
            }
        }
    }

    std::array<u8, 20> Final() {
        const u64 bit_len = static_cast<u64>(total_len_) * 8u;
        const u8 pad = 0x80;
        Update(&pad, 1);
        const u8 zero = 0;
        while (block_len_ != 56) Update(&zero, 1);
        u8 len_be[8];
        for (int i = 0; i < 8; ++i) {
            len_be[i] = static_cast<u8>(bit_len >> ((7 - i) * 8));
        }
        Update(len_be, sizeof(len_be));

        std::array<u8, 20> out{};
        for (size_t i = 0; i < state_.size(); ++i) {
            out[i * 4 + 0] = static_cast<u8>(state_[i] >> 24);
            out[i * 4 + 1] = static_cast<u8>(state_[i] >> 16);
            out[i * 4 + 2] = static_cast<u8>(state_[i] >> 8);
            out[i * 4 + 3] = static_cast<u8>(state_[i]);
        }
        return out;
    }

private:
    static u32 Rotl(u32 v, int bits) {
        return (v << bits) | (v >> (32 - bits));
    }

    void ProcessBlock(const u8* block) {
        u32 w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<u32>(block[i * 4 + 0]) << 24) |
                   (static_cast<u32>(block[i * 4 + 1]) << 16) |
                   (static_cast<u32>(block[i * 4 + 2]) << 8) |
                   (static_cast<u32>(block[i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        u32 a = state_[0], b = state_[1], c = state_[2], d = state_[3],
            e = state_[4];
        for (int i = 0; i < 80; ++i) {
            u32 f;
            u32 k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const u32 tmp = Rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rotl(b, 30);
            b = a;
            a = tmp;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }

    std::array<u32, 5> state_{};
    std::array<u8, 64> block_{};
    u64 total_len_ = 0;
    size_t block_len_ = 0;
};

// PS5 NID derivation salt (see aerolib / prospero tooling).
constexpr u8 kNidSalt[16] = {0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
                             0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30};

// NID = base64_sony( reverse( SHA1(name || salt)[0..8) ) )
std::string NameToNid(const std::string& name) {
    Sha1 sha;
    sha.Update(reinterpret_cast<const u8*>(name.data()), name.size());
    sha.Update(kNidSalt, sizeof(kNidSalt));
    const auto digest = sha.Final();

    Common::Ps5Nid nid{};
    for (size_t i = 0; i < nid.bytes.size(); ++i) {
        nid.bytes[i] = digest[7 - i];
    }
    return Common::EncodeNid(nid);
}

// ---------------------------------------------------------------------------
// Scan logic
// ---------------------------------------------------------------------------

struct Stats {
    size_t files_seen = 0;
    size_t files_loaded = 0;
    size_t files_failed = 0;
    size_t exports_seen = 0;
    size_t named_computed = 0;   // human-readable names -> derived NID
    size_t nid_form_named = 0;   // NID-form symbols resolved via the table
    size_t nid_form_unknown = 0; // NID-form symbols with no known name
};

// Strip a trailing 4-char "#X#X" type tag, if present.
std::string StripTag(const std::string& s) {
    if (s.size() > 4 && s[s.size() - 4] == '#' && s[s.size() - 2] == '#') {
        return s.substr(0, s.size() - 4);
    }
    return s;
}

// True if `s` (already tag-stripped) is exactly an 11-char NID string.
bool LooksLikeNid(const std::string& s, Common::Ps5Nid& out) {
    if (s.size() != Common::kPs5NidEncodedLen) return false;
    auto decoded = Common::DecodeNid(s);
    if (!decoded) return false;
    out = *decoded;
    return true;
}

bool IsModuleFile(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return ext == ".prx" || ext == ".sprx";
}

void ScanModule(const std::filesystem::path& path, std::set<std::string>& lines,
                Stats& stats) {
    ++stats.files_seen;

    Loader::LoadedModule m{};
    bool ok = false;
    if (Loader::IsSelfFile(path.string())) {
        ok = Loader::LoadSelf(path.string(), m);
    } else {
        ok = Loader::Load(path.string(), m);
    }
    if (!ok) {
        std::fprintf(stderr, "[nid_scan] warning: failed to load %s (skipped)\n",
                     path.string().c_str());
        ++stats.files_failed;
        return;
    }
    ++stats.files_loaded;

    // Module name: DT_SONAME when present, else the file stem.
    std::string module = m.soname;
    if (module.empty()) module = path.stem().string();

    Loader::ModuleMetadata meta{};
    Loader::ParseModuleMetadata(m, meta);

    for (const auto& exp : meta.exports) {
        if (exp.name.empty()) continue;
        ++stats.exports_seen;

        const std::string base = StripTag(exp.name);
        Common::Ps5Nid nid{};
        if (LooksLikeNid(base, nid)) {
            // The symbol name IS a NID; emit only if we already know the name.
            auto name = Common::LookupNidName(nid);
            if (name) {
                lines.insert(base + "\t" + module + "\t" + std::string(*name));
                ++stats.nid_form_named;
            } else {
                ++stats.nid_form_unknown;
            }
        } else {
            // Human-readable name: derive the Sony NID.
            lines.insert(NameToNid(base) + "\t" + module + "\t" + base);
            ++stats.named_computed;
        }
    }

    // The loader maps every PIE module at the same fixed base (0x800000000)
    // and there is no Loader-level unload.  Release the reservation so the
    // next module can be mapped in this process.  Reserve() aligns the size
    // up to the 64KB Windows allocation granularity; mirror that here.
    const u64 kAllocGran = 0x10000;
    const u64 unmap_size =
        (m.image_size + kAllocGran - 1) & ~(kAllocGran - 1);
    if (Memory::Unmap(m.base_address, unmap_size) != Memory::Status::Ok) {
        std::fprintf(stderr,
                     "[nid_scan] warning: could not unmap %s; later modules "
                     "may fail to load\n", path.string().c_str());
    }
}

void CollectModules(const std::filesystem::path& root,
                    std::vector<std::filesystem::path>& out) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(root, ec)) {
        if (IsModuleFile(root)) out.push_back(root);
        return;
    }
    if (!std::filesystem::is_directory(root, ec)) {
        std::fprintf(stderr, "[nid_scan] warning: not a file or directory: %s\n",
                     root.string().c_str());
        return;
    }
    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && IsModuleFile(it->path())) {
            out.push_back(it->path());
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: nid_scan <dir-or-prx> [more paths...] > out.txt\n"
                     "  Recursively scans *.prx/*.sprx modules and emits\n"
                     "  nid_db.txt lines (NID<TAB>module<TAB>name) on stdout.\n");
        return 2;
    }

    // Keep stdout clean for the nid_db output: the tool reports load
    // failures itself on stderr, so only Critical logger records are shown.
    LogConfig::SetLevel(LogCategory::Loader, LogLevel::Critical);
    LogConfig::SetLevel(LogCategory::Memory, LogLevel::Critical);
    LogConfig::SetLevel(LogCategory::General, LogLevel::Critical);

    // Merge the shipped database so NID-form exports can resolve to names.
    const char* db_candidates[] = {"assets/nid_db.txt", "../assets/nid_db.txt",
                                   "../../assets/nid_db.txt"};
    for (const char* db : db_candidates) {
        std::error_code ec;
        if (std::filesystem::exists(db, ec)) {
            Common::LoadNidDatabase(db);
            break;
        }
    }

    if (!Memory::Initialize()) {
        std::fprintf(stderr, "[nid_scan] FATAL: Memory::Initialize failed\n");
        return 1;
    }

    std::vector<std::filesystem::path> modules;
    for (int i = 1; i < argc; ++i) {
        CollectModules(argv[i], modules);
    }
    std::sort(modules.begin(), modules.end());
    modules.erase(std::unique(modules.begin(), modules.end()), modules.end());

    std::set<std::string> lines;
    Stats stats;
    for (const auto& mod : modules) {
        ScanModule(mod, lines, stats);
    }

    Memory::Shutdown();

    std::printf("# nid_scan output\n");
    std::printf("# modules found   : %zu\n", stats.files_seen);
    std::printf("# modules loaded  : %zu (failed: %zu)\n", stats.files_loaded,
                stats.files_failed);
    std::printf("# exports seen    : %zu\n", stats.exports_seen);
    std::printf("# named (computed NID)      : %zu\n", stats.named_computed);
    std::printf("# NID-form, name known      : %zu\n", stats.nid_form_named);
    std::printf("# NID-form, name unknown    : %zu (not emitted)\n",
                stats.nid_form_unknown);
    std::printf("# unique lines    : %zu\n", lines.size());
    for (const auto& line : lines) {
        std::printf("%s\n", line.c_str());
    }
    return 0;
}
