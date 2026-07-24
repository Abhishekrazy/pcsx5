// libSceNet + libSceNetCtl HLE — offline network stubs.
//
// PS5 games always init libSceNet and libSceNetCtl at boot, then typically
// check if the console is "online" before proceeding.  We fake:
//   - Network stack: initialized successfully, no actual sockets.
//   - NetCtl state: CONNECTED (state=3) — this stops games from blocking
//     in a retry loop waiting for network.
//   - Socket calls: return dummy handles; all actual I/O returns "disconnected".
//
// This matches the approach used by shadPS4 and KytyPS5 for offline emulation.
#include "hle.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <atomic>
#include <cstring>

namespace HLE {

namespace {

constexpr u64 ORBIS_OK                          = 0;
constexpr u64 ORBIS_NET_ERROR_ENOTCONN          = 0x80410045;
constexpr u64 ORBIS_NET_ERROR_ENOTSUP           = 0x80410005;
constexpr u64 ORBIS_NET_ERROR_EINVAL            = 0x80410016;
constexpr u64 ORBIS_NETCTL_ERROR_NOT_INITIALIZED = 0x80910001;

// Monotonically-increasing dummy socket fd counter
std::atomic<s32> g_next_sockfd{200};

// NetCtl states (SCE_NET_CTL_STATE_*)
constexpr s32 SCE_NET_CTL_STATE_DISCONNECTED = 1;
constexpr s32 SCE_NET_CTL_STATE_CONNECTING   = 2;
constexpr s32 SCE_NET_CTL_STATE_CONNECTED    = 3;
constexpr s32 SCE_NET_CTL_STATE_IPOBTAINED   = 4;

bool g_netctl_initialized = false;
bool g_net_initialized    = false;

} // namespace

void RegisterLibNet() {
    LOG_INFO(HLE, "Registering libSceNet + libSceNetCtl HLE symbols...");

    // ====================================================================
    // libSceNet — network stack lifecycle
    // ====================================================================

    // sceNetInit — init network subsystem.
    auto NetInitImpl = [](const GuestArgs& args) -> u64 {
        g_net_initialized = true;
        LOG_DEBUG(HLE, "sceNetInit(param: 0x%llx) -> 0 (offline)", args.arg1);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceNet", "sceNetInit",     NetInitImpl);
    RegisterSymbol("libSceNet", "sceNetInit#T#T", NetInitImpl);

    // sceNetTerm — shutdown network subsystem.
    auto NetTermImpl = [](const GuestArgs&) -> u64 {
        g_net_initialized = false;
        LOG_DEBUG(HLE, "sceNetTerm() -> 0");
        return ORBIS_OK;
    };
    RegisterSymbol("libSceNet", "sceNetTerm",     NetTermImpl);
    RegisterSymbol("libSceNet", "sceNetTerm#T#T", NetTermImpl);

    // sceNetSocket — return dummy fd.
    auto SocketImpl = [](const GuestArgs& args) -> u64 {
        const s32 fd = g_next_sockfd.fetch_add(1);
        LOG_DEBUG(HLE, "sceNetSocket(domain: %llu, type: %llu, proto: %llu) -> fd=%d (dummy)",
                  args.arg2, args.arg3, args.arg4, fd);
        return static_cast<u64>(static_cast<u32>(fd));
    };
    RegisterSymbol("libSceNet", "sceNetSocket",     SocketImpl);
    RegisterSymbol("libSceNet", "sceNetSocket#T#T", SocketImpl);

    // sceNetClose — close socket (no-op; return 0).
    auto CloseImpl = [](const GuestArgs& args) -> u64 {
        LOG_DEBUG(HLE, "sceNetClose(fd: %llu) -> 0", args.arg1);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceNet", "sceNetClose",     CloseImpl);
    RegisterSymbol("libSceNet", "sceNetClose#T#T", CloseImpl);

    // sceNetConnect — offline: return ENOTCONN.
    auto ConnectImpl = [](const GuestArgs& args) -> u64 {
        LOG_DEBUG(HLE, "sceNetConnect(fd: %llu) -> ENOTCONN (offline)", args.arg1);
        return ORBIS_NET_ERROR_ENOTCONN;
    };
    RegisterSymbol("libSceNet", "sceNetConnect",     ConnectImpl);
    RegisterSymbol("libSceNet", "sceNetConnect#T#T", ConnectImpl);

    // sceNetBind — return success (no actual bind).
    auto BindImpl = [](const GuestArgs& args) -> u64 {
        LOG_DEBUG(HLE, "sceNetBind(fd: %llu) -> 0 (stub)", args.arg1);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceNet", "sceNetBind",     BindImpl);
    RegisterSymbol("libSceNet", "sceNetBind#T#T", BindImpl);

    // sceNetSend / sceNetSendto — offline: return ENOTCONN.
    auto SendImpl = [](const GuestArgs& /*args*/) -> u64 {
        return ORBIS_NET_ERROR_ENOTCONN;
    };
    RegisterSymbol("libSceNet", "sceNetSend",       SendImpl);
    RegisterSymbol("libSceNet", "sceNetSend#T#T",   SendImpl);
    RegisterSymbol("libSceNet", "sceNetSendto",     SendImpl);
    RegisterSymbol("libSceNet", "sceNetSendto#T#T", SendImpl);

    // sceNetRecv / sceNetRecvfrom — offline: return ENOTCONN.
    auto RecvImpl = [](const GuestArgs& /*args*/) -> u64 {
        return ORBIS_NET_ERROR_ENOTCONN;
    };
    RegisterSymbol("libSceNet", "sceNetRecv",        RecvImpl);
    RegisterSymbol("libSceNet", "sceNetRecv#T#T",    RecvImpl);
    RegisterSymbol("libSceNet", "sceNetRecvfrom",    RecvImpl);
    RegisterSymbol("libSceNet", "sceNetRecvfrom#T#T",RecvImpl);

    // sceNetGetsockopt / sceNetSetsockopt — return 0 (no-op).
    auto SockoptImpl = [](const GuestArgs&) -> u64 { return ORBIS_OK; };
    RegisterSymbol("libSceNet", "sceNetGetsockopt",     SockoptImpl);
    RegisterSymbol("libSceNet", "sceNetGetsockopt#T#T", SockoptImpl);
    RegisterSymbol("libSceNet", "sceNetSetsockopt",     SockoptImpl);
    RegisterSymbol("libSceNet", "sceNetSetsockopt#T#T", SockoptImpl);

    // sceNetSelect / sceNetPoll — return 0 (nothing ready).
    RegisterSymbol("libSceNet", "sceNetSelect",     [](const GuestArgs&) -> u64 { return 0; });
    RegisterSymbol("libSceNet", "sceNetSelect#T#T", [](const GuestArgs&) -> u64 { return 0; });
    RegisterSymbol("libSceNet", "sceNetPoll",       [](const GuestArgs&) -> u64 { return 0; });
    RegisterSymbol("libSceNet", "sceNetPoll#T#T",   [](const GuestArgs&) -> u64 { return 0; });

    // sceNetGetpeername / sceNetGetsockname — return ENOTCONN.
    RegisterSymbol("libSceNet", "sceNetGetpeername",     [](const GuestArgs&) -> u64 { return ORBIS_NET_ERROR_ENOTCONN; });
    RegisterSymbol("libSceNet", "sceNetGetpeername#T#T", [](const GuestArgs&) -> u64 { return ORBIS_NET_ERROR_ENOTCONN; });
    RegisterSymbol("libSceNet", "sceNetGetsockname",     [](const GuestArgs&) -> u64 { return ORBIS_NET_ERROR_ENOTCONN; });
    RegisterSymbol("libSceNet", "sceNetGetsockname#T#T", [](const GuestArgs&) -> u64 { return ORBIS_NET_ERROR_ENOTCONN; });

    // sceNetHtons / sceNetHtonl / sceNetNtohs / sceNetNtohl — byte-order helpers.
    RegisterSymbol("libSceNet", "sceNetHtons",
                   [](const GuestArgs& a) -> u64 { return (a.arg1 >> 8) | ((a.arg1 & 0xFF) << 8); });
    RegisterSymbol("libSceNet", "sceNetHtons#T#T",
                   [](const GuestArgs& a) -> u64 { return (a.arg1 >> 8) | ((a.arg1 & 0xFF) << 8); });
    RegisterSymbol("libSceNet", "sceNetHtonl",
                   [](const GuestArgs& a) -> u64 {
                       u32 v = static_cast<u32>(a.arg1);
                       return ((v>>24)&0xFF)|((v>>8)&0xFF00)|((v&0xFF00)<<8)|((v&0xFF)<<24);
                   });
    RegisterSymbol("libSceNet", "sceNetHtonl#T#T",
                   [](const GuestArgs& a) -> u64 {
                       u32 v = static_cast<u32>(a.arg1);
                       return ((v>>24)&0xFF)|((v>>8)&0xFF00)|((v&0xFF00)<<8)|((v&0xFF)<<24);
                   });
    // Ntohs/Ntohl same as Htons/Htonl (symmetric)
    RegisterSymbol("libSceNet", "sceNetNtohs",
                   [](const GuestArgs& a) -> u64 { return (a.arg1 >> 8) | ((a.arg1 & 0xFF) << 8); });
    RegisterSymbol("libSceNet", "sceNetNtohs#T#T",
                   [](const GuestArgs& a) -> u64 { return (a.arg1 >> 8) | ((a.arg1 & 0xFF) << 8); });
    RegisterSymbol("libSceNet", "sceNetNtohl",
                   [](const GuestArgs& a) -> u64 {
                       u32 v = static_cast<u32>(a.arg1);
                       return ((v>>24)&0xFF)|((v>>8)&0xFF00)|((v&0xFF00)<<8)|((v&0xFF)<<24);
                   });
    RegisterSymbol("libSceNet", "sceNetNtohl#T#T",
                   [](const GuestArgs& a) -> u64 {
                       u32 v = static_cast<u32>(a.arg1);
                       return ((v>>24)&0xFF)|((v>>8)&0xFF00)|((v&0xFF00)<<8)|((v&0xFF)<<24);
                   });

    // sceNetInetPton — parse IP string, return 1 on success (fill with loopback).
    auto InetPtonImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t dst_ptr = args.arg3;
        if (dst_ptr) {
            // Write 127.0.0.1 as default (4 bytes for AF_INET)
            Memory::Write<u32>(dst_ptr, 0x0100007F); // 127.0.0.1 little-endian
        }
        return 1; // INET_PTON success
    };
    RegisterSymbol("libSceNet", "sceNetInetPton",     InetPtonImpl);
    RegisterSymbol("libSceNet", "sceNetInetPton#T#T", InetPtonImpl);

    // sceNetInetNtop — convert IP to string "127.0.0.1".
    auto InetNtopImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t dst_ptr = args.arg3;
        const u64          dst_len = args.arg4;
        if (dst_ptr && dst_len >= 10) {
            const char ip[] = "127.0.0.1";
            for (size_t i = 0; i <= sizeof(ip); ++i) {
                Memory::Write<u8>(dst_ptr + i, static_cast<u8>(ip[i]));
            }
        }
        return dst_ptr; // returns pointer on success
    };
    RegisterSymbol("libSceNet", "sceNetInetNtop",     InetNtopImpl);
    RegisterSymbol("libSceNet", "sceNetInetNtop#T#T", InetNtopImpl);

    // ====================================================================
    // libSceNetCtl — network control
    // ====================================================================

    // sceNetCtlInit — init NetCtl subsystem.
    auto NetCtlInitImpl = [](const GuestArgs&) -> u64 {
        g_netctl_initialized = true;
        LOG_DEBUG(HLE, "sceNetCtlInit() -> 0");
        return ORBIS_OK;
    };
    RegisterSymbol("libSceNetCtl", "sceNetCtlInit",     NetCtlInitImpl);
    RegisterSymbol("libSceNetCtl", "sceNetCtlInit#T#T", NetCtlInitImpl);

    // sceNetCtlTerm — shutdown NetCtl.
    auto NetCtlTermImpl = [](const GuestArgs&) -> u64 {
        g_netctl_initialized = false;
        LOG_DEBUG(HLE, "sceNetCtlTerm() -> 0");
        return ORBIS_OK;
    };
    RegisterSymbol("libSceNetCtl", "sceNetCtlTerm",     NetCtlTermImpl);
    RegisterSymbol("libSceNetCtl", "sceNetCtlTerm#T#T", NetCtlTermImpl);

    // sceNetCtlGetState — return CONNECTED (state=4: IP obtained) so games
    // don't spin in a retry loop.
    // Prototype: int sceNetCtlGetState(int* state)
    auto GetStateImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t state_ptr = args.arg1;
        if (!state_ptr) return ORBIS_NETCTL_ERROR_NOT_INITIALIZED;
        Memory::Write<s32>(state_ptr, SCE_NET_CTL_STATE_IPOBTAINED);
        LOG_DEBUG(HLE, "sceNetCtlGetState(0x%llx) -> state=4 (IP_OBTAINED)", state_ptr);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceNetCtl", "sceNetCtlGetState",     GetStateImpl);
    RegisterSymbol("libSceNetCtl", "sceNetCtlGetState#T#T", GetStateImpl);

    // sceNetCtlCheckCallback — process pending callbacks (no-op; return 0).
    auto CheckCbImpl = [](const GuestArgs&) -> u64 {
        return ORBIS_OK;
    };
    RegisterSymbol("libSceNetCtl", "sceNetCtlCheckCallback",     CheckCbImpl);
    RegisterSymbol("libSceNetCtl", "sceNetCtlCheckCallback#T#T", CheckCbImpl);

    // sceNetCtlGetInfo — return a minimal info struct (IP=127.0.0.1).
    // Prototype: int sceNetCtlGetInfo(int code, SceNetCtlInfo* info)
    auto GetInfoImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t info_ptr = args.arg2;
        if (!info_ptr) return ORBIS_NETCTL_ERROR_NOT_INITIALIZED;
        // Zero the union first (128 bytes)
        for (int i = 0; i < 128; i += 8) Memory::Write<u64>(info_ptr + i, 0);
        // For code=8 (IP address): write "127.0.0.1" string at offset 0
        const char ip[] = "127.0.0.1";
        for (size_t i = 0; i <= sizeof(ip); ++i) {
            Memory::Write<u8>(info_ptr + i, static_cast<u8>(ip[i]));
        }
        LOG_DEBUG(HLE, "sceNetCtlGetInfo(code: %llu, 0x%llx) -> IP=127.0.0.1", args.arg1, info_ptr);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceNetCtl", "sceNetCtlGetInfo",     GetInfoImpl);
    RegisterSymbol("libSceNetCtl", "sceNetCtlGetInfo#T#T", GetInfoImpl);

    // sceNetCtlRegisterCallback / sceNetCtlUnregisterCallback — stub.
    auto RegCbImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t id_ptr = args.arg3;
        if (id_ptr) Memory::Write<s32>(id_ptr, 1); // hand back dummy handle
        return ORBIS_OK;
    };
    RegisterSymbol("libSceNetCtl", "sceNetCtlRegisterCallback",     RegCbImpl);
    RegisterSymbol("libSceNetCtl", "sceNetCtlRegisterCallback#T#T", RegCbImpl);

    auto UnregCbImpl = [](const GuestArgs&) -> u64 { return ORBIS_OK; };
    RegisterSymbol("libSceNetCtl", "sceNetCtlUnregisterCallback",     UnregCbImpl);
    RegisterSymbol("libSceNetCtl", "sceNetCtlUnregisterCallback#T#T", UnregCbImpl);
}

} // namespace HLE
