// libSceAtrac9 HLE — ATRAC9 codec path (ROADMAP Phase 5 M3).
//
// Games decode their music/voice streams through libSceAtrac9.  We back it
// with the vendored LibAtrac9 (RPCSX fork; see third_party/LibAtrac9 and the
// proven decode flow in src/ui/snd_player.cpp).
//
// Implemented exports (module name "libSceAtrac9"):
//
//   sceAtrac9InitHandle(pConfigData, pHandle, pFormatType)
//     pConfigData points at either the 4-byte ATRAC9 config blob or a full
//     RIFF/WAVE (.at9) stream, in which case the config is extracted from the
//     fmt chunk (same layout as snd_player.cpp).  Returns a small s32 handle.
//   sceAtrac9ReleaseHandle(handle)
//   sceAtrac9Decode(handle, pAtrac9Buffer, pPcmBuffer, pNBytesUsed)
//     Decodes ONE frame; PCM16 interleaved into the guest buffer.
//   sceAtrac9GetInfoType(handle, infoType, pInfo)
//     Frame/codec info queries (see kInfo* below).
//   sceAtrac9GetInternalErrorInfo(handle, pInfo, infoSize)
//     Copies the last LibAtrac9 status for the handle.
//
// Neither assets/nid_db.txt nor SharpEmu names these NIDs, so anything beyond
// this list keeps the auto-stub behaviour from hle.cpp.  Error codes use the
// 0x80A6xxxx family libSceAtrac9 is known to use; exact per-error values are
// our own assignment (no public spec), chosen stable and logged on failure.
#include "hle.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include "libatrac9/libatrac9.h"
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace HLE {

namespace {

// libSceAtrac9 error family (see file header note on values).
constexpr u64 SCE_ATRAC9_OK                   = 0;
constexpr u64 SCE_ATRAC9_ERROR_INVALID_ARG    = 0x80A60001;
constexpr u64 SCE_ATRAC9_ERROR_INVALID_HANDLE = 0x80A60002;
constexpr u64 SCE_ATRAC9_ERROR_INVALID_CONFIG = 0x80A60003;
constexpr u64 SCE_ATRAC9_ERROR_DECODE_FAILED  = 0x80A60004;

// Format type reported through sceAtrac9InitHandle's out param.  We only
// support plain ATRAC9 (AT9); there is no public constant, so 0 = AT9.
constexpr u32 SCE_ATRAC9_FORMAT_AT9 = 0;

// sceAtrac9GetInfoType selectors (own assignment; stable for guests that
// only pass them through).
constexpr u32 kInfoCodec = 0;   // SceAtrac9CodecInfo (below)
constexpr u32 kInfoBitrate = 1; // u32, bits per second
constexpr u32 kInfoSuperframeSize = 2; // u32, compressed bytes per superframe
constexpr u32 kInfoFrameSamples = 3;   // u32, PCM samples per frame (per channel)

struct SceAtrac9CodecInfo {
    u32 sampling_rate;
    u32 channels;
    u32 frame_samples;        // per frame, per channel
    u32 frames_in_superframe;
    u32 superframe_size;      // compressed bytes
    u32 bitrate;              // bits per second
    u8  config_data[ATRAC9_CONFIG_DATA_SIZE];
    u32 reserved;
};

// ATRAC9 subformat GUID (KSDATAFORMAT_SUBTYPE_ATRAC9), same 16 bytes as
// snd_player.cpp — an RIFF stream is only accepted when these match.
constexpr u8 kAtrac9SubFormat[16] = {
    0xD2, 0x42, 0xE1, 0x47, 0xBA, 0x36, 0x8D, 0x4D,
    0x88, 0xFC, 0x61, 0x65, 0x4F, 0x8C, 0x83, 0x6C,
};

struct Atrac9Entry {
    void*           decoder = nullptr;
    Atrac9CodecInfo info{};
    u32             last_status = 0; // last LibAtrac9 result code
};

std::mutex                   g_atrac9_mutex;
std::unordered_map<s32, Atrac9Entry> g_atrac9_handles;
s32                          g_atrac9_next_handle = 1;

void SetLastError(Atrac9Entry& e, int status) {
    e.last_status = status ? static_cast<u32>(status) : 0;
}

// Scan a RIFF/WAVE image for the fmt chunk and extract the 4-byte ATRAC9
// config at fmt+44 (layout documented in src/ui/snd_player.cpp).
bool ParseRiffConfig(const std::vector<u8>& file, u8 config_out[ATRAC9_CONFIG_DATA_SIZE]) {
    if (file.size() < 12 ||
        std::memcmp(file.data(), "RIFF", 4) != 0 ||
        std::memcmp(file.data() + 8, "WAVE", 4) != 0) {
        return false;
    }
    std::size_t off = 12;
    while (off + 8 <= file.size()) {
        u32 chunk_size = 0;
        std::memcpy(&chunk_size, file.data() + off + 4, sizeof(chunk_size));
        const std::size_t payload = off + 8;
        if (payload + chunk_size > file.size()) {
            return false;
        }
        if (std::memcmp(file.data() + off, "fmt ", 4) == 0) {
            if (chunk_size < 48 ||
                std::memcmp(file.data() + payload + 24, kAtrac9SubFormat, 16) != 0) {
                return false;
            }
            std::memcpy(config_out, file.data() + payload + 44, ATRAC9_CONFIG_DATA_SIZE);
            return true;
        }
        // Chunks are word-aligned.
        off = payload + chunk_size + (chunk_size & 1u);
    }
    return false;
}

// sceAtrac9InitHandle(const u8* pConfigData, s32* pHandle, u32* pFormatType)
u64 Atrac9InitHandleImpl(const GuestArgs& args) {
    const guest_addr_t config_addr = args.arg1;
    const guest_addr_t handle_out  = args.arg2;
    const guest_addr_t type_out    = args.arg3;
    if (!config_addr || !handle_out) {
        LOG_WARN(HLE, "sceAtrac9InitHandle: bad args (config=0x%llx handle_out=0x%llx)",
                 config_addr, handle_out);
        return SCE_ATRAC9_ERROR_INVALID_ARG;
    }

    // Peek enough for either the 4-byte config or a RIFF header.  64 bytes
    // covers RIFF(12) + a fmt chunk with the GUID + config at offset 44.
    u8 probe[64];
    Memory::ReadBuffer(config_addr, probe, sizeof(probe));

    u8 config[ATRAC9_CONFIG_DATA_SIZE];
    bool from_riff = false;
    if (std::memcmp(probe, "RIFF", 4) == 0) {
        // Full header: read up to 4 KiB so a padded fmt chunk still fits.
        u32 riff_size = 0;
        std::memcpy(&riff_size, probe + 4, sizeof(riff_size));
        const u64 header_bytes = (riff_size >= 8 && riff_size <= 4096)
                                     ? (riff_size + 8)
                                     : 4096;
        std::vector<u8> head(header_bytes);
        Memory::ReadBuffer(config_addr, head.data(), header_bytes);
        if (!ParseRiffConfig(head, config)) {
            LOG_WARN(HLE, "sceAtrac9InitHandle: RIFF image is not ATRAC9");
            return SCE_ATRAC9_ERROR_INVALID_CONFIG;
        }
        from_riff = true;
    } else {
        std::memcpy(config, probe, ATRAC9_CONFIG_DATA_SIZE);
    }

    void* decoder = Atrac9GetHandle();
    if (!decoder) {
        LOG_ERROR(HLE, "sceAtrac9InitHandle: Atrac9GetHandle failed");
        return SCE_ATRAC9_ERROR_INVALID_CONFIG;
    }
    if (Atrac9InitDecoder(decoder, config) != 0) {
        Atrac9ReleaseHandle(decoder);
        LOG_WARN(HLE, "sceAtrac9InitHandle: bad config %02X %02X %02X %02X",
                 config[0], config[1], config[2], config[3]);
        return SCE_ATRAC9_ERROR_INVALID_CONFIG;
    }

    Atrac9Entry entry;
    entry.decoder = decoder;
    if (Atrac9GetCodecInfo(decoder, &entry.info) != 0) {
        Atrac9ReleaseHandle(decoder);
        return SCE_ATRAC9_ERROR_INVALID_CONFIG;
    }

    s32 handle = 0;
    {
        std::lock_guard<std::mutex> lock(g_atrac9_mutex);
        handle = g_atrac9_next_handle++;
        g_atrac9_handles.emplace(handle, entry);
    }

    Memory::WriteBuffer(handle_out, &handle, sizeof(handle));
    if (type_out) {
        const u32 fmt_type = SCE_ATRAC9_FORMAT_AT9;
        Memory::WriteBuffer(type_out, &fmt_type, sizeof(fmt_type));
    }
    LOG_INFO(HLE, "sceAtrac9InitHandle: handle=%d %s rate=%d ch=%d frameSamples=%d superframe=%dB",
             handle, from_riff ? "riff" : "raw",
             entry.info.samplingRate, entry.info.channels,
             entry.info.frameSamples, entry.info.superframeSize);
    return SCE_ATRAC9_OK;
}

// sceAtrac9ReleaseHandle(s32 handle)
u64 Atrac9ReleaseHandleImpl(const GuestArgs& args) {
    const s32 handle = static_cast<s32>(args.arg1);
    Atrac9Entry entry;
    {
        std::lock_guard<std::mutex> lock(g_atrac9_mutex);
        auto it = g_atrac9_handles.find(handle);
        if (it == g_atrac9_handles.end()) {
            LOG_WARN(HLE, "sceAtrac9ReleaseHandle: unknown handle %d", handle);
            return SCE_ATRAC9_ERROR_INVALID_HANDLE;
        }
        entry = it->second;
        g_atrac9_handles.erase(it);
    }
    Atrac9ReleaseHandle(entry.decoder);
    return SCE_ATRAC9_OK;
}

// sceAtrac9Decode(s32 handle, const u8* pAtrac9Buffer, s16* pPcmBuffer,
//                 s32* pNBytesUsed)
// Decodes one frame of PCM16.  Guest input holds at least one superframe of
// compressed data; LibAtrac9 reports how many bytes the frame consumed.
u64 Atrac9DecodeImpl(const GuestArgs& args) {
    const s32 handle               = static_cast<s32>(args.arg1);
    const guest_addr_t at9_addr    = args.arg2;
    const guest_addr_t pcm_addr    = args.arg3;
    const guest_addr_t used_out    = args.arg4;
    if (!at9_addr || !pcm_addr) {
        return SCE_ATRAC9_ERROR_INVALID_ARG;
    }

    Atrac9Entry entry;
    {
        std::lock_guard<std::mutex> lock(g_atrac9_mutex);
        auto it = g_atrac9_handles.find(handle);
        if (it == g_atrac9_handles.end()) {
            LOG_WARN(HLE, "sceAtrac9Decode: unknown handle %d", handle);
            return SCE_ATRAC9_ERROR_INVALID_HANDLE;
        }
        entry = it->second;
    }

    const Atrac9CodecInfo& info = entry.info;
    if (info.superframeSize <= 0 || info.framesInSuperframe <= 0 ||
        info.frameSamples <= 0 || info.channels <= 0) {
        return SCE_ATRAC9_ERROR_INVALID_HANDLE;
    }
    const u32 frame_bytes = static_cast<u32>(info.superframeSize) /
                            static_cast<u32>(info.framesInSuperframe);

    // +0x10 read slack: LibAtrac9's bit reader over-reads a few bytes past
    // the frame (same workaround as snd_player.cpp).
    constexpr u64 kReadSlack = 0x10;
    std::vector<u8> frame(frame_bytes + kReadSlack, 0);
    Memory::ReadBuffer(at9_addr, frame.data(), frame_bytes);

    const u64 pcm_bytes = static_cast<u64>(info.frameSamples) *
                          static_cast<u64>(info.channels) * 2;
    std::vector<u8> pcm(pcm_bytes);

    int n_bytes_used = 0;
    const int status = Atrac9Decode(entry.decoder, frame.data(), pcm.data(),
                                    kAtrac9FormatS16, &n_bytes_used);
    {
        std::lock_guard<std::mutex> lock(g_atrac9_mutex);
        auto it = g_atrac9_handles.find(handle);
        if (it != g_atrac9_handles.end()) {
            SetLastError(it->second, status);
        }
    }
    if (status != 0) {
        LOG_WARN(HLE, "sceAtrac9Decode: frame failed (handle=%d status=0x%X)",
                 handle, static_cast<u32>(status));
        return SCE_ATRAC9_ERROR_DECODE_FAILED;
    }

    Memory::WriteBuffer(pcm_addr, pcm.data(), pcm_bytes);
    if (used_out) {
        Memory::WriteBuffer(used_out, &n_bytes_used, sizeof(n_bytes_used));
    }
    return SCE_ATRAC9_OK;
}

// sceAtrac9GetInfoType(s32 handle, s32 infoType, void* pInfo)
u64 Atrac9GetInfoTypeImpl(const GuestArgs& args) {
    const s32 handle      = static_cast<s32>(args.arg1);
    const u32 info_type   = static_cast<u32>(args.arg2);
    const guest_addr_t out = args.arg3;
    if (!out) {
        return SCE_ATRAC9_ERROR_INVALID_ARG;
    }

    Atrac9Entry entry;
    {
        std::lock_guard<std::mutex> lock(g_atrac9_mutex);
        auto it = g_atrac9_handles.find(handle);
        if (it == g_atrac9_handles.end()) {
            return SCE_ATRAC9_ERROR_INVALID_HANDLE;
        }
        entry = it->second;
    }
    const Atrac9CodecInfo& info = entry.info;
    const u64 superframe_samples =
        static_cast<u64>(info.frameSamples) * static_cast<u64>(info.framesInSuperframe);
    const u32 bitrate = superframe_samples
        ? static_cast<u32>(static_cast<u64>(info.samplingRate) *
                           static_cast<u64>(info.superframeSize) * 8ull /
                           superframe_samples)
        : 0;

    switch (info_type) {
    case kInfoCodec: {
        SceAtrac9CodecInfo ci{};
        ci.sampling_rate        = static_cast<u32>(info.samplingRate);
        ci.channels             = static_cast<u32>(info.channels);
        ci.frame_samples        = static_cast<u32>(info.frameSamples);
        ci.frames_in_superframe = static_cast<u32>(info.framesInSuperframe);
        ci.superframe_size      = static_cast<u32>(info.superframeSize);
        ci.bitrate              = bitrate;
        std::memcpy(ci.config_data, info.configData, ATRAC9_CONFIG_DATA_SIZE);
        Memory::WriteBuffer(out, &ci, sizeof(ci));
        return SCE_ATRAC9_OK;
    }
    case kInfoBitrate:
        Memory::WriteBuffer(out, &bitrate, sizeof(bitrate));
        return SCE_ATRAC9_OK;
    case kInfoSuperframeSize: {
        const u32 v = static_cast<u32>(info.superframeSize);
        Memory::WriteBuffer(out, &v, sizeof(v));
        return SCE_ATRAC9_OK;
    }
    case kInfoFrameSamples: {
        const u32 v = static_cast<u32>(info.frameSamples);
        Memory::WriteBuffer(out, &v, sizeof(v));
        return SCE_ATRAC9_OK;
    }
    default:
        LOG_WARN(HLE, "sceAtrac9GetInfoType: unknown infoType %u (handle=%d)",
                 info_type, handle);
        return SCE_ATRAC9_ERROR_INVALID_ARG;
    }
}

// sceAtrac9GetInternalErrorInfo(s32 handle, void* pInfo, u64 infoSize)
// Copies up to infoSize bytes of {u32 last_status} for the handle.
u64 Atrac9GetInternalErrorInfoImpl(const GuestArgs& args) {
    const s32 handle       = static_cast<s32>(args.arg1);
    const guest_addr_t out = args.arg2;
    const u64 size         = args.arg3;
    if (!out || size < sizeof(u32)) {
        return SCE_ATRAC9_ERROR_INVALID_ARG;
    }
    u32 last = 0;
    {
        std::lock_guard<std::mutex> lock(g_atrac9_mutex);
        auto it = g_atrac9_handles.find(handle);
        if (it == g_atrac9_handles.end()) {
            return SCE_ATRAC9_ERROR_INVALID_HANDLE;
        }
        last = it->second.last_status;
    }
    Memory::WriteBuffer(out, &last, sizeof(last));
    return SCE_ATRAC9_OK;
}

} // namespace

void RegisterLibAtrac9() {
    LOG_INFO(HLE, "Registering libSceAtrac9 HLE symbols...");
    RegisterSymbol("libSceAtrac9", "sceAtrac9InitHandle", Atrac9InitHandleImpl);
    RegisterSymbol("libSceAtrac9", "sceAtrac9ReleaseHandle", Atrac9ReleaseHandleImpl);
    RegisterSymbol("libSceAtrac9", "sceAtrac9Decode", Atrac9DecodeImpl);
    RegisterSymbol("libSceAtrac9", "sceAtrac9GetInfoType", Atrac9GetInfoTypeImpl);
    RegisterSymbol("libSceAtrac9", "sceAtrac9GetInternalErrorInfo",
                   Atrac9GetInternalErrorInfoImpl);
}

} // namespace HLE
