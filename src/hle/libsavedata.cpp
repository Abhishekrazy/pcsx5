#include "hle.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace HLE {

    // -------------------------------------------------------------------------
    // Save-data host backing store.
    //
    // Mounts are backed by a real host directory so save data persists across
    // runs:  <cwd>/pcsx5_savedata/<title-id>/
    // The title id comes from --title-id (see main.cpp); "PPSA02929" is the
    // fallback when the emulator is launched without one.
    // -------------------------------------------------------------------------
    namespace {
        std::string g_savedata_title_id = "PPSA02929";

        // sceSaveDataMountPoint: char data[16] on PS4/PS5 — the mount name.
        // We zero 32 bytes (generous) before writing so padding reads as 0.
        constexpr u64 kMountPointOutSize = 32;
        // sceSaveDataDirNameSearch result buffers are game-sized; we only
        // promise the leading hitCount field (u32 at offset 0) plus a zeroed
        // scratch region so partial structs never expose garbage.
        constexpr u64 kDirSearchZeroSize = 0x40;

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
            const std::string dir = EnsureSaveDataDir();
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
            return 0;
        };
        RegisterSaveDataSymbol("libSceSaveData", "sceSaveDataCommit",
                               "ie7qhZ4X0Cc#G#H", Commit);

        // -----------------------------------------------------------------
        // sceSaveDataDirNameSearch(cond*, result*)
        // Report an empty result set: hitCount (u32 at result+0) = 0, leading
        // bytes zeroed.  The game treats "no saves found" as a fresh profile.
        // -----------------------------------------------------------------
        auto DirNameSearch = [](const GuestArgs& args) -> u64 {
            guest_addr_t cond   = args.arg1;
            guest_addr_t result = args.arg2;
            LOG_INFO(HLE, "sceSaveDataDirNameSearch(cond: 0x%llx, result: 0x%llx)", cond, result);
            if (result) {
                if (!Memory::IsWritable(result, kDirSearchZeroSize)) {
                    LOG_WARN(HLE, "sceSaveDataDirNameSearch: result 0x%llx not writable — skipped", result);
                    return 0;
                }
                u8 zero[kDirSearchZeroSize] = {};
                Memory::WriteBuffer(result, zero, sizeof(zero));
                LOG_INFO(HLE, "sceSaveDataDirNameSearch -> 0 hits (zeroed %llu bytes)",
                         (unsigned long long)kDirSearchZeroSize);
            }
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
