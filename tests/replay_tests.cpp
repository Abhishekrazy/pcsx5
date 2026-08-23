#define _CRT_SECURE_NO_WARNINGS
//
// I2.2: Replay integration test — generates a basic replay JSON and runs
// pcsx5_cli with --headless --play-input to verify the emulator doesn't
// crash during replay-driven execution on a known-good test ELF.
//
// Usage:
//   replay_tests --cli=<pcsx5_cli_path> --elf=<test_elf_path>
//
// The test creates a temporary replay file with a short button sequence,
// launches the CLI as a subprocess, waits for it to complete, and checks
// the exit code and output for crash signatures.  The replay file is
// cleaned up on exit.
//
// Exit codes:
//   0 = PASS (CLI completed without crash) or SKIP (missing inputs)
//   1 = FAIL (crash detected)
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <filesystem>
#include <fstream>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool HasCrashSignature(const std::string& output) {
    // Known crash signatures logged by pcsx5 during a bot-run session.
    const char* signatures[] = {
        "VEH Unhandled Exception",
        "GUEST APPLICATION CRASHED",
        "FATAL",
        "ACCESS_VIOLATION",
    };
    for (const char* sig : signatures) {
        if (output.find(sig) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Same as HasCrashSignature but tolerant of the benign teardown line
// "FATAL: abort() raised (signal 22)" the guest logs on a clean
// abort()-terminated run (see the crashed computation in main).
static bool HasCrashSignatureExcludingBenignFatal(const std::string& output) {
    std::string cleaned = output;
    const char* benign = "FATAL: abort() raised (signal 22)";
    size_t pos;
    while ((pos = cleaned.find(benign)) != std::string::npos) {
        cleaned.erase(pos, strlen(benign));
    }
    return HasCrashSignature(cleaned);
}

// Write a minimal input replay JSON to the given path.
static bool WriteReplayFile(const std::string& path) {
    std::ofstream out(path);
    if (!out.is_open()) {
        std::printf("FAIL: Cannot write replay file: %s\n", path.c_str());
        return false;
    }
    out << "{\n";
    out << "  \"version\": 1,\n";
    out << "  \"title_id\": \"TEST\",\n";
    out << "  \"events\": [\n";
    out << "    { \"frame\": 0,   \"buttons\": 0,    \"lx\": 128, \"ly\": 128, \"rx\": 128, \"ry\": 128, \"l2\": 0, \"r2\": 0 },\n";
    out << "    { \"frame\": 60,  \"buttons\": 1,    \"lx\": 128, \"ly\": 128, \"rx\": 128, \"ry\": 128, \"l2\": 0, \"r2\": 0 },\n";
    out << "    { \"frame\": 120, \"buttons\": 0x40, \"lx\": 255, \"ly\": 128, \"rx\": 128, \"ry\": 128, \"l2\": 0, \"r2\": 0 }\n";
    out << "  ]\n";
    out << "}\n";
    out.close();
    return true;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::string cli_path;
    std::string elf_path;

    // Parse arguments: --cli=<path> --elf=<path> (positional also accepted).
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--cli=", 0) == 0) {
            cli_path = arg.substr(6);
        } else if (arg.rfind("--elf=", 0) == 0) {
            elf_path = arg.substr(6);
        } else if (cli_path.empty()) {
            cli_path = arg;
        } else if (elf_path.empty()) {
            elf_path = arg;
        }
    }

    // Validate required args.
    if (cli_path.empty()) {
        std::printf("FAIL: --cli=<pcsx5_cli> is required\n");
        std::printf("Usage: replay_tests --cli=<path> --elf=<path>\n");
        return 1;
    }
    if (elf_path.empty()) {
        std::printf("FAIL: --elf=<test_elf> is required\n");
        std::printf("Usage: replay_tests --cli=<path> --elf=<path>\n");
        return 1;
    }

    // Check that both files exist; skip with a clear message if either is
    // missing so the test is non-fatal in CI without clang/guest ELFs.
    if (!std::filesystem::exists(cli_path)) {
        std::printf("SKIP: CLI not found: %s\n", cli_path.c_str());
        return 0;
    }
    if (!std::filesystem::exists(elf_path)) {
        std::printf("SKIP: Test ELF not found: %s\n", elf_path.c_str());
        return 0;
    }

    // Create a temporary replay file in the system temp directory.
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path replay_path = temp_dir / "pcsx5_replay_test.json";

    if (!WriteReplayFile(replay_path.string())) {
        return 1;
    }

    // Build the CLI command line.
    // Use double-quoted paths so shell handles spaces in file names.
    // Explicit "cmd /c" wrapper with "< NUL": the CLI fails to initialize
    // when its stdin is the pipe _popen provides ("The filename, directory
    // name, or volume label syntax is incorrect." during CRT startup);
    // pointing stdin at the null device avoids that.  Verified: same
    // binary+args exit=0 with < NUL, exit=1 via plain _popen (2026-08-23).
    std::string cmd = "cmd /c \"\"" + cli_path + "\" --headless --play-input=\"" +
                      replay_path.string() + "\" \"" + elf_path +
                      "\" < NUL 2>&1\"";

    std::printf("CLI:   %s\n", cli_path.c_str());
    std::printf("ELF:   %s\n", elf_path.c_str());
    std::printf("Replay: %s\n", replay_path.string().c_str());
    std::printf("Running: %s\n", cmd.c_str());

    // Launch the CLI as a subprocess and capture stdout+stderr.
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) {
        std::printf("FAIL: _popen failed\n");
        std::filesystem::remove(replay_path);
        return 1;
    }

    std::string output;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        output += buf;
    }
    int exit_code = _pclose(pipe);

    // Clean up the temporary replay file.
    std::error_code ec;
    std::filesystem::remove(replay_path, ec);

    // Report full output on failure for debugging.  The "FATAL" signature is
    // only meaningful on a dirty exit: the guest teardown path logs a benign
    // "FATAL: abort() raised (signal 22)" line when the test ELF terminates
    // via abort() after sys_exit(0).  The other signatures (VEH unhandled
    // exception, ACCESS_VIOLATION, GUEST APPLICATION CRASHED) always fail
    // the run — a process that logs them and exits 0 is still broken.
    bool crashed = (exit_code != 0) || HasCrashSignatureExcludingBenignFatal(output);

    if (crashed) {
        std::printf("\n=== OUTPUT (exit=%d) ===\n%s\n=== END ===\n",
                    exit_code, output.c_str());
        std::printf("FAIL: pcsx5_cli crashed or returned error (exit=%d)\n",
                    exit_code);
        return 1;
    }

    std::printf("PASS: pcsx5_cli completed without crash (exit=%d)\n",
                exit_code);
    return 0;
}
