// Discord Rich Presence — Windows named pipe IPC implementation.
//
// Protocol:
//   1. Iterate pipe numbers 0..9, try to connect to \\.\pipe\discord-ipc-{n}
//   2. Send handshake frame: {"v":1,"client_id":"<app_id>"}
//   3. Receive response with opcode 1 (READY) or 2 (CLOSE)
//   4. Send SET_ACTIVITY frames (opcode 3) with Rich Presence payload
//
// Frame format (little-endian):
//   int32  opcode
//   int32  length (payload length in bytes)
//   char[] payload (JSON, UTF-8)

#include "discord_rpc.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>   // htonl, htons
#pragma comment(lib, "ws2_32.lib")
#else
#error "DiscordRPC currently only supports Windows named pipes."
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>

namespace Ui {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::string BuildHandshakeJson(const char* app_id) {
    // {"v":1,"client_id":"<app_id>"}
    std::string json;
    json.reserve(64);
    json  = "{\"v\":1,\"client_id\":\"";
    json += app_id;
    json += "\"}";
    return json;
}

static std::string BuildPresenceJson(const char* details,
                                     const char* state,
                                     const char* large_image_key,
                                     const char* large_image_text,
                                     const char* small_image_key,
                                     const char* small_image_text,
                                     const char* party_id,
                                     int party_size,
                                     int party_max,
                                     std::int64_t start_time) {
    // Build the activity payload manually (no JSON library dependency).
    std::string json;
    json.reserve(512);
    json = "{";

    // Top-level fields
    if (details && details[0]) {
        json += "\"details\":\"";
        // Escape backslashes and double-quotes minimally.
        for (const char* p = details; *p; ++p) {
            if (*p == '\\') json += "\\\\";
            else if (*p == '"') json += "\\\"";
            else json += *p;
        }
        json += "\",";
    }
    if (state && state[0]) {
        json += "\"state\":\"";
        for (const char* p = state; *p; ++p) {
            if (*p == '\\') json += "\\\\";
            else if (*p == '"') json += "\\\"";
            else json += *p;
        }
        json += "\",";
    }

    // Timestamps
    if (start_time > 0) {
        json += "\"timestamps\":{\"start\":";
        json += std::to_string(start_time);
        json += "},";
    } else {
        // Use "now" — send current Unix timestamp.
        auto now = std::chrono::system_clock::now();
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                        now.time_since_epoch()).count();
        json += "\"timestamps\":{\"start\":";
        json += std::to_string(secs);
        json += "},";
    }

    // Assets (images)
    bool has_large = (large_image_key && large_image_key[0]);
    bool has_small = (small_image_key && small_image_key[0]);
    if (has_large || has_small) {
        json += "\"assets\":{";
        if (has_large) {
            json += "\"large_image\":\"";
            json += large_image_key;
            json += "\"";
            if (large_image_text && large_image_text[0]) {
                json += ",\"large_text\":\"";
                for (const char* p = large_image_text; *p; ++p) {
                    if (*p == '\\') json += "\\\\";
                    else if (*p == '"') json += "\\\"";
                    else json += *p;
                }
                json += "\"";
            }
        }
        if (has_small) {
            if (has_large) json += ",";
            json += "\"small_image\":\"";
            json += small_image_key;
            json += "\"";
            if (small_image_text && small_image_text[0]) {
                json += ",\"small_text\":\"";
                for (const char* p = small_image_text; *p; ++p) {
                    if (*p == '\\') json += "\\\\";
                    else if (*p == '"') json += "\\\"";
                    else json += *p;
                }
                json += "\"";
            }
        }
        json += "},";
    }

    // Party
    if (party_id && party_id[0] && party_max > 0) {
        json += "\"party\":{\"id\":\"";
        json += party_id;
        json += "\"";
        if (party_size >= 0 && party_max > 0) {
            json += ",\"size\":[";
            json += std::to_string(party_size);
            json += ",";
            json += std::to_string(party_max);
            json += "]";
        }
        json += "},";
    }

    // Remove trailing comma if present
    if (json.back() == ',') {
        json.pop_back();
    }
    json += "}";
    return json;
}

// ---------------------------------------------------------------------------
// Pipe connection
// ---------------------------------------------------------------------------

bool DiscordRPC::ConnectPipe(int num) {
    wchar_t pipe_name[64];
    swprintf_s(pipe_name, 64, L"\\\\.\\pipe\\discord-ipc-%d", num);

    HANDLE hPipe = CreateFileW(
        pipe_name,
        GENERIC_READ | GENERIC_WRITE,
        0,                  // no sharing
        nullptr,            // default security
        OPEN_EXISTING,
        0,                  // default attributes
        nullptr);           // no template

    if (hPipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Set pipe to non-blocking mode with a short timeout
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    pipe_ = hPipe;
    pipe_num_ = num;
    return true;
}

void DiscordRPC::ClosePipe() {
    if (pipe_) {
        CloseHandle(static_cast<HANDLE>(pipe_));
        pipe_ = nullptr;
        pipe_num_ = 0;
        handshake_done_ = false;
    }
}

// ---------------------------------------------------------------------------
// Frame I/O
// ---------------------------------------------------------------------------

bool DiscordRPC::SendFrame(int opcode, const std::string& payload) {
    if (!pipe_) return false;

    HANDLE hPipe = static_cast<HANDLE>(pipe_);

    // Build frame header + payload
    // Header: int32 opcode (LE), int32 length (LE)
    std::int32_t op_le = static_cast<std::int32_t>(opcode);
    std::int32_t len_le = static_cast<std::int32_t>(payload.size());

    // Write header
    DWORD written = 0;
    if (!WriteFile(hPipe, &op_le, 4, &written, nullptr) || written != 4) {
        ClosePipe();
        return false;
    }
    if (!WriteFile(hPipe, &len_le, 4, &written, nullptr) || written != 4) {
        ClosePipe();
        return false;
    }

    // Write payload
    if (!payload.empty()) {
        if (!WriteFile(hPipe, payload.data(), static_cast<DWORD>(payload.size()),
                       &written, nullptr) ||
            written != payload.size()) {
            ClosePipe();
            return false;
        }
    }

    return true;
}

std::string DiscordRPC::ReadFrame() {
    if (!pipe_) return "";

    HANDLE hPipe = static_cast<HANDLE>(pipe_);

    // Read header: opcode (4 bytes) + length (4 bytes)
    std::int32_t header[2] = {0, 0};
    DWORD read = 0;

    // Non-blocking read — if no data, return empty
    if (!ReadFile(hPipe, header, 8, &read, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_NO_DATA || err == ERROR_PIPE_NOT_CONNECTED) {
            return "";  // no data available, not an error
        }
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
            ClosePipe();
            return "";
        }
        return "";
    }

    if (read < 8) {
        // Partial header — wait for more next frame
        return "";
    }

    std::int32_t len = header[1];
    if (len < 0 || len > 65536) {
        ClosePipe();  // malformed
        return "";
    }

    if (len == 0) return "";

    // Read payload
    std::string payload(static_cast<size_t>(len), '\0');
    DWORD total_read = 0;
    while (total_read < static_cast<DWORD>(len)) {
        DWORD chunk = 0;
        if (!ReadFile(hPipe, &payload[total_read],
                      static_cast<DWORD>(len) - total_read, &chunk, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_NO_DATA) {
                // Data not ready yet — discard partial read
                return "";
            }
            ClosePipe();
            return "";
        }
        if (chunk == 0) break;
        total_read += chunk;
    }

    if (total_read < static_cast<DWORD>(len)) {
        return "";  // incomplete
    }

    return payload;
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

bool DiscordRPC::SendHandshake() {
    std::string json = BuildHandshakeJson(app_id_.c_str());
    if (!SendFrame(0, json)) return false;

    // Read response — Discord sends opcode 1 (READY) or 2 (CLOSE)
    // We poll a few times since the pipe is non-blocking
    for (int attempt = 0; attempt < 30; ++attempt) {
        std::string resp = ReadFrame();
        if (!resp.empty()) {
            // Check if it looks like a valid response (contains "cmd" or "evt")
            if (resp.find("\"cmd\"") != std::string::npos ||
                resp.find("\"evt\"") != std::string::npos) {
                handshake_done_ = true;
                return true;
            }
            // If we got a CLOSE frame, fail
            if (resp.find("\"code\"") != std::string::npos) {
                ClosePipe();
                return false;
            }
        }
        Sleep(50);
    }

    // If we got no response but pipe is still open, assume success
    // (Discord sometimes doesn't send READY immediately)
    if (pipe_) {
        handshake_done_ = true;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

DiscordRPC::~DiscordRPC() {
    Stop();
}

bool DiscordRPC::Start(const char* app_id) {
    if (pipe_) return true;  // already connected

    app_id_ = app_id ? app_id : kDefaultAppId;

    // Try pipe numbers 0..9 (Discord uses 0 by default, up to 9)
    for (int n = 0; n < 10; ++n) {
        if (ConnectPipe(n)) {
            if (SendHandshake()) {
                return true;
            }
            // Handshake failed, try next pipe
            ClosePipe();
        }
    }

    return false;
}

void DiscordRPC::Stop() {
    if (pipe_ && handshake_done_) {
        // Clear presence before disconnecting
        SendFrame(3, "{}");
        Sleep(50);
    }
    ClosePipe();
}

void DiscordRPC::UpdatePresence(const char* details,
                                const char* state,
                                const char* large_image_key,
                                const char* large_image_text,
                                const char* small_image_key,
                                const char* small_image_text,
                                const char* party_id,
                                int party_size,
                                int party_max,
                                std::int64_t start_time) {
    if (!pipe_ || !handshake_done_) return;

    std::string activity = BuildPresenceJson(
        details, state, large_image_key, large_image_text,
        small_image_key, small_image_text,
        party_id, party_size, party_max, start_time);

    // Wrap in SET_ACTIVITY command
    // {"cmd":"SET_ACTIVITY","args":{"pid":<pid>,"activity":<activity>},"nonce":"<uuid>"}
    DWORD pid = GetCurrentProcessId();
    std::string frame;
    frame.reserve(activity.size() + 128);
    frame  = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":";
    frame += std::to_string(pid);
    frame += ",\"activity\":";
    frame += activity;
    frame += "},\"nonce\":\"";
    // Simple nonce from tick count
    frame += std::to_string(GetTickCount64());
    frame += "\"}";

    SendFrame(1, frame);  // opcode 1 = FRAME
}

void DiscordRPC::ClearPresence() {
    if (!pipe_ || !handshake_done_) return;

    DWORD pid = GetCurrentProcessId();
    std::string frame;
    frame  = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":";
    frame += std::to_string(pid);
    frame += "},\"nonce\":\"";
    frame += std::to_string(GetTickCount64());
    frame += "\"}";

    SendFrame(1, frame);
}

void DiscordRPC::Pump() {
    if (!pipe_) return;

    // Drain any pending messages from Discord
    std::string msg;
    int safety = 0;
    while (safety < 10) {
        msg = ReadFrame();
        if (msg.empty()) break;
        ++safety;
    }
}

}  // namespace Ui