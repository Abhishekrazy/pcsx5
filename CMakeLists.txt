cmake_minimum_required(VERSION 3.20)
cmake_policy(VERSION 3.5)
project(pcsx5 LANGUAGES CXX C)

# Force C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configure ASM MASM for x64 Windows assembly compilation
if(MSVC)
    enable_language(ASM_MASM)
    set(DISPATCHER_SRC src/hle/dispatcher.asm)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/FS>" "$<$<COMPILE_LANGUAGE:C>:/FS>")
else()
    message(FATAL_ERROR "Currently, only MSVC toolchains are supported for building pcsx5 on Windows due to MASM calling convention bridge requirements.")
endif()

# Fetch dependencies
include(FetchContent)

# Fetch GLFW
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.3.9
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# Fetch Vulkan Headers
FetchContent_Declare(
    vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        v1.3.275
)
FetchContent_MakeAvailable(vulkan_headers)

# Fetch Dear ImGui (docking branch, MIT) for pcsx5_ui.  We do not use the
# upstream CMakeLists because it requires OpenGL/Vulkan to be selected at
# configure time; we instead add the .cpp files we need directly.
set(IMGUI_DIR ${CMAKE_BINARY_DIR}/_deps/imgui-src CACHE PATH "" FORCE)
if(NOT EXISTS ${IMGUI_DIR})
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.90.4
    )
    FetchContent_Populate(imgui)
    set(IMGUI_DIR ${imgui_SOURCE_DIR} CACHE PATH "" FORCE)
endif()

# Fetch Lua 5.4.7 — compiled as a static C library for the Lua init script engine.
# The official Lua repository does NOT have a CMakeLists.txt, so we use
# FetchContent_Populate (download-only) and manually build the library.
# Suppress CMP0169 deprecation warning for FetchContent_Populate.
cmake_policy(SET CMP0169 OLD)
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        v5.4.7
)
FetchContent_GetProperties(lua)
if(NOT lua_POPULATED)
    FetchContent_Populate(lua)
endif()

# Fetch nlohmann/json — header-only JSON library for i18n translation files.
# Set policy minimum to 3.5 to allow nlohmann_json's old cmake_minimum_required
cmake_policy(SET CMP0150 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.2
)
FetchContent_MakeAvailable(nlohmann_json)

# Collect Lua core + library sources (exclude the standalone interpreters lua.c/luac.c
# and the amalgamation file onelua.c which duplicates all symbols).
# Lua sources are flat in the repo root, not in a src/ subdirectory.
file(GLOB LUA_SRC "${lua_SOURCE_DIR}/*.c")
list(REMOVE_ITEM LUA_SRC "${lua_SOURCE_DIR}/lua.c" "${lua_SOURCE_DIR}/luac.c" "${lua_SOURCE_DIR}/onelua.c")

add_library(lua STATIC ${LUA_SRC})
target_include_directories(lua PUBLIC ${lua_SOURCE_DIR})
# Lua uses C99 features; silence MSVC deprecation warnings and mark as DLL build
# so __declspec(dllexport) is used (required for the static library to export symbols).
target_compile_definitions(lua PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(lua PRIVATE $<$<COMPILE_LANGUAGE:C>:/W3>)
endif()

set(KERNEL_SRC
    src/kernel/kernel.h
    src/kernel/kernel.cpp
    src/kernel/fd_table.h
    src/kernel/fd_table.cpp
    src/kernel/memory.h
    src/kernel/memory.cpp
    src/kernel/thread.h
    src/kernel/thread.cpp
    src/kernel/syscalls.h
    src/kernel/syscalls.cpp
    src/kernel/tls.h
    src/kernel/tls.cpp
)

# CPU core — owns the guest thread registry; required wherever KERNEL_SRC is
# linked because kernel/thread.cpp is a shim over CpuCore.
set(CPU_SRC
    src/cpu/cpu.h
    src/cpu/cpu.cpp
)

add_executable(pcsx5
    src/main.cpp
    src/common/types.h
    src/common/log.h
    src/common/log.cpp
    src/memory/memory.h
    src/memory/memory.cpp
    src/loader/elf.h
    src/loader/elf.cpp
    src/hle/hle.h
    src/hle/hle.cpp
    src/hle/libkernel.cpp
    src/hle/libpad.cpp
    src/hle/libvideoout.cpp
    src/hle/libagc.cpp
    src/hle/libscepad.cpp
    src/common/nid.cpp
    ${KERNEL_SRC}
    ${CPU_SRC}
    src/gpu/gpu.h
    src/gpu/vulkan_backend.cpp
    src/diagnostics/diagnostics.h
    src/diagnostics/diagnostics.cpp
    src/config/config.h
    src/config/config.cpp
    src/reports/reports.h
    src/reports/reports.cpp
    src/lua/lua_init.h
    src/lua/lua_init.cpp
    ${DISPATCHER_SRC}
    src/pcsx5.rc
)

include(CTest)
if(BUILD_TESTING)
    add_executable(loader_validation_tests
        tests/loader_validation.cpp
        src/common/log.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
    )
    target_include_directories(loader_validation_tests PRIVATE src)
    target_compile_options(loader_validation_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME loader_validation COMMAND loader_validation_tests)

    # ------------------------------------------------------------------------
    # Positive ELF corpus — hand-crafts valid PIE / fixed-address / multi-
    # segment / BSS / PT_DYNAMIC binaries and asserts the post-load invariants
    # the rest of the emulator depends on.  Does not require clang.
    # ------------------------------------------------------------------------
    add_executable(loader_corpus_tests
        tests/loader_corpus.cpp
        src/common/log.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
    )
    target_include_directories(loader_corpus_tests PRIVATE src)
    target_compile_options(loader_corpus_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME loader_corpus COMMAND loader_corpus_tests)

    add_executable(memory_validation_tests
        tests/memory_validation.cpp
        src/common/log.cpp
        src/memory/memory.cpp
    )
    target_include_directories(memory_validation_tests PRIVATE src)
    target_compile_options(memory_validation_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME memory_validation COMMAND memory_validation_tests)

    # ------------------------------------------------------------------------
    # Memory query / fault / status tests — exercises the new Status-based
    # API added in Phase 1 (Query, error codes, guest fault VEH, stats,
    # legacy wrapper compatibility).
    # ------------------------------------------------------------------------
    add_executable(memory_query_tests
        tests/memory_query_tests.cpp
        src/common/log.cpp
        src/memory/memory.cpp
    )
    target_include_directories(memory_query_tests PRIVATE src)
    target_compile_options(memory_query_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME memory_query COMMAND memory_query_tests)

    add_executable(elf_metadata_tests
        tests/elf_metadata_tests.cpp
        src/common/log.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
    )
    target_include_directories(elf_metadata_tests PRIVATE src)
    target_compile_options(elf_metadata_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME elf_metadata COMMAND elf_metadata_tests)

    # ------------------------------------------------------------------------
    # PS5 NID encoder/decoder tests — round-trip, type-tag parsing, and a
    # known-name table lookup hit (using the top PPSA02929 NID as the
    # canonical sample).
    # ------------------------------------------------------------------------
    add_executable(nid_tests
        tests/nid_tests.cpp
        src/common/log.cpp
        src/common/nid.cpp
    )
    target_include_directories(nid_tests PRIVATE src)
    target_compile_options(nid_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME nid COMMAND nid_tests)

    # ------------------------------------------------------------------------
    # SELF (Signed ELF) container parser tests — synthetic SELF files with
    # a known inner ELF, exercising the structural parser, ext-info, segment
    # flag decoding, authority-id classification, and the end-to-end
    # LoadSelf path for fake-signed plaintext SELFs.
    # ------------------------------------------------------------------------
    add_executable(self_header_tests
        tests/self_header_tests.cpp
        src/common/log.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
    )
    target_include_directories(self_header_tests PRIVATE src)
    target_compile_options(self_header_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME self_header COMMAND self_header_tests)

    add_executable(tls_context_tests
        tests/tls_context.cpp
        src/kernel/tls.cpp
    )
    target_include_directories(tls_context_tests PRIVATE src)
    target_compile_options(tls_context_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME tls_context COMMAND tls_context_tests)

    # ------------------------------------------------------------------------
    # HLE subsystem unit tests — exercises the import reporting, strict mode,
    # and registry behavior without running the full emulator.
    # ------------------------------------------------------------------------
    add_executable(hle_import_report_tests
        tests/hle_import_report.cpp
        src/common/log.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
        ${KERNEL_SRC}
        ${CPU_SRC}
        src/gpu/vulkan_backend.cpp
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        src/hle/libscepad.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(hle_import_report_tests PRIVATE src)
    target_compile_options(hle_import_report_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(hle_import_report_tests PRIVATE glfw Vulkan::Headers nlohmann_json::nlohmann_json)
    add_test(NAME hle_import_report COMMAND hle_import_report_tests)

    # ------------------------------------------------------------------------
    # Reports tests — CompatSummary jsonl history, regression verdict,
    # aggregated markdown report.
    # ------------------------------------------------------------------------
    add_executable(report_tests
        tests/report_tests.cpp
        src/common/log.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
        ${KERNEL_SRC}
        ${CPU_SRC}
        src/gpu/vulkan_backend.cpp
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        src/hle/libscepad.cpp
        src/reports/reports.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(report_tests PRIVATE src)
    target_compile_options(report_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(report_tests PRIVATE glfw Vulkan::Headers nlohmann_json::nlohmann_json)
    add_test(NAME reports COMMAND report_tests)

    # ------------------------------------------------------------------------
    # Config service tests — round-trip, missing file, corrupt file, defaults,
    # migration, per-title override layering.
    # ------------------------------------------------------------------------
    add_executable(config_tests
        tests/config_tests.cpp
        src/common/log.cpp
        src/config/config.cpp
    )
    target_include_directories(config_tests PRIVATE src)
    target_compile_options(config_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME config COMMAND config_tests)

    # ------------------------------------------------------------------------
    # Diagnostics tests — structured logging, HLE import trace, crash-report
    # bundle layout.  Exercises the new Phase 0 problems-and-diagnostics APIs.
    # ------------------------------------------------------------------------
    add_executable(diagnostics_tests
        tests/diagnostics_tests.cpp
        src/common/log.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
        ${KERNEL_SRC}
        ${CPU_SRC}
        src/gpu/vulkan_backend.cpp
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        src/hle/libscepad.cpp
        src/diagnostics/diagnostics.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(diagnostics_tests PRIVATE src)
    target_compile_options(diagnostics_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(diagnostics_tests PRIVATE glfw Vulkan::Headers nlohmann_json::nlohmann_json)
    add_test(NAME diagnostics COMMAND diagnostics_tests)

    # ------------------------------------------------------------------------
    # Guest smoke test — builds the freestanding test ELFs (syscall + TLS) and
    # runs them through the pcsx5 emulator in strict-imports mode.  The build
    # step requires clang on PATH; if it is missing the test is skipped with
    # a clear diagnostic.
    # ------------------------------------------------------------------------
    find_program(CLANG_EXECUTABLE
        NAMES clang clang.exe
        PATHS "C:/Program Files/LLVM/bin" "C:/Program Files (x86)/LLVM/bin"
        DOC   "Clang front-end required to build the freestanding guest test ELFs."
    )

    # ------------------------------------------------------------------------
    # Cover-art helper — generates synthetic placeholder PNGs in
    # ./Covers/ so the UI has something to display during development.
    # ------------------------------------------------------------------------
    add_executable(make_test_covers
        tests/make_test_covers.cpp
    )
    target_include_directories(make_test_covers PRIVATE third_party)
    target_compile_options(make_test_covers PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W3>)

    # ------------------------------------------------------------------------
    # LibAtrac9 — vendored C port of Thealexbarney/LibAtrac9 (MIT).
    # Used by SndPreviewPlayer to decode sce_sys/snd0.at9 to PCM16 WAV
    # for the title-music preview.  See src/ui/snd_player.cpp.
    # ------------------------------------------------------------------------
    add_library(atrac9 STATIC
        third_party/LibAtrac9/src/band_extension.c
        third_party/LibAtrac9/src/bit_allocation.c
        third_party/LibAtrac9/src/bit_reader.c
        third_party/LibAtrac9/src/decinit.c
        third_party/LibAtrac9/src/decoder.c
        third_party/LibAtrac9/src/huffCodes.c
        third_party/LibAtrac9/src/imdct.c
        third_party/LibAtrac9/src/libatrac9.c
        third_party/LibAtrac9/src/quantization.c
        third_party/LibAtrac9/src/scale_factors.c
        third_party/LibAtrac9/src/tables.c
        third_party/LibAtrac9/src/unpack.c
        third_party/LibAtrac9/src/utility.c
    )
    target_include_directories(atrac9
        PUBLIC  third_party/LibAtrac9/include
        PRIVATE third_party/LibAtrac9/include/libatrac9
    )
    if(MSVC)
        # The LibAtrac9 header uses __declspec(dllexport/dllimport) for
        # a DLL build.  We compile it as a static library instead, so
        # both the producer and the consumer must see `dllexport` for
        # the linker to match the symbols.  See libatrac9.h:6-11.
        target_compile_definitions(atrac9 PUBLIC COMPILING_DLL)
        target_compile_options(atrac9 PRIVATE $<$<COMPILE_LANGUAGE:C>:/W3>)
    endif()

    if(CLANG_EXECUTABLE)
        # Copy every freestanding test source next to the build tree so the
        # .bat file can operate on a stable working directory.
        file(COPY ${CMAKE_SOURCE_DIR}/tests/test_elf/main.cpp
                  ${CMAKE_SOURCE_DIR}/tests/test_elf/tls_main.cpp
                  ${CMAKE_SOURCE_DIR}/tests/test_elf/build_all.bat
                  ${CMAKE_SOURCE_DIR}/tests/test_elf/build_elf.bat
                  ${CMAKE_SOURCE_DIR}/tests/test_elf/build_tls_elf.bat
                  ${CMAKE_SOURCE_DIR}/tests/test_elf/build_corpus.bat
             DESTINATION ${CMAKE_BINARY_DIR}/test_elf)

        add_custom_command(
            OUTPUT  ${CMAKE_BINARY_DIR}/test_elf/test_guest.elf
                    ${CMAKE_BINARY_DIR}/test_elf/tls_guest.elf
            COMMAND ${CMAKE_BINARY_DIR}/test_elf/build_all.bat
                    ${CMAKE_BINARY_DIR}/test_elf
            DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_elf/main.cpp
                    ${CMAKE_SOURCE_DIR}/tests/test_elf/tls_main.cpp
            COMMENT "Building freestanding guest test ELFs (clang)"
            VERBATIM
        )
        add_custom_target(guest_test_elfs
            DEPENDS ${CMAKE_BINARY_DIR}/test_elf/test_guest.elf
                    ${CMAKE_BINARY_DIR}/test_elf/tls_guest.elf
        )

        # Syscall smoke test: writes "Hello Guest\n" via sys_write and exits.
        add_test(NAME guest_syscall_smoke
            COMMAND pcsx5 --strict-imports --report $<CONFIG>/guest_syscall_report.json
                    ${CMAKE_BINARY_DIR}/test_elf/test_guest.elf
        )
        set_tests_properties(guest_syscall_smoke PROPERTIES
            WILL_FAIL FALSE
            PASS_REGULAR_EXPRESSION "Hello Guest"
            DEPENDS guest_test_elfs
        )

        # TLS smoke test: uses fs:0 read/write to verify the VEH trap.
        add_test(NAME guest_tls_smoke
            COMMAND pcsx5 --strict-imports --report $<CONFIG>/guest_tls_report.json
                    ${CMAKE_BINARY_DIR}/test_elf/tls_guest.elf
        )
        set_tests_properties(guest_tls_smoke PROPERTIES
            WILL_FAIL FALSE
            PASS_REGULAR_EXPRESSION "TLS OK"
            DEPENDS guest_test_elfs
        )
    else()
        message(STATUS
            "Skipping guest smoke tests: clang not found on PATH. "
            "Install LLVM/clang and reconfigure to enable these tests.")
    endif()
# Enable Lua scripting for the subsystem lifecycle init script.
target_compile_definitions(pcsx5 PRIVATE PCSX5_HAS_LUA)

# Link GLFW, Vulkan Headers, and Lua dependencies
target_link_libraries(pcsx5 PRIVATE glfw Vulkan::Headers lua ws2_32 nlohmann_json::nlohmann_json)
target_include_directories(pcsx5 PRIVATE src)

# Add Windows SDK include path for resource compiler (RC) to find windows.h
if(MSVC)
    target_include_directories(pcsx5 PRIVATE
        $<$<COMPILE_LANGUAGE:RC>:${CMAKE_RC_INCLUDE_PATH}>
    )
    # Windows SDK / MSVC system libraries (ws2_32, ucrt, msvcprt, ...) are
    # resolved by the linker via the toolchain's own search paths; no
    # hardcoded SDK/MSVC directories are needed here.
endif()

# Enable high compiler warnings and warnings-as-errors to maintain code cleanliness for C++ files
if(MSVC)
    target_compile_options(pcsx5 PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_options(pcsx5 PRIVATE "/MAP")
endif()

# Set binary output directories
set_target_properties(pcsx5 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ------------------------------------------------------------------------
# dump_imports — load a real PS5 game ELF (eboot.bin.esbak) through the
# standard Loader::Load path and write the full import list to
# Games/PPSA02929-app0/imports.txt plus a per-library histogram.  Skips
# itself with a clear message if the game file is not present.
# ------------------------------------------------------------------------
add_executable(dump_imports
        tests/dump_imports.cpp
        src/common/log.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
    )
    target_include_directories(dump_imports PRIVATE src)
    target_compile_options(dump_imports PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME dump_imports COMMAND dump_imports)

    # Dump the dynamic-tag table of a real PS5 game ELF to identify
    # which DT_* / PT_* tags are actually used (Sony-specific tags
    # are common).  Mirrors the constants in Kyty's loader.
    add_executable(dump_dt
        tests/dump_dt.cpp
        src/common/log.cpp
    )
    target_include_directories(dump_dt PRIVATE src)
    target_compile_options(dump_dt PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME dump_dt COMMAND dump_dt)

    # ------------------------------------------------------------------------
    # Per-title compatibility database — schema round-trip, list, search,
    # auto-field update, remove/re-add, and markdown report.
    # ------------------------------------------------------------------------
    add_executable(compat_tests
        tests/compat_tests.cpp
        src/common/log.cpp
        src/compat/compat.cpp
    )
    target_include_directories(compat_tests PRIVATE src)
    target_compile_options(compat_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME compat COMMAND compat_tests)

    # ------------------------------------------------------------------------
    # pcsx5_compat — CLI for the compatibility database.  Mirrors the style
    # of dump_imports / dump_dt: a thin standalone tool that operates on the
    # compat module.
    # ------------------------------------------------------------------------
    add_executable(pcsx5_compat
        tests/compat_tool.cpp
        src/common/log.cpp
        src/compat/compat.cpp
    )
    target_include_directories(pcsx5_compat PRIVATE src)
    target_compile_options(pcsx5_compat PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME pcsx5_compat COMMAND pcsx5_compat --root $<CONFIG>/compat_seed list)

    # ------------------------------------------------------------------------
    # pcsx5_ui — the desktop UI for end users.  Dear ImGui + GLFW + OpenGL3
    # backend.  Out-of-process: it spawns pcsx5.exe as a child and pipes
    # stdout/stderr into the detachable console window.
    # ------------------------------------------------------------------------
    # pcsx5_ui — the desktop UI for end users. Compiled in C# (WPF) with native Windows 11 Mica.
    # Out-of-process: it spawns pcsx5.exe as a child and pipes stdout/stderr.
    # Requires the .NET SDK; skipped with a message when dotnet is not on PATH.
    find_program(DOTNET_EXE NAMES dotnet)
    if(DOTNET_EXE)
        add_custom_target(pcsx5_ui ALL
            COMMAND "${DOTNET_EXE}" build "${CMAKE_SOURCE_DIR}/src/ui_csharp/Pcsx5Ui.csproj" -c Release -o "${CMAKE_BINARY_DIR}/Release/"
            COMMENT "Building C# WPF Emulator UI (Mica, Fluent Design)"
        )
    else()
        message(STATUS "dotnet SDK not found on PATH — skipping pcsx5_ui target")
    endif()

    # ------------------------------------------------------------------------
    # pcsx5_boot_parser — standalone, independent game boot & memory parser.
    # Can be run individually without dependencies.
    # ------------------------------------------------------------------------
    add_executable(pcsx5_boot_parser
        pcsx5_boot_parser.cpp
        src/common/log.cpp
        src/loader/elf.cpp
        src/memory/memory.cpp
    )
    target_include_directories(pcsx5_boot_parser PRIVATE src)
    target_compile_options(pcsx5_boot_parser PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)

    # ------------------------------------------------------------------------
    # SndPreviewPlayer test — decodes the first snd0.at9 found under
    # ./Games/ to a PCM16 WAV and validates the header.
    # ------------------------------------------------------------------------
    add_executable(snd_player_test
        tests/snd_player_test.cpp
        src/ui/snd_player.cpp
        src/common/log.cpp
    )
    target_include_directories(snd_player_test PRIVATE
        src
        third_party
        third_party/LibAtrac9/include
    )
    target_link_libraries(snd_player_test PRIVATE atrac9)
    target_compile_options(snd_player_test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    if(MSVC)
        # winmm.lib is a Windows SDK system library; link it by name and let
        # the MSVC toolchain resolve it (no hardcoded SDK/MSVC paths).
        target_link_libraries(snd_player_test PRIVATE winmm)
    endif()
    add_test(NAME snd_player_test
             COMMAND snd_player_test
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    # ------------------------------------------------------------------------
    # pcsx5_snd_decode — standalone ATRAC9/OGG to WAV sound decoder.
    # ------------------------------------------------------------------------
    add_executable(pcsx5_snd_decode
        tests/pcsx5_snd_decode.cpp
        src/ui/snd_player.cpp
        src/common/log.cpp
    )
    target_include_directories(pcsx5_snd_decode PRIVATE
        src
        third_party
        third_party/LibAtrac9/include
    )
    target_link_libraries(pcsx5_snd_decode PRIVATE atrac9)
    target_compile_options(pcsx5_snd_decode PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    if(MSVC)
        # winmm.lib is a Windows SDK system library; link it by name and let
        # the MSVC toolchain resolve it (no hardcoded SDK/MSVC paths).
        target_link_libraries(pcsx5_snd_decode PRIVATE winmm)
    endif()

    # Set binary output directory to match pcsx5 target
    set_target_properties(pcsx5_snd_decode PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )


    # ------------------------------------------------------------------------
    # System info test — quick dump of CPU/GPU/RAM/OS as reported by
    # the cross-platform Sys module.
    # ------------------------------------------------------------------------
    add_executable(system_info_test
        tests/system_info_test.cpp
        src/system/system.cpp
    )
    target_include_directories(system_info_test PRIVATE src)
    target_compile_options(system_info_test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)

    # ------------------------------------------------------------------------
    # Syscall HLE validation tests — round-trip, arguments mapping, logging,
    # and exception handling verification.
    # ------------------------------------------------------------------------
    add_executable(syscall_validation_tests
        tests/syscall_validation_tests.cpp
        src/common/log.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
        ${KERNEL_SRC}
        ${CPU_SRC}
        src/gpu/vulkan_backend.cpp
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(syscall_validation_tests PRIVATE src)
    target_compile_options(syscall_validation_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(syscall_validation_tests PRIVATE glfw Vulkan::Headers nlohmann_json::nlohmann_json)
    add_test(NAME syscall_validation COMMAND syscall_validation_tests)

endif()
