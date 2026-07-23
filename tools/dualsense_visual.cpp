//
// dualsense_visual — redirection shim to unified pcsx5_cli --test-input CLI (D3.4).
//
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    std::printf("==================================================\n");
    std::printf("  pcsx5 - Standalone Visual Input Tester Redirect\n");
    std::printf("==================================================\n");
    std::printf("[NOTICE] 'dualsense_visual' has been deprecated in favor of the unified input tester.\n");
    std::printf("For graphical controller visualization, use the WPF Launcher UI or run:\n");
    std::printf("Redirecting to: pcsx5_cli.exe --test-input ...\n\n");

    char module_path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
    std::filesystem::path cli_path;
    if (len > 0 && len < MAX_PATH) {
        cli_path = std::filesystem::path(module_path).parent_path() / "pcsx5_cli.exe";
    }
    if (cli_path.empty() || !std::filesystem::exists(cli_path)) {
        cli_path = "pcsx5_cli.exe";
    }

    std::string cmd = "\"" + cli_path.string() + "\" --test-input";
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exit_code = 0;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return static_cast<int>(exit_code);
    } else {
        std::fprintf(stderr, "Failed to launch pcsx5_cli.exe --test-input (error code %ld)\n", GetLastError());
        return 1;
    }
}
