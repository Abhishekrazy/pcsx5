#include "hle.h"
#include "../common/crypto.h"
#include "../common/log.h"
#include "../config/config.h"
#include "../memory/memory.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace HLE {

    // -------------------------------------------------------------------------
    // Save-data host backing store.
    //
    // Mounts are backed by a real host directory so save data persists across
    // runs:  <cwd>/pcsx5_savedata/<title-id>/
    // The title id comes from --title-id (see main.cpp); "PPSA02929" is the
    // fallback when the emulator is launched without one.
    //
    // Per-user layout: when the config carries more than one user profile the
    // effective savedata dir gains a per-user level (<dir>/<user-id>/).  With
    // zero or one profile the legacy flat dir is kept so existing saves are
    // migration-safe.  Trophies (libnp.cpp) deliberately stay on the flat
    // per-title dir.
    //
    // Encrypted savedata images: when the per-title config enables
    // savedata_crypto (ConfigService::SaveDataKeysFor), the savedata area is
    // stored as a single AES-128-XTS encrypted image file <dir>/sdimg.dat
    // instead of plain files.  The image is a simple archive (see
    // kSdimgMagic) encrypted whole; the XTS data-unit number is the 16-byte
    // little-endian index of each 16-byte block (data_unit[i] = block index,
    // i.e. byte offset / 16).  On mount the image is decrypted and extracted
    // to the effective dir; on umount/commit the dir is re-archived and
    // re-encrypted.  The plain (decrypted) host dir is the only form guest
    // file I/O ever sees.
    // -------------------------------------------------------------------------
    namespace {
        std::string g_savedata_title_id = "PPSA02929";

        // sceSaveDataMountPoint: char data[16] on PS4/PS5 — the mount name.
        // We zero 32 bytes (generous) before writing so padding reads as 0.
        constexpr u64 kMountPointOutSize = 32;

        // SceSaveDataDirNameSearchResult layout we write (mirrors the PS4/PS5
        // ABI): u32 hitCount at offset 0, followed by an array of
        // SceSaveDataDirName (char data[32]) entries at offset 4.  The cond
        // struct is game-built and its maxCount field offset is not part of
        // any stable public doc, so we accept an optional guest-provided cap
        // read as a u32 at cond+0x20 (0 = no cap) and additionally clamp to a
        // hard limit so a corrupt cond can never drive unbounded writes.
        constexpr u64  kDirNameSize          = 32;
        constexpr u64  kDirSearchResultBase  = 4;   // hitCount u32
        constexpr u64  kDirSearchCondMaxOff  = 0x20;
        constexpr u32  kDirSearchHardMax     = 64;

        // Encrypted savedata image container (sdimg.dat), pre-encryption:
        //   u32 magic 'P5SD', u32 version(1), u32 file_count, u32 reserved
        //   then per file: u32 rel_path_len, char rel_path[rel_path_len],
        //                  u64 data_size, u8 data[data_size]
        // The archive is zero-padded to a 16-byte multiple before XTS.
        constexpr u32 kSdimgMagic   = 0x50445335; // "P5SD" little-endian
        constexpr u32 kSdimgVersion = 1;
        constexpr const char* kSdimgFileName = "sdimg.dat";

        std::string EnsureSaveDataDir() {
            std::error_code ec;
            std::filesystem::path root =
                std::filesystem::current_path(ec) / "pcsx5_savedata" / g_savedata_title_id;
            if (ec) {
                root = std::filesystem::path("pcsx5_savedata") / g_savedata_title_id;
            }
            std::filesystem::create_directories(root, ec);
            if (ec) {
                LOG_WARN(HLE, "SaveData: failed to create host dir '%s': %s",
                         root.string().c_str(), ec.message().c_str());
            }
            return root.string();
        }

        // ------------------------------------------------------------------
        // XTS helpers: data-unit number = 16-byte little-endian block index
        // (block i covers image bytes [i*16, i*16+16)).  Empty input is a
        // no-op success.
        // ------------------------------------------------------------------
        bool XtsCryptWhole(const ConfigService::SaveDataCrypto& keys,
                           const std::vector<u8>& in, std::vector<u8>& out,
                           bool encrypt) {
            if (in.empty()) { out.clear(); return true; }
            if (in.size() % Common::kAes128BlockSize != 0) return false;
            const Common::Aes128Key k1 = Common::Aes128ExpandKey(keys.xts_key1.data());
            const Common::Aes128Key k2 = Common::Aes128ExpandKey(keys.xts_key2.data());
            out.resize(in.size());
            const size_t blocks = in.size() / Common::kAes128BlockSize;
            for (size_t i = 0; i < blocks; ++i) {
                u8 data_unit[16] = {};
                std::memcpy(data_unit, &i, sizeof(i)); // LE block index
                const u8* src = in.data() + i * 16;
                u8* dst = out.data() + i * 16;
                const bool ok = encrypt
                    ? Common::Aes128XtsEncrypt(k1, k2, data_unit, src, dst, 16)
                    : Common::Aes128XtsDecrypt(k1, k2, data_unit, src, dst, 16);
                if (!ok) return false;
            }
            return true;
        }

        // Little-endian POD append/read for the archive format.
        template <typename T>
        void ArchiveWrite(std::vector<u8>& buf, const T& v) {
            const u8* p = reinterpret_cast<const u8*>(&v);
            buf.insert(buf.end(), p, p + sizeof(T));
        }
        template <typename T>
        bool ArchiveRead(const std::vector<u8>& buf, size_t& pos, T& out) {
            if (pos + sizeof(T) > buf.size()) return false;
            std::memcpy(&out, buf.data() + pos, sizeof(T));
            pos += sizeof(T);
            return true;
        }

        // Pack every regular file under `dir` (relative paths, '/'-separated)
        // into the archive format, zero-padded to a 16-byte multiple.
        bool BuildArchiveFromDir(const std::string& dir, std::vector<u8>& out) {
            std::error_code ec;
            std::vector<std::pair<std::string, std::filesystem::path>> files;
            const std::filesystem::path root(dir);
            for (const auto& e : std::filesystem::recursive_directory_iterator(root, ec)) {
                if (!e.is_regular_file()) continue;
                if (e.path().filename() == kSdimgFileName) continue; // never pack the image itself
                files.emplace_back(e.path().lexically_relative(root).generic_string(), e.path());
            }
            if (ec) {
                LOG_WARN(HLE, "SAVEDATA_CRYPTO: failed to enumerate '%s': %s",
                         dir.c_str(), ec.message().c_str());
                return false;
            }
            out.clear();
            ArchiveWrite(out, kSdimgMagic);
            ArchiveWrite(out, kSdimgVersion);
            ArchiveWrite(out, static_cast<u32>(files.size()));
            ArchiveWrite(out, static_cast<u32>(0));
            for (const auto& [rel, path] : files) {
                std::ifstream f(path, std::ios::binary);
                if (!f) {
                    LOG_WARN(HLE, "SAVEDATA_CRYPTO: cannot read '%s'", path.string().c_str());
                    return false;
                }
                std::vector<u8> data((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
                ArchiveWrite(out, static_cast<u32>(rel.size()));
                out.insert(out.end(), rel.begin(), rel.end());
                ArchiveWrite(out, static_cast<u64>(data.size()));
                out.insert(out.end(), data.begin(), data.end());
            }
            out.resize((out.size() + 15) & ~size_t(15), 0); // XTS block align
            return true;
        }

        // Inverse of BuildArchiveFromDir: wipes non-image content of `dir`
        // and extracts the archive into it.
        bool ExtractArchiveToDir(const std::vector<u8>& arc, const std::string& dir) {
            size_t pos = 0;
            u32 magic = 0, version = 0, count = 0, reserved = 0;
            if (!ArchiveRead(arc, pos, magic) || !ArchiveRead(arc, pos, version) ||
                !ArchiveRead(arc, pos, count) || !ArchiveRead(arc, pos, reserved) ||
                magic != kSdimgMagic || version != kSdimgVersion) {
                LOG_WARN(HLE, "SAVEDATA_CRYPTO: sdimg.dat archive header invalid (bad keys?)");
                return false;
            }
            std::error_code ec;
            const std::filesystem::path root(dir);
            for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
                if (e.path().filename() == kSdimgFileName) continue;
                std::filesystem::remove_all(e.path(), ec);
            }
            for (u32 i = 0; i < count; ++i) {
                u32 name_len = 0;
                u64 data_size = 0;
                if (!ArchiveRead(arc, pos, name_len) ||
                    name_len == 0 || pos + name_len > arc.size()) return false;
                const std::string rel(reinterpret_cast<const char*>(arc.data() + pos), name_len);
                pos += name_len;
                if (!ArchiveRead(arc, pos, data_size) ||
                    data_size > arc.size() || pos + data_size > arc.size()) return false;
                // Reject path escapes before touching the host fs.
                if (rel.find("..") != std::string::npos ||
                    rel.find(':') != std::string::npos || rel[0] == '/' || rel[0] == '\\') {
                    LOG_WARN(HLE, "SAVEDATA_CRYPTO: refusing unsafe archive path '%s'", rel.c_str());
                    return false;
                }
                const std::filesystem::path out_path = root / rel;
                std::filesystem::create_directories(out_path.parent_path(), ec);
                std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
                if (!f) return false;
                f.write(reinterpret_cast<const char*>(arc.data() + pos),
                        static_cast<std::streamsize>(data_size));
                pos += static_cast<size_t>(data_size);
            }
            return true;
        }

        std::string SdimgPath(const std::string& dir) {
            return (std::filesystem::path(dir) / kSdimgFileName).string();
        }

        // Mount-time: if an encrypted image exists and keys are configured,
        // decrypt + extract it to the effective dir.
        void SdimgDecryptOnMount(const std::string& dir) {
            const auto keys = ConfigService::SaveDataKeysFor(g_savedata_title_id);
            if (!keys) return;
            const std::string img = SdimgPath(dir);
            std::error_code ec;
            if (!std::filesystem::exists(img, ec)) {
                LOG_INFO(HLE, "SAVEDATA_CRYPTO: keys enabled, no image yet at %s — "
                              "plain dir will be materialised on commit", img.c_str());
                return;
            }
            std::ifstream f(img, std::ios::binary);
            std::vector<u8> cipher((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
            std::vector<u8> plain;
            if (!XtsCryptWhole(*keys, cipher, plain, /*encrypt=*/false) ||
                !ExtractArchiveToDir(plain, dir)) {
                LOG_WARN(HLE, "SAVEDATA_CRYPTO: mount decrypt failed for %s — leaving dir as-is",
                         img.c_str());
                return;
            }
            LOG_INFO(HLE, "SAVEDATA_CRYPTO: decrypted %zu-byte image into %s",
                     cipher.size(), dir.c_str());
        }

        // Commit-time: re-archive the effective dir and write the encrypted
        // image.  No-op when keys are not configured.
        void SdimgEncryptOnCommit(const std::string& dir) {
            const auto keys = ConfigService::SaveDataKeysFor(g_savedata_title_id);
            if (!keys) return;
            std::vector<u8> plain, cipher;
            if (!BuildArchiveFromDir(dir, plain) ||
                !XtsCryptWhole(*keys, plain, cipher, /*encrypt=*/true)) {
                LOG_WARN(HLE, "SAVEDATA_CRYPTO: commit encrypt failed for %s", dir.c_str());
                return;
            }
            const std::string img = SdimgPath(dir);
            std::ofstream f(img, std::ios::binary | std::ios::trunc);
            if (!f) {
                LOG_WARN(HLE, "SAVEDATA_CRYPTO: cannot write image %s", img.c_str());
                return;
            }
            f.write(reinterpret_cast<const char*>(cipher.data()),
                    static_cast<std::streamsize>(cipher.size()));
            LOG_INFO(HLE, "SAVEDATA_CRYPTO: wrote %zu-byte encrypted image %s",
                     cipher.size(), img.c_str());
        }

        void WriteMountPointName(guest_addr_t out_ptr, const char* name) {
            if (!out_ptr) return;
            if (!Memory::IsWritable(out_ptr, kMountPointOutSize)) {
                LOG_WARN(HLE, "SaveData: mountPoint out 0x%llx not writable — skipped", out_ptr);
                return;
            }
            u8 zero[kMountPointOutSize] = {};
            Memory::WriteBuffer(out_ptr, zero, sizeof(zero));
            Memory::WriteBuffer(out_ptr, name, std::strlen(name) + 1);
        }

        // Register under the canonical module by plain name AND under
        // "libkernel" by NID: games that statically link these libraries
        // frequently import the symbols weakly through libkernel, which is
        // exactly how PPSA02929 reaches them (see ppsa02929_run2.log).
        void RegisterSaveDataSymbol(const char* module, const char* name,
                                    const char* nid, HleHandler handler) {
            RegisterSymbol(module, name, handler);
            RegisterSymbol(module, nid, handler);
            RegisterSymbol("libkernel", nid, handler);
        }
    } // namespace

    void SetSaveDataTitleId(const std::string& title_id) {
        if (!title_id.empty()) {
            g_savedata_title_id = title_id;
        }
    }

    std::string GetSaveDataDir() {
        return EnsureSaveDataDir();
    }

    std::string GetEffectiveSaveDataDir() {
        const std::string flat = EnsureSaveDataDir();
        // Multi-profile configs split saves per user; the legacy single-user
        // layout keeps the flat dir untouched (migration-safe).
        const auto& profiles = ConfigService::Global().users.profiles;
        if (profiles.size() <= 1) return flat;
        const u32 uid = ConfigService::ActiveUserId();
        std::error_code ec;
        std::filesystem::path per_user =
            std::filesystem::path(flat) / std::to_string(uid);
        std::filesystem::create_directories(per_user, ec);
        if (ec) {
            LOG_WARN(HLE, "SaveData: failed to create per-user dir '%s': %s — using flat dir",
                     per_user.string().c_str(), ec.message().c_str());
            return flat;
        }
        return per_user.string();
    }

    void RegisterLibSaveData() {
        LOG_INFO(HLE, "Registering libSceSaveData / common-dialog HLE symbols...");

        // -----------------------------------------------------------------
        // sceSaveDataInitialize3(const SceSaveDataInitParams3* params)
        // No out-params.  Creates the host backing directory up front.
        // -----------------------------------------------------------------
        auto Initialize3 = [](const GuestArgs& args) -> u64 {
            LOG_INFO(HLE, "sceSaveDataInitialize3(params: 0x%llx)", args.arg1);
            const std::string dir = EnsureSaveDataDir();
            LOG_INFO(HLE, "sceSaveDataInitialize3 -> host dir: %s", dir.c_str());
            return 0;
        };
        RegisterSaveDataSymbol("libSceSaveData", "sceSaveDataInitialize3",
                               "TywrFKCoLGY#G#H", Initialize3);

        // -----------------------------------------------------------------
        // sceSaveDataMount3(SceSaveDataMountPoint* mountPoint /*out*/)
        // Fills the out-param with a valid mount name so the game can use it
        // in subsequent path building instead of dereferencing garbage.
        // -----------------------------------------------------------------
        auto Mount3 = [](const GuestArgs& args) -> u64 {
            guest_addr_t mount_out = args.arg1;
            LOG_INFO(HLE, "sceSaveDataMount3(mountPoint out: 0x%llx)", mount_out);
            const std::string dir = GetEffectiveSaveDataDir();
            SdimgDecryptOnMount(dir);
            WriteMountPointName(mount_out, "/savedata0");
            LOG_INFO(HLE, "sceSaveDataMount3 -> '/savedata0' (host: %s, out zeroed %llu bytes)",
                     dir.c_str(), (unsigned long long)kMountPointOutSize);
            return 0;
        };
        RegisterSaveDataSymbol("libSceSaveData", "sceSaveDataMount3",
                               "ZP4e7rlzOUk#G#H", Mount3);

        // sceSaveDataUmount2(const SceSaveDataMountPoint* mountPoint)
        auto Umount2 = [](const GuestArgs& args) -> u64 {
            LOG_INFO(HLE, "sceSaveDataUmount2(mountPoint: 0x%llx)", args.arg1);
            SdimgEncryptOnCommit(GetEffectiveSaveDataDir());
            return 0;
        };
        RegisterSaveDataSymbol("libSceSaveData", "sceSaveDataUmount2",
                               "uW4vfTwMQVo#G#H", Umount2);

        // sceSaveDataPrepare(...) — internal transaction helper; no outs used.
        auto Prepare = [](const GuestArgs& args) -> u64 {
            LOG_INFO(HLE, "sceSaveDataPrepare(a1: 0x%llx, a2: 0x%llx)", args.arg1, args.arg2);
            EnsureSaveDataDir();
            return 0;
        };
        RegisterSaveDataSymbol("libSceSaveData", "sceSaveDataPrepare",
                               "sDCBrmc61XU#G#H", Prepare);

        // sceSaveDataCommit(...) — data is already on the host fs; success.
        auto Commit = [](const GuestArgs& args) -> u64 {
            LOG_INFO(HLE, "sceSaveDataCommit(a1: 0x%llx)", args.arg1);
            SdimgEncryptOnCommit(GetEffectiveSaveDataDir());
            return 0;
        };
        RegisterSaveDataSymbol("libSceSaveData", "sceSaveDataCommit",
                               "ie7qhZ4X0Cc#G#H", Commit);

        // -----------------------------------------------------------------
        // sceSaveDataDirNameSearch(cond*, result*)
        // Enumerates the effective savedata dir; each host subdirectory is a
        // save dir.  Result layout: u32 hitCount at +0, then hitCount
        // SceSaveDataDirName (char[32]) entries at +4.  Entry count is capped
        // by the guest-provided maxCount (u32 at cond+0x20, 0 = no cap) and
        // by kDirSearchHardMax as a safety rail.
        // -----------------------------------------------------------------
        auto DirNameSearch = [](const GuestArgs& args) -> u64 {
            guest_addr_t cond   = args.arg1;
            guest_addr_t result = args.arg2;
            LOG_INFO(HLE, "sceSaveDataDirNameSearch(cond: 0x%llx, result: 0x%llx)", cond, result);
            if (!result) return 0;

            // Guest-provided cap (best-effort; an unreadable cond means none).
            u32 max_count = kDirSearchHardMax;
            if (cond && Memory::IsReadable(cond + kDirSearchCondMaxOff, sizeof(u32))) {
                const u32 guest_max = Memory::Read<u32>(cond + kDirSearchCondMaxOff);
                if (guest_max > 0 && guest_max < max_count) max_count = guest_max;
            }

            std::vector<std::string> names;
            std::error_code ec;
            for (const auto& entry :
                 std::filesystem::directory_iterator(GetEffectiveSaveDataDir(), ec)) {
                if (entry.is_directory(ec)) names.push_back(entry.path().filename().string());
            }
            if (ec) {
                LOG_WARN(HLE, "sceSaveDataDirNameSearch: enumerate failed: %s",
                         ec.message().c_str());
            }
            std::sort(names.begin(), names.end());
            if (names.size() > max_count) names.resize(max_count);

            const u64 need = kDirSearchResultBase + kDirNameSize * names.size();
            if (!Memory::IsWritable(result, need)) {
                LOG_WARN(HLE, "sceSaveDataDirNameSearch: result 0x%llx not writable "
                         "(%llu bytes needed) — skipped", result, (unsigned long long)need);
                return 0;
            }
            Memory::Write<u32>(result, static_cast<u32>(names.size()));
            for (size_t i = 0; i < names.size(); ++i) {
                const guest_addr_t slot = result + kDirSearchResultBase + kDirNameSize * i;
                u8 buf[kDirNameSize] = {};
                const size_t copy = std::min(names[i].size(), static_cast<size_t>(kDirNameSize - 1));
                std::memcpy(buf, names[i].data(), copy);
                Memory::WriteBuffer(slot, buf, sizeof(buf));
            }
            LOG_INFO(HLE, "sceSaveDataDirNameSearch -> %u hit(s)", (unsigned)names.size());
            return 0;
        };
        RegisterSaveDataSymbol("libSceSaveData", "sceSaveDataDirNameSearch",
                               "dyIhnXq-0SM#G#H", DirNameSearch);

        // -----------------------------------------------------------------
        // sceSaveDataCreateTransactionResource(u32* outId)
        // Hand out a fake resource id; commits are host-fs writes already.
        // -----------------------------------------------------------------
        auto CreateTransactionResource = [](const GuestArgs& args) -> u64 {
            guest_addr_t out_id = args.arg1;
            LOG_INFO(HLE, "sceSaveDataCreateTransactionResource(out: 0x%llx)", out_id);
            if (out_id) {
                // Games have been seen passing garbage here (0xc0000 during
                // the PPSA02929 boot) — probe before writing so a bad out
                // pointer is a warning, not a first-chance AV swallowed by
                // the dispatcher's SEH guard.
                if (!Memory::IsWritable(out_id, sizeof(u32))) {
                    LOG_WARN(HLE, "sceSaveDataCreateTransactionResource: out 0x%llx not writable — skipped",
                             out_id);
                    return 0;
                }
                Memory::Write<u32>(out_id, 1);
            }
            return 0;
        };
        RegisterSaveDataSymbol("libSceSaveData", "sceSaveDataCreateTransactionResource",
                               "gjRZNnw0JPE#G#H", CreateTransactionResource);

        // =================================================================
        // libSceCommonDialog / libSceSaveDataDialog (dialog-free path)
        // =================================================================
        auto CommonDialogInitialize = [](const GuestArgs& args) -> u64 {
            LOG_INFO(HLE, "sceCommonDialogInitialize(a1: 0x%llx)", args.arg1);
            return 0;
        };
        RegisterSaveDataSymbol("libSceCommonDialog", "sceCommonDialogInitialize",
                               "uoUpLGNkygk#O#P", CommonDialogInitialize);

        auto SaveDataDialogInitialize = [](const GuestArgs& args) -> u64 {
            LOG_INFO(HLE, "sceSaveDataDialogInitialize(a1: 0x%llx)", args.arg1);
            return 0;
        };
        RegisterSaveDataSymbol("libSceSaveDataDialog", "sceSaveDataDialogInitialize",
                               "s9e3+YpRnzw#H#I", SaveDataDialogInitialize);

        // SceCommonDialogStatus: 0=NONE, 1=INITIALIZED, 2=RUNNING, 3=FINISHED.
        // Report FINISHED so dialog polling loops exit immediately.
        auto SaveDataDialogGetStatus = [](const GuestArgs& /*args*/) -> u64 {
            LOG_DEBUG(HLE, "sceSaveDataDialogGetStatus() -> FINISHED");
            return 3;
        };
        RegisterSaveDataSymbol("libSceSaveDataDialog", "sceSaveDataDialogGetStatus",
                               "ERKzksauAJA#H#I", SaveDataDialogGetStatus);
    }
}
// namespace HLE
