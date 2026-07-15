// pcsx5_ui — desktop front-end.
//
// Boots ImGui on a GLFW + OpenGL3 window, scans ./Games for installed
// titles, lets the user pick one, and runs pcsx5.exe (or pcsx5 on POSIX)
// as a child process with its stdout/stderr piped into a detachable
// in-app console.  The compat DB (src/compat) is queried to show the
// curated status next to each title.

// Include the Windows header FIRST so <wingdi.h> picks up the correct
// types (Windows headers are order-sensitive).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "ui/ui.h"
#include "ui/library.h"
#include "ui/console.h"
#include "ui/process.h"
#include "ui/theme.h"
#include "ui/thumbnail.h"
#include "ui/system_panel.h"
#include "ui/snd_player.h"
#include "ui/discord_rpc.h"
#include "ui/i18n.h"
#include "compat/compat.h"
#include "system/system.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Ui {

// Left sidebar with primary actions and emulator status.
struct SidebarState {
    bool   console_open     = true;
    bool   about_open       = false;
    bool   about_requested  = false;
    bool   refresh_requested = false;
    bool   title_music_on   = true;   // SharpEmu has the same toggle
    int    view_mode        = 0;   // 0 = library, 1 = system
};

static void DrawTopBar(bool backend_ok, SidebarState& s, char* search_buf, int search_buf_size, std::function<void()> on_rescan) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p  = ImGui::GetCursorScreenPos();
    const float  w  = ImGui::GetContentRegionAvail().x;
    const float  h  = 54.0f;

    // Draw a subtle translucent background for the header
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(10, 15, 25, 140));

    // Left side: L1 tab Library Options R1
    ImGui::SetCursorScreenPos(ImVec2(p.x + 14, p.y + 11));
    
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.6f, 0.9f, 1.0f));
    ImGui::TextUnformatted("L1");
    ImGui::PopStyleColor();
    
    ImGui::SameLine(0, 10);
    
    // Library Tab
    bool is_lib = (s.view_mode == 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    if (is_lib) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.66f, 0.99f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.66f, 0.99f, 0.9f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.1f));
    }
    if (ImGui::Button("Library", ImVec2(100, 32))) {
        s.view_mode = 0;
    }
    ImGui::PopStyleColor(2);
    
    ImGui::SameLine(0, 10);
    
    // Options Tab
    bool is_opt = (s.view_mode == 1);
    if (is_opt) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.66f, 0.99f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.66f, 0.99f, 0.9f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.1f));
    }
    if (ImGui::Button("Options", ImVec2(100, 32))) {
        s.view_mode = 1;
    }
    ImGui::PopStyleColor(2);
    
    ImGui::SameLine(0, 10);
    
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.6f, 0.9f, 1.0f));
    ImGui::TextUnformatted("R1");
    ImGui::PopStyleColor();
    
    ImGui::PopStyleVar();

    // Right side elements: Search, buttons, status
    const char* bk = backend_ok ? "VULKAN READY" : "NO BACKEND";
    ImU32 bk_col = backend_ok ? IM_COL32(63, 185, 80, 255) : IM_COL32(248, 81, 73, 255);
    ImVec2 bk_ts = ImGui::CalcTextSize(bk);
    
    float search_w = 180.0f;
    float buttons_w = 260.0f;
    
    ImGui::SameLine(p.x + w - bk_ts.x - search_w - buttons_w - 60.0f);
    
    ImGui::SetNextItemWidth(search_w);
    ImGui::InputTextWithHint("##search_top", "Search library...", search_buf, search_buf_size);
    
    ImGui::SameLine(0, 10);
    if (ImGui::Button("+ Add folder")) {
        on_rescan();
    }
    ImGui::SameLine(0, 6);
    if (ImGui::Button("Rescan")) {
        on_rescan();
    }
    ImGui::SameLine(0, 6);
    if (ImGui::Button("Open file...")) {
        // Mock open file
    }
    
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - bk_ts.x - 20, p.y + 18));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(bk_col));
    ImGui::TextUnformatted(bk);
    ImGui::PopStyleColor();

    // Bottom border line
    dl->AddRectFilled(ImVec2(p.x, p.y + h - 1), ImVec2(p.x + w, p.y + h), IM_COL32(0, 168, 252, 100));
    ImGui::Dummy(ImVec2(w, h));
}

static void DrawSidebar(ProcessRunner& runner,
                        const std::string& backend_path,
                        bool backend_ok,
                        int game_count,
                        SidebarState& s,
                        std::string& last_message,
                        std::function<void()> on_refresh_status_clicked) {
    ImGui::BeginChild("sidebar", ImVec2(240, 0), true);

    // Section: emulator
    DrawSectionHeader(I18n::Tr("sidebar.emulator"));
    if (runner.running()) {
        if (runner.paused()) {
            DrawStatusPill(I18n::Tr("status.paused"), IM_COL32(220, 165, 60, 255));
        } else {
            DrawStatusPill(I18n::Tr("status.running"), Palette::StatusOk);
        }
    } else {
        DrawStatusPill(I18n::Tr("status.idle"), Palette::StatusIdle);
    }
    ImGui::Dummy(ImVec2(0, 4));

    // Backend status (compact).
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.58f, 0.62f, 1.0f));
    ImGui::TextWrapped("%s", backend_ok
                              ? I18n::Tr("backend.ready")
                              : I18n::Tr("backend.not_found"));
    ImGui::PopStyleColor();
    if (!backend_path.empty() && backend_ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.43f, 0.50f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.7f);
        ImGui::TextWrapped("%s", backend_path.c_str());
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 16));
    DrawSectionHeader(I18n::Tr("sidebar.tools"));

    // Secondary actions.
    {
        if (ImGui::Button(I18n::Tr("button.refresh_library"),
                          ImVec2(ImGui::GetContentRegionAvail().x, 32))) {
            s.refresh_requested = true;
        }
    }
    ImGui::Dummy(ImVec2(0, 2));
    {
        const bool disabled = game_count == 0;
        if (disabled) ImGui::BeginDisabled();
        if (ImGui::Button(I18n::Tr("button.refresh_status"),
                          ImVec2(ImGui::GetContentRegionAvail().x, 32))) {
            on_refresh_status_clicked();
        }
        if (disabled) ImGui::EndDisabled();
    }

    ImGui::Dummy(ImVec2(0, 16));
    DrawSectionHeader(I18n::Tr("sidebar.audio"));

    // Title-music preview toggle.  Mirrors SharpEmu's setting: when
    // off, the SndPreviewPlayer is silenced and any currently-playing
    // preview is stopped immediately.
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.85f, 0.95f, 1.0f));
        bool prev = s.title_music_on;
        if (ImGui::Checkbox(I18n::Tr("audio.title_music"), &s.title_music_on) && prev != s.title_music_on) {
            // No direct hook here; the call-site reads sbar.title_music_on
            // every frame and calls snd.SetEnabled() accordingly.
        }
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, 16));
    DrawSectionHeader(I18n::Tr("sidebar.view"));
    // Tab-like switcher for the main content area.
    {
        const bool is_lib = (s.view_mode == 0);
        const bool is_sys = (s.view_mode == 1);
        const float half_w = (ImGui::GetContentRegionAvail().x - 4.0f) * 0.5f;
        if (is_lib) ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4(0.0f, 0.659f, 0.988f, 1.0f));
        else        ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4(0.118f, 0.141f, 0.176f, 1.0f));
        if (ImGui::Button(I18n::Tr("view.library"), ImVec2(half_w, 28))) {
            s.view_mode = 0;
        }
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 4);
        if (is_sys) ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4(0.0f, 0.659f, 0.988f, 1.0f));
        else        ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4(0.118f, 0.141f, 0.176f, 1.0f));
        if (ImGui::Button(I18n::Tr("view.system"), ImVec2(half_w, 28))) {
            s.view_mode = 1;
        }
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Checkbox(I18n::Tr("sidebar.console"), &s.console_open);
    ImGui::Dummy(ImVec2(0, 12));
    DrawSectionHeader(I18n::Tr("sidebar.help"));
    if (ImGui::Button(I18n::Tr("button.about"),
                      ImVec2(ImGui::GetContentRegionAvail().x, 32))) {
        s.about_requested = true;
    }

    // Language selector.
    ImGui::Dummy(ImVec2(0, 12));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.85f, 0.95f, 1.0f));
    ImGui::Text(I18n::Tr("sidebar.language"));
    ImGui::PopStyleColor();
    static int lang_idx = 0;
    const auto& langs = I18n::AllLanguages();
    static std::vector<const char*> lang_names;
    if (lang_names.empty()) {
        for (I18n::Language lang : langs) {
            lang_names.push_back(I18n::LanguageName(lang));
        }
    }
    I18n::Language cur_lang = I18n::GetLanguage();
    for (int i = 0; i < (int)langs.size(); ++i) {
        if (langs[i] == cur_lang) { lang_idx = i; break; }
    }
    if (ImGui::Combo("##lang_selector", &lang_idx, lang_names.data(),
                     (int)lang_names.size())) {
        if (lang_idx >= 0 && lang_idx < (int)langs.size()) {
            I18n::SetLanguage(langs[lang_idx]);
        }
    }

    // Push status text to the bottom of the sidebar.
    ImGui::Dummy(ImVec2(0, 12));
    if (!last_message.empty()) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.95f, 1.0f));
        ImGui::TextWrapped("%s", last_message.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
}

// Footer status bar across the bottom of the window.
static void DrawFooter(ProcessRunner& runner, int game_count) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p  = ImGui::GetCursorScreenPos();
    const float  w  = ImGui::GetContentRegionAvail().x;
    const float  h  = 26.0f;

    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                      IM_COL32(13, 17, 23, 255));
    dl->AddRectFilled(ImVec2(p.x, p.y),
                      ImVec2(p.x + w, p.y + 1),
                      IM_COL32(0, 168, 252, 120));

    ImGui::SetCursorScreenPos(ImVec2(p.x + 10, p.y + 4));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.58f, 0.62f, 1.0f));
    ImGui::Text(I18n::Tr("footer.games"), game_count);
    ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16);
    ImGui::PushStyleColor(ImGuiCol_Text,
        runner.running() ? ImVec4(0.247f, 0.725f, 0.314f, 1.0f)
                          : ImVec4(0.545f, 0.58f, 0.62f, 1.0f));
    ImGui::Text("%s", runner.running() ? I18n::Tr("footer.running") : I18n::Tr("footer.idle"));
    ImGui::PopStyleColor();

    // Right side: backend hint or path.
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - 280, p.y + 4));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.43f, 0.50f, 1.0f));
    ImGui::Text(I18n::Tr("footer.backend"), 
                runner.running() ? I18n::Tr("footer.active") : I18n::Tr("footer.standby"));
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(w, h));
}

static void DrawControlToolbar(ProcessRunner& runner, int selected_index, const std::vector<GameEntry>& games,
                               ThumbnailCache& thumbs, std::string& last_message, bool& console_open, std::function<void()> on_boot, std::function<void()> on_stop) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.08f, 0.12f, 0.85f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 10));

    float avail_x = ImGui::GetContentRegionAvail().x;
    float capsule_w = avail_x - 32.0f; 
    ImGui::SetCursorPosX(16.0f);

    ImGui::BeginChild("control_toolbar", ImVec2(capsule_w, 72.0f), true, ImGuiWindowFlags_None);

    bool is_running = runner.running();

    if (selected_index >= 0 && selected_index < (int)games.size()) {
        const auto& g = games[selected_index];
        
        // Thumbnail
        Thumbnail thumb = thumbs.Get(g.title_id);
        if (thumb.valid()) {
            ImGui::Image(reinterpret_cast<ImTextureID>(thumb.texture), ImVec2(48, 48));
            ImGui::SameLine(0, 12);
        }
        
        // Metadata text block
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::Text("%s", g.display_name.c_str());
        ImGui::PopStyleColor();
        
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.6f, 0.7f, 1.0f));
        ImGui::Text("%s  •  v1.00  •  %.1f GB", g.title_id.c_str(), g.size_bytes / (1024.0 * 1024.0 * 1024.0));
        ImGui::PopStyleColor();
        
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.43f, 0.50f, 0.8f));
        // Shorten path if it's too long
        std::string short_path = g.dir_path + "/eboot.bin";
        if (short_path.size() > 60) {
            short_path = "..." + short_path.substr(short_path.size() - 57);
        }
        ImGui::Text("%s", short_path.c_str());
        ImGui::PopStyleColor();
        
        // Status dot + text
        ImGui::SameLine(0, 15);
        ImVec4 dot_col = is_running ? ImVec4(0.247f, 0.725f, 0.314f, 1.0f) : ImVec4(0.55f, 0.6f, 0.7f, 1.0f);
        ImGui::TextColored(dot_col, "• %s", is_running ? "Running" : "Idle");
        
        ImGui::EndGroup();
        
        // Controls on Far Right
        float buttons_width = 330.0f;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - buttons_width);
        
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
        
        // Console Toggle Button
        if (ImGui::Button("= Console", ImVec2(100, 36))) {
            console_open = !console_open;
        }
        
        ImGui::SameLine(0, 8);
        
        // Launch/Play Button
        bool play_enabled = !is_running || (is_running && runner.paused());
        if (!play_enabled) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.30f, 0.90f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.40f, 1.00f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.20f, 0.80f, 1.0f));
        if (ImGui::Button("▶ Launch", ImVec2(100, 36))) {
            if (is_running && runner.paused()) {
                runner.Resume();
                last_message = I18n::Tr("msg.resumed");
            } else {
                on_boot();
            }
        }
        ImGui::PopStyleColor(3);
        if (!play_enabled) ImGui::EndDisabled();
        
        ImGui::SameLine(0, 8);
        
        // Stop Button
        bool stop_enabled = is_running;
        if (!stop_enabled) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.22f, 0.27f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.32f, 0.37f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.17f, 0.22f, 1.0f));
        if (ImGui::Button("■ Stop", ImVec2(100, 36))) {
            on_stop();
        }
        ImGui::PopStyleColor(3);
        if (!stop_enabled) ImGui::EndDisabled();
    } else {
        ImGui::TextDisabled("No game selected");
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

int RunUi(int argc, char** argv, const Options& opts) {
    (void)argc; (void)argv;

    // Initialize i18n system
    I18n::Init();
    if (!opts.language.empty()) {
        I18n::SetLanguage(I18n::LanguageFromString(opts.language));
    }

    // ------------------------------------------------------------------
    // 1. CLI parsing
    // ------------------------------------------------------------------
    std::string games_dir  = opts.games_dir;
    std::string compat_dir = opts.compat_dir;
    std::string covers_dir = opts.covers_dir;
    std::string backend    = opts.backend_bin;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--games=", 0) == 0) games_dir = a.substr(8);
        else if (a.rfind("--compat=", 0) == 0) compat_dir = a.substr(9);
        else if (a.rfind("--covers=", 0) == 0) covers_dir = a.substr(9);
        else if (a.rfind("--backend=", 0) == 0) backend = a.substr(10);
        else if (a == "-h" || a == "--help") {
            std::printf("Usage: pcsx5_ui [--games=DIR] [--compat=DIR] [--covers=DIR] [--backend=NAME]\n");
            return 0;
        }
    }

    Compat::Initialize(compat_dir);
    std::string backend_path = LocateBackend(backend);
    if (backend_path.empty()) {
        std::fprintf(stderr,
            "Could not find backend '%s'. Build pcsx5 first, or pass --backend=path/to/pcsx5.\n",
            backend.c_str());
    }

    // ------------------------------------------------------------------
    // 2. GLFW + OpenGL + ImGui setup
    // ------------------------------------------------------------------
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
#if defined(__APPLE__)
    const char* glsl_version = "#version 150";
#else
    const char* glsl_version = "#version 130";
#endif
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1280, 800, "pcsx5", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ApplyModernTheme();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // ------------------------------------------------------------------
    // 3. State
    // ------------------------------------------------------------------
    std::vector<GameEntry> games = ScanGames(games_dir, covers_dir);
    int selected_index = games.empty() ? -1 : 0;
    int prev_selected_index = -2;  // force first-frame dispatch
    LogConsole console;
    ProcessRunner runner;
    ThumbnailCache thumbs;
    thumbs.SetCoversDir(covers_dir);
    Ui::SystemPanelState sys_state;
    SidebarState sbar;
    std::string last_message;
    SndPreviewPlayer snd;
    snd.SetEnabled(true);

    // Discord Rich Presence — connect if an app ID is configured.
    DiscordRPC discord_rpc;
    const char* discord_app_id = DiscordRPC::kDefaultAppId;
    if (discord_app_id && discord_app_id[0]) {
        if (discord_rpc.Start(discord_app_id)) {
            discord_rpc.ClearPresence();  // start with "Idle" presence
        }
    }

    static char search_buf[128] = "";
    static float thumbnail_size = 168.0f;

    // ------------------------------------------------------------------
    // 4. Main loop
    // ------------------------------------------------------------------
    static bool last_pad_left = false;
    static bool last_pad_right = false;
    static bool last_pad_l1 = false;
    static bool last_pad_r1 = false;
    static bool last_pad_a = false;
    static bool last_pad_b = false;
    static bool last_pad_f12 = false;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        discord_rpc.Pump();  // drain Discord IPC messages

        if (sbar.refresh_requested) {
            games = ScanGames(games_dir, covers_dir);
            if (selected_index >= static_cast<int>(games.size())) {
                selected_index = games.empty() ? -1 : 0;
            }
            sbar.refresh_requested = false;
            char buf[128];
            std::snprintf(buf, sizeof(buf), I18n::Tr("msg.library_refreshed"), static_cast<int>(games.size()));
            last_message = buf;
        }

        // Controller Navigation & Shortcuts Check
        bool pad_left = false, pad_right = false, pad_l1 = false, pad_r1 = false, pad_a = false, pad_b = false, pad_f12 = false;
        GLFWgamepadstate gamepad;
        if (glfwGetGamepadState(GLFW_JOYSTICK_1, &gamepad)) {
            pad_left = gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] || gamepad.axes[GLFW_GAMEPAD_AXIS_LEFT_X] < -0.5f;
            pad_right = gamepad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] || gamepad.axes[GLFW_GAMEPAD_AXIS_LEFT_X] > 0.5f;
            pad_l1 = gamepad.buttons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER];
            pad_r1 = gamepad.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER];
            pad_a = gamepad.buttons[GLFW_GAMEPAD_BUTTON_A];
            pad_b = gamepad.buttons[GLFW_GAMEPAD_BUTTON_B];
            pad_f12 = gamepad.buttons[GLFW_GAMEPAD_BUTTON_BACK];
        }

        bool trigger_left = (pad_left && !last_pad_left) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
        bool trigger_right = (pad_right && !last_pad_right) || ImGui::IsKeyPressed(ImGuiKey_RightArrow);
        bool trigger_l1 = (pad_l1 && !last_pad_l1) || ImGui::IsKeyPressed(ImGuiKey_PageUp);
        bool trigger_r1 = (pad_r1 && !last_pad_r1) || ImGui::IsKeyPressed(ImGuiKey_PageDown);
        bool trigger_a = (pad_a && !last_pad_a) || ImGui::IsKeyPressed(ImGuiKey_Enter);
        bool trigger_b = (pad_b && !last_pad_b) || ImGui::IsKeyPressed(ImGuiKey_Escape);
        bool trigger_f12 = (pad_f12 && !last_pad_f12) || ImGui::IsKeyPressed(ImGuiKey_F12) || 
                           (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyDown(ImGuiKey_LeftShift) && ImGui::IsKeyPressed(ImGuiKey_C));

        last_pad_left = pad_left;
        last_pad_right = pad_right;
        last_pad_l1 = pad_l1;
        last_pad_r1 = pad_r1;
        last_pad_a = pad_a;
        last_pad_b = pad_b;
        last_pad_f12 = pad_f12;

        if (trigger_left && selected_index > 0) {
            selected_index--;
        }
        if (trigger_right && selected_index < static_cast<int>(games.size()) - 1) {
            selected_index++;
        }
        if (trigger_l1) {
            sbar.view_mode = 0; // Library
        }
        if (trigger_r1) {
            sbar.view_mode = 1; // Options
        }
        if (trigger_f12) {
            sbar.console_open = !sbar.console_open;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // 1. Draw Fullscreen Hero Background Image
        ImDrawList* back_dl = ImGui::GetBackgroundDrawList();
        ImVec2 display_size = ImGui::GetIO().DisplaySize;
        bool has_bg = false;
        if (selected_index >= 0 && selected_index < static_cast<int>(games.size())) {
            const auto& gsel = games[selected_index];
            if (!gsel.background_path.empty()) {
                Thumbnail bg_thumb = thumbs.GetFromPath(gsel.title_id + "_bg", gsel.background_path);
                if (bg_thumb.valid()) {
                    back_dl->AddImage(reinterpret_cast<ImTextureID>(bg_thumb.texture),
                                      ImVec2(0, 0), display_size,
                                      ImVec2(0, 0), ImVec2(1, 1),
                                      IM_COL32(255, 255, 255, 255));
                    has_bg = true;
                }
            }
        }
        if (!has_bg) {
            // Draw default rich dark blue/cyan gradient
            back_dl->AddRectFilledMultiColor(ImVec2(0, 0), display_size,
                                            IM_COL32(10, 20, 35, 255), IM_COL32(5, 10, 20, 255),
                                            IM_COL32(5, 10, 20, 255), IM_COL32(10, 25, 45, 255));
        }
        // Dark translucent overlay tint for readability
        back_dl->AddRectFilled(ImVec2(0, 0), display_size, IM_COL32(8, 12, 20, 205));

        // Pushing transparent window background color for root
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(display_size);
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove    | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_MenuBar);

        auto on_boot = [&]() {
            if (runner.running()) { last_message = I18n::Tr("msg.already_running"); return; }
            if (selected_index < 0 || selected_index >= static_cast<int>(games.size())) {
                last_message = I18n::Tr("msg.select_game");
                return;
            }
            const auto& g = games[selected_index];
            snd.Stop();
            std::string be = LocateBackend(backend);
            if (be.empty()) { last_message = "Backend binary not found"; return; }
            console.Clear();
            console.Append(">>> pcsx5_ui: launching " + be);
            console.Append(">>> args: " + g.elf_path + " --title-id=" + g.title_id);
            std::vector<std::string> args = {
                g.elf_path,
                std::string("--title-id=") + g.title_id
            };
            if (!runner.Start(be, args, &console, [](int code) {
                (void)code;
            })) {
                last_message = I18n::Tr("msg.failed_spawn");
            } else {
                char buf[256];
                std::snprintf(buf, sizeof(buf), I18n::Tr("msg.booting"), g.title_id.c_str());
                last_message = buf;
                if (discord_rpc.Connected()) {
                    discord_rpc.UpdatePresence(
                        g.display_name.c_str(),
                        "In-Game",
                        g.title_id.c_str(),
                        g.display_name.c_str());
                }
            }
        };
        auto on_stop = [&]() {
            runner.Stop();
            last_message = I18n::Tr("msg.stopped");
            if (discord_rpc.Connected()) {
                discord_rpc.ClearPresence();
            }
        };
        auto on_refresh_status = [&]() {
            if (selected_index < 0 || selected_index >= static_cast<int>(games.size())) {
                last_message = I18n::Tr("msg.select_game");
                return;
            }
            const auto& g = games[selected_index];
            Compat::Entry e;
            if (Compat::Load(g.title_id, e, nullptr)) {
                Compat::Save(std::move(e), nullptr);
            }
            char buf[256];
            std::snprintf(buf, sizeof(buf), I18n::Tr("msg.touched_compat"), g.title_id.c_str());
            last_message = buf;
        };

        if (trigger_a) {
            on_boot();
        }
        if (trigger_b) {
            on_stop();
        }

        DrawTopBar(!backend_path.empty(), sbar, search_buf, sizeof(search_buf), [&](){ sbar.refresh_requested = true; });

        // Compute middle workspace area height dynamically
        float top_bar_h = 54.0f;
        float toolbar_h = 68.0f;
        float footer_h = 26.0f;
        float console_h = sbar.console_open ? 220.0f : 0.0f;
        float total_y = display_size.y;
        float middle_h = total_y - top_bar_h - toolbar_h - footer_h - console_h - (sbar.console_open ? 12.0f : 0.0f) - 10.0f;
        if (middle_h < 100.0f) middle_h = 100.0f;

        // Middle workspace child window
        ImGui::BeginChild("middle_area", ImVec2(0, middle_h), false, ImGuiWindowFlags_None);

        if (sbar.view_mode == 1) {
            DrawSidebar(runner, backend_path, !backend_path.empty(),
                        static_cast<int>(games.size()), sbar, last_message,
                        on_refresh_status);
            ImGui::SameLine();
        }
        snd.SetEnabled(sbar.title_music_on);

        ImGui::BeginChild("content", ImVec2(0, 0), false, ImGuiWindowFlags_None);

        if (sbar.view_mode == 1) {
            DrawSystemPanel(sys_state);
        } else {
            ImGui::AlignTextToFramePadding();
            DrawSectionHeader(I18n::Tr("library.header"));
            ImGui::SameLine(0, 10);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.58f, 0.62f, 1.0f));
            ImGui::Text(I18n::Tr("library.titles"), static_cast<int>(games.size()));
            ImGui::PopStyleColor();
            
            ImGui::Dummy(ImVec2(0, 2));

            ImGui::BeginChild("lib.scroll", ImVec2(0, 0), false, ImGuiWindowFlags_None);
            if (DrawLibraryPanel(games, selected_index, thumbs, thumbnail_size, search_buf)) {
                on_boot();
            }
            ImGui::EndChild();

            if (selected_index != prev_selected_index) {
                prev_selected_index = selected_index;
                if (selected_index >= 0 && selected_index < static_cast<int>(games.size())) {
                    const auto& gsel = games[selected_index];
                    if (gsel.music_path.empty()) {
                        snd.Stop();
                    } else {
                        snd.Play(gsel.music_path);
                    }
                } else {
                    snd.Stop();
                }
            }
        }

        ImGui::EndChild(); // end content
        ImGui::EndChild(); // end middle_area

        // Draw horizontal bottom controls toolbar
        DrawControlToolbar(runner, selected_index, games, thumbs, last_message, sbar.console_open, on_boot, on_stop);

        // VS Code-style full-width bottom console panel
        if (sbar.console_open) {
            ImGui::Separator();
            ImGui::BeginChild("vscode_console", ImVec2(0, console_h), false, ImGuiWindowFlags_None);
            console.Draw(I18n::Tr("sidebar.console"));
            ImGui::EndChild();
        } else {
            // Draw a tiny bottom toggle tab for console (Phase 4)
            ImGui::SetCursorPosY(total_y - footer_h - 22.0f);
            ImGui::SetCursorPosX(14.0f);
            if (ImGui::Button("^ Console (F12)", ImVec2(120, 20))) {
                sbar.console_open = true;
            }
        }

        DrawFooter(runner, static_cast<int>(games.size()));

        ImGui::End();
        ImGui::PopStyleColor(); // pop WindowBg

        // ----- About dialog -----
        if (sbar.about_requested) {
            ImGui::OpenPopup(I18n::Tr("about.title"));
            sbar.about_requested = false;
        }
        if (ImGui::BeginPopupModal(I18n::Tr("about.title"), nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", I18n::Tr("about.line1"));
            ImGui::Separator();
            ImGui::BulletText("%s", I18n::Tr("about.line2"));
            ImGui::BulletText("%s", I18n::Tr("about.line3"));
            ImGui::BulletText("%s", I18n::Tr("about.line4"));
            ImGui::Dummy(ImVec2(0, 6));
            if (ImGui::Button(I18n::Tr("about.ok"), ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.051f, 0.067f, 0.090f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // ------------------------------------------------------------------
    // 5. Cleanup
    // ------------------------------------------------------------------
    discord_rpc.Stop();  // clear presence and disconnect from Discord
    runner.Stop();
    snd.Stop();
    thumbs.Clear();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}  // namespace Ui
