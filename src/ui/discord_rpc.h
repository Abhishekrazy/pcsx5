// Discord Rich Presence for pcsx5 — manual RPC over Windows named pipe.
//
// Connects to the local Discord client via \\.\pipe\discord-ipc-{n},
// sends the handshake, and periodically updates the user's activity
// (game title, status, timestamps, party info).
//
// No Discord Game SDK DLL required — pure Win32 + JSON over named pipe.
//
// Protocol reference: https://discord.com/developers/docs/rich-presence/how-to

#pragma once

#include <cstdint>
#include <string>
#include <chrono>

namespace Ui {

// Lightweight Discord Rich Presence manager.
// Create one instance, call Start() once, then call UpdatePresence()
// whenever the current game/state changes.  The destructor clears
// the presence and disconnects.
class DiscordRPC {
public:
    // Application ID registered at https://discord.com/developers/applications.
    // Use a placeholder for now — users can replace with their own app ID.
    static constexpr const char* kDefaultAppId = "1323456789012345678";

    DiscordRPC() = default;
    ~DiscordRPC();

    // Non-copyable, non-movable (owns the pipe handle).
    DiscordRPC(const DiscordRPC&) = delete;
    DiscordRPC& operator=(const DiscordRPC&) = delete;

    // Connect to Discord and send the handshake.  Returns true on success.
    // app_id: Discord application client ID (string, e.g. "1323456789012345678").
    bool Start(const char* app_id = kDefaultAppId);

    // Disconnect and clear presence.  Safe to call multiple times.
    void Stop();

    // Returns true if currently connected to Discord.
    bool Connected() const { return pipe_ != nullptr; }

    // Update the Rich Presence activity.  Call whenever the current
    // game or emulator state changes.  All string fields are optional
    // (empty = not shown).
    //
    // Fields:
    //   details    — top line (e.g. "Playing Astro's Playroom")
    //   state      — second line (e.g. "In-Game" or "Main Menu")
    //   large_image_key — asset key for the large icon (e.g. "game_icon")
    //   large_image_text— tooltip for the large icon
    //   small_image_key — asset key for the small icon overlay
    //   small_image_text— tooltip for the small icon
    //   party_id   — unique party identifier (for "Ask to Join")
    //   party_size — current party size
    //   party_max  — maximum party size
    //   start_time — Unix timestamp when the activity began (0 = now)
    void UpdatePresence(const char* details,
                        const char* state,
                        const char* large_image_key = "",
                        const char* large_image_text = "",
                        const char* small_image_key = "",
                        const char* small_image_text = "",
                        const char* party_id = "",
                        int party_size = 0,
                        int party_max = 0,
                        std::int64_t start_time = 0);

    // Convenience: clear presence (set "Idle" with no game).
    void ClearPresence();

    // Process incoming pipe data (non-blocking).  Call once per frame
    // from the main loop to handle Discord responses/errors.
    void Pump();

private:
    void* pipe_ = nullptr;  // HANDLE (void* to avoid windows.h in header)
    int    pipe_num_ = 0;
    bool   handshake_done_ = false;
    std::string app_id_;

    // Internal helpers.
    bool ConnectPipe(int num);
    bool SendHandshake();
    bool SendFrame(int opcode, const std::string& payload);
    std::string ReadFrame();
    void ClosePipe();
};

}  // namespace Ui