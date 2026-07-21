cmake_minimum_required(VERSION 3.20)
cmake_policy(VERSION 3.20)
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
    src/kernel/guest_clock.h
    src/kernel/guest_clock.cpp
    src/kernel/memory.h
    src/kernel/memory.cpp
    src/kernel/thread.h
    src/kernel/thread.cpp
    src/kernel/syscalls.h
    src/kernel/syscalls.cpp
    src/kernel/tls.h
    src/kernel/tls.cpp
    src/kernel/tls_patch.h
    src/kernel/tls_patch.cpp
)

# CPU core — owns the guest thread registry; required wherever KERNEL_SRC is
# linked because kernel/thread.cpp is a shim over CpuCore.
set(CPU_SRC
    src/cpu/cpu.h
    src/cpu/cpu.cpp
    src/cpu/amd_compat.h
    src/cpu/amd_compat.cpp
)

# Shader recompiler (Phase 5 M2) — decoder, metadata reader, SPIR-V builder
# and the SPIR-V translator.  Required wherever libagc.cpp is linked: the M3
# draw path translates shaders at draw time.
set(SHADER_TRANSLATE_SRC
    src/gpu/shader/gcn_decode.h
    src/gpu/shader/gcn_decode.cpp
    src/gpu/shader/metadata.h
    src/gpu/shader/metadata.cpp
    src/gpu/shader/spirv_builder.h
    src/gpu/shader/spirv_builder.cpp
    src/gpu/shader/gcn_translate.h
    src/gpu/shader/gcn_translate_internal.h
    src/gpu/shader/gcn_translate.cpp
    src/gpu/shader/gcn_translate_alu.cpp
    src/gpu/shader/gcn_eval.h
    src/gpu/shader/gcn_eval.cpp
    # H7: disk cache for translated SPIR-V (owned by the shader-cache agent).
    src/gpu/shader_cache.h
    src/gpu/shader_cache.cpp
)

# ------------------------------------------------------------------------
# LibAtrac9 — vendored C port of Thealexbarney/LibAtrac9 (MIT).
# Used by the SndPreviewPlayer (src/ui/snd_player.cpp) and by the
# libSceAtrac9 HLE codec path (src/hle/libatrac9.cpp).  Defined at top
# level (not under BUILD_TESTING) so the pcsx5 target can link it.
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

add_executable(pcsx5
    src/main.cpp
    src/common/types.h
    src/common/log.h
    src/common/log.cpp
    src/memory/memory.h
    src/memory/memory.cpp
    src/common/crypto.h
    src/common/crypto.cpp
    src/loader/elf.h
    src/loader/elf.cpp
    src/loader/param_json.h
    src/loader/param_json.cpp
    src/loader/pkg.h
    src/loader/pkg.cpp
    src/loader/pfs.h
    src/loader/pfs.cpp
    src/loader/module_graph.h
    src/loader/module_graph.cpp
    src/loader/module_resolver.h
    src/loader/module_resolver.cpp
    src/hle/hle.h
    src/hle/hle.cpp
    src/hle/libkernel.cpp
    src/hle/libkernel_sync.h
    src/hle/libkernel_sync.cpp
    src/hle/libpad.cpp
    src/hle/libvideoout.cpp
    src/hle/libagc.cpp
    src/hle/libscepad.cpp
    src/hle/libsavedata.cpp
    src/hle/libuser_service.cpp
    src/hle/libsysmodule.cpp
    src/hle/guest_printf.cpp
    src/hle/liblibc.cpp
    src/hle/libsystem_service.cpp
    src/hle/libnp.cpp
    src/hle/libkeystone.cpp
    src/hle/libatrac9.cpp
    src/hle/libaudioout.cpp
    src/common/nid.cpp
    ${KERNEL_SRC}
    ${CPU_SRC}
    src/gpu/gpu.h
    src/gpu/vulkan_backend.cpp
    src/gpu/vk_context.cpp
    src/gpu/vk_present.cpp
    src/gpu/gfx10_state.cpp
    src/gpu/vk_draw.cpp
    ${SHADER_TRANSLATE_SRC}
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
    # Manual GPU smoke tool (Phase 5 M1) — NOT a ctest: requires a real GPU
    # and opens a window.  Presents synthetic frames through the same
    # GPU::RenderFrame entry point the videoout flip path uses.
    add_executable(vk_present_smoke
        tests/vk_present_smoke.cpp
        src/common/log.cpp
        src/memory/memory.cpp
        src/gpu/vulkan_backend.cpp
        src/gpu/vk_context.cpp
        src/gpu/vk_present.cpp
        src/gpu/gfx10_state.cpp
        src/gpu/vk_draw.cpp
    )
    target_include_directories(vk_present_smoke PRIVATE src)
    target_compile_options(vk_present_smoke PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(vk_present_smoke PRIVATE glfw Vulkan::Headers)

    # Present-path format helper unit tests (SharpEmu #448 port) — no GPU
    # required: exercises VkPresentSrgbCounterpart / VkPresentIsLinearFloatSource.
    add_executable(vk_present_format_tests
        tests/vk_present_format_tests.cpp
        src/common/log.cpp
        src/gpu/vk_context.cpp
        src/gpu/vk_present.cpp
    )
    target_include_directories(vk_present_format_tests PRIVATE src)
    target_compile_options(vk_present_format_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(vk_present_format_tests PRIVATE glfw Vulkan::Headers)
    add_test(NAME vk_present_format COMMAND vk_present_format_tests)

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

    # Guest image write-tracker tests (SharpEmu #447 port): write-generation
    # invariants backing the vk_draw texture-cache refresh.
    add_executable(memory_write_tracker_tests
        tests/memory_write_tracker_tests.cpp
        src/common/log.cpp
        src/memory/memory.cpp
    )
    target_include_directories(memory_write_tracker_tests PRIVATE src)
    target_compile_options(memory_write_tracker_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME memory_write_tracker COMMAND memory_write_tracker_tests)

    # Kernel guest-memory protection mapping tests (ProtectGuestMemory).
    add_executable(kernel_memory_tests
        tests/kernel_memory_tests.cpp
        src/kernel/memory.cpp
    )
    target_include_directories(kernel_memory_tests PRIVATE src)
    target_compile_options(kernel_memory_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME kernel_memory COMMAND kernel_memory_tests)

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
        src/common/crypto.cpp
        src/config/config.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
        src/loader/module_resolver.cpp
        src/loader/module_graph.cpp
        ${KERNEL_SRC}
        ${CPU_SRC}
        src/gpu/vulkan_backend.cpp
        src/gpu/vk_context.cpp
        src/gpu/vk_present.cpp
        src/gpu/gfx10_state.cpp
        src/gpu/vk_draw.cpp
    src/gpu/vk_context.cpp
    src/gpu/vk_present.cpp
    src/gpu/gfx10_state.cpp
    src/gpu/vk_draw.cpp
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libkernel_sync.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        ${SHADER_TRANSLATE_SRC}
        src/hle/libscepad.cpp
        src/hle/libsavedata.cpp
        src/hle/libuser_service.cpp
    src/hle/libsysmodule.cpp
    src/hle/guest_printf.cpp
    src/hle/liblibc.cpp
    src/hle/libsystem_service.cpp
    src/hle/libnp.cpp
    src/hle/libkeystone.cpp
    src/hle/libatrac9.cpp
    src/hle/libaudioout.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(hle_import_report_tests PRIVATE src)
    target_compile_options(hle_import_report_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(hle_import_report_tests PRIVATE glfw Vulkan::Headers nlohmann_json::nlohmann_json atrac9)
    add_test(NAME hle_import_report COMMAND hle_import_report_tests)

    # ------------------------------------------------------------------------
    # Reports tests — CompatSummary jsonl history, regression verdict,
    # aggregated markdown report.
    # ------------------------------------------------------------------------
    add_executable(report_tests
        tests/report_tests.cpp
        src/common/log.cpp
        src/common/crypto.cpp
        src/config/config.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
        src/loader/module_resolver.cpp
        src/loader/module_graph.cpp
        ${KERNEL_SRC}
        ${CPU_SRC}
        src/gpu/vulkan_backend.cpp
        src/gpu/vk_context.cpp
        src/gpu/vk_present.cpp
        src/gpu/gfx10_state.cpp
        src/gpu/vk_draw.cpp
    src/gpu/vk_context.cpp
    src/gpu/vk_present.cpp
    src/gpu/gfx10_state.cpp
    src/gpu/vk_draw.cpp
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libkernel_sync.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        ${SHADER_TRANSLATE_SRC}
        src/hle/libscepad.cpp
        src/hle/libsavedata.cpp
        src/hle/libuser_service.cpp
    src/hle/libsysmodule.cpp
    src/hle/guest_printf.cpp
    src/hle/liblibc.cpp
    src/hle/libsystem_service.cpp
    src/hle/libnp.cpp
    src/hle/libkeystone.cpp
    src/hle/libatrac9.cpp
    src/hle/libaudioout.cpp
        src/reports/reports.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(report_tests PRIVATE src)
    target_compile_options(report_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(report_tests PRIVATE glfw Vulkan::Headers nlohmann_json::nlohmann_json atrac9)
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
        src/common/crypto.cpp
        src/config/config.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
        src/loader/module_resolver.cpp
        src/loader/module_graph.cpp
        ${KERNEL_SRC}
        ${CPU_SRC}
        src/gpu/vulkan_backend.cpp
        src/gpu/vk_context.cpp
        src/gpu/vk_present.cpp
        src/gpu/gfx10_state.cpp
        src/gpu/vk_draw.cpp
    src/gpu/vk_context.cpp
    src/gpu/vk_present.cpp
    src/gpu/gfx10_state.cpp
    src/gpu/vk_draw.cpp
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libkernel_sync.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        ${SHADER_TRANSLATE_SRC}
        src/hle/libscepad.cpp
        src/hle/libsavedata.cpp
        src/hle/libuser_service.cpp
    src/hle/libsysmodule.cpp
    src/hle/guest_printf.cpp
    src/hle/liblibc.cpp
    src/hle/libsystem_service.cpp
    src/hle/libnp.cpp
    src/hle/libkeystone.cpp
    src/hle/libatrac9.cpp
    src/hle/libaudioout.cpp
        src/diagnostics/diagnostics.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(diagnostics_tests PRIVATE src)
    target_compile_options(diagnostics_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(diagnostics_tests PRIVATE glfw Vulkan::Headers nlohmann_json::nlohmann_json atrac9)
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

    if(CLANG_EXECUTABLE)
        # Copy every freestanding test source next to the build tree so the
        # .bat file can operate on a stable working directory.  build_all.bat
        # derives source names from the ELF name (<elf>_main.cpp), so the
        # copies must be renamed accordingly.
        file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test_elf)
        file(COPY_FILE ${CMAKE_SOURCE_DIR}/tests/test_elf/main.cpp
             ${CMAKE_BINARY_DIR}/test_elf/test_guest_main.cpp)
        file(COPY_FILE ${CMAKE_SOURCE_DIR}/tests/test_elf/tls_main.cpp
             ${CMAKE_BINARY_DIR}/test_elf/tls_guest_main.cpp)
        file(COPY ${CMAKE_SOURCE_DIR}/tests/test_elf/build_all.bat
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
        # ALL: ctest does not build targets — the ELFs must be produced by the
        # default build or the smoke tests fail with missing files (CI bug).
        add_custom_target(guest_test_elfs ALL
            DEPENDS ${CMAKE_BINARY_DIR}/test_elf/test_guest.elf
                    ${CMAKE_BINARY_DIR}/test_elf/tls_guest.elf
        )

        # Syscall smoke test: writes "Hello Guest\n" via sys_write and exits.
        add_test(NAME guest_syscall_smoke
            COMMAND pcsx5 --strict-imports --report=${CMAKE_BINARY_DIR}/guest_syscall_report.json
                    ${CMAKE_BINARY_DIR}/test_elf/test_guest.elf
        )
        set_tests_properties(guest_syscall_smoke PROPERTIES
            WILL_FAIL FALSE
            PASS_REGULAR_EXPRESSION "Hello Guest"
            DEPENDS guest_test_elfs
        )

        # TLS smoke test: uses fs:0 read/write to verify the VEH trap.
        add_test(NAME guest_tls_smoke
            COMMAND pcsx5 --strict-imports --report=${CMAKE_BINARY_DIR}/guest_tls_report.json
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
target_compile_definitions(pcsx5 PRIVATE PCSX5_HAS_LUA PCSX5_TLS_PATCH_FULL)

# Link GLFW, Vulkan Headers, and Lua dependencies
target_link_libraries(pcsx5 PRIVATE glfw Vulkan::Headers lua ws2_32 nlohmann_json::nlohmann_json atrac9)
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

    # ------------------------------------------------------------------------
    # nid_scan — scan dumped PS5 PRX/SPRX modules and emit nid_db.txt lines
    # (NID<TAB>module<TAB>name) on stdout.  Standalone dev tool; requires
    # real module dumps as input, so it is not registered as a ctest test
    # (same policy as make_test_covers).
    # ------------------------------------------------------------------------
    add_executable(nid_scan
        tools/nid_scan.cpp
        src/common/log.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
    )
    target_include_directories(nid_scan PRIVATE src)
    target_compile_options(nid_scan PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)

    # ------------------------------------------------------------------------
    # dualsense_test — standalone console utility that opens the DualSense
    # through the header-only reader in src/gpu/dualsense_hid.h and shows a
    # live view of every parsed input (buttons/sticks/triggers/touch/gyro/
    # accel/transport) plus keyboard-driven output tests (rumble + adaptive
    # trigger effects).  Interactive dev tool; not a ctest test.
    # ------------------------------------------------------------------------
    add_executable(dualsense_test
        tools/dualsense_test.cpp
        src/common/log.cpp
    )
    target_include_directories(dualsense_test PRIVATE src)
    target_compile_options(dualsense_test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)

    # ------------------------------------------------------------------------
    # dualsense_visual — windowed Dear ImGui DualSense tester.  Same input
    # path as dualsense_test (src/gpu/dualsense_hid.h) rendered live: buttons
    # with a per-button "tested" checklist, stick plots, L2/R2 bars, touchpad
    # finger view, gyro/accel, plus rumble and adaptive-trigger output
    # controls.  Interactive dev tool; not a ctest test.
    # ------------------------------------------------------------------------
    add_executable(dualsense_visual
        tools/dualsense_visual.cpp
        src/common/log.cpp
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(dualsense_visual PRIVATE src ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_compile_options(dualsense_visual PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    # ImGui is third-party: keep it at /W3 without /WX (MSVC warns on the
    # OpenGL3 backend under /W4, e.g. C4701 on last_pixel_unpack_buffer).
    set_source_files_properties(
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
        TARGET_DIRECTORY dualsense_visual
        PROPERTIES COMPILE_OPTIONS "/W3"
    )
    target_link_libraries(dualsense_visual PRIVATE glfw opengl32)

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
    # Phase 1 — crypto primitives (AES-128, SHA-256) test vectors.
    # ------------------------------------------------------------------------
    add_executable(crypto_tests
        tests/crypto_tests.cpp
        src/common/log.cpp
        src/common/crypto.cpp
    )
    target_include_directories(crypto_tests PRIVATE src)
    target_compile_options(crypto_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME crypto COMMAND crypto_tests)

    # ------------------------------------------------------------------------
    # Phase 1 — param.json parser tests (title id, version, category).
    # ------------------------------------------------------------------------
    add_executable(param_json_tests
        tests/param_json_tests.cpp
        src/common/log.cpp
        src/loader/param_json.cpp
    )
    target_include_directories(param_json_tests PRIVATE src)
    target_compile_options(param_json_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(param_json_tests PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME param_json COMMAND param_json_tests)

    # ------------------------------------------------------------------------
    # Phase 1 — PKG container tests — synthetic fake-PKG fixtures: header,
    # entry table, passcode key derivation, AES decrypt, extraction.
    # ------------------------------------------------------------------------
    add_executable(pkg_tests
        tests/pkg_tests.cpp
        src/common/log.cpp
        src/common/crypto.cpp
        src/loader/pkg.cpp
    )
    target_include_directories(pkg_tests PRIVATE src)
    target_compile_options(pkg_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME pkg COMMAND pkg_tests)

    # ------------------------------------------------------------------------
    # Phase 1 — PFS image parser tests (read-only) — synthetic PFS fixtures.
    # ------------------------------------------------------------------------
    add_executable(pfs_tests
        tests/pfs_tests.cpp
        src/common/log.cpp
        src/loader/pfs.cpp
    )
    target_include_directories(pfs_tests PRIVATE src)
    target_compile_options(pfs_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME pfs COMMAND pfs_tests)

    # ------------------------------------------------------------------------
    # Phase 7 — keystone header parser tests — synthetic keystone blobs.
    # HLE::RegisterSymbol / Memory::ReadBuffer are no-op stubbed inside the
    # test file, so only the parser TU itself is linked.
    # ------------------------------------------------------------------------
    add_executable(keystone_tests
        tests/keystone_tests.cpp
        src/common/log.cpp
        src/hle/libkeystone.cpp
    )
    target_include_directories(keystone_tests PRIVATE src)
    target_compile_options(keystone_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME keystone COMMAND keystone_tests)

    # ------------------------------------------------------------------------
    # Phase 1 — NID database file tests — parse assets/nid_db.txt format,
    # merge over built-in table, precedence rules.
    # ------------------------------------------------------------------------
    add_executable(nid_db_tests
        tests/nid_db_tests.cpp
        src/common/log.cpp
        src/common/nid.cpp
    )
    target_include_directories(nid_db_tests PRIVATE src)
    target_compile_options(nid_db_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME nid_db COMMAND nid_db_tests)

    # ------------------------------------------------------------------------
    # Phase 1 — Module dependency graph tests — topological load order with
    # chains, diamonds, cycles, and missing nodes.
    # ------------------------------------------------------------------------
    add_executable(module_graph_tests
        tests/module_graph_tests.cpp
        src/common/log.cpp
        src/loader/module_graph.cpp
        src/loader/module_resolver.cpp
    )
    target_include_directories(module_graph_tests PRIVATE src)
    target_compile_options(module_graph_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME module_graph COMMAND module_graph_tests)

    # ------------------------------------------------------------------------
    # SSE4a bit-field emulator tests — EXTRQ/INSERTQ immediate-form semantics
    # and the AMD-only instruction decoder used by the VEH illegal-instruction
    # software fallback (port of SharpEmu Sse4aBitFieldEmulatorTests).
    # ------------------------------------------------------------------------
    add_executable(sse4a_bitfield_tests
        tests/sse4a_bitfield_tests.cpp
        src/cpu/amd_compat.cpp
    )
    target_include_directories(sse4a_bitfield_tests PRIVATE src)
    target_compile_options(sse4a_bitfield_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME sse4a_bitfield COMMAND sse4a_bitfield_tests)

    # ------------------------------------------------------------------------
    # Syscall HLE validation tests — round-trip, arguments mapping, logging,
    # and exception handling verification.
    # ------------------------------------------------------------------------
    add_executable(syscall_validation_tests
        tests/syscall_validation_tests.cpp
        src/common/log.cpp
        src/common/crypto.cpp
        src/config/config.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
        src/loader/module_resolver.cpp
        src/loader/module_graph.cpp
        ${KERNEL_SRC}
        ${CPU_SRC}
        src/gpu/vulkan_backend.cpp
        src/gpu/vk_context.cpp
        src/gpu/vk_present.cpp
        src/gpu/gfx10_state.cpp
        src/gpu/vk_draw.cpp
    src/gpu/vk_context.cpp
    src/gpu/vk_present.cpp
    src/gpu/gfx10_state.cpp
    src/gpu/vk_draw.cpp
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libkernel_sync.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        ${SHADER_TRANSLATE_SRC}
        src/hle/libscepad.cpp
        src/hle/libsavedata.cpp
        src/hle/libuser_service.cpp
    src/hle/libsysmodule.cpp
    src/hle/guest_printf.cpp
    src/hle/liblibc.cpp
    src/hle/libsystem_service.cpp
    src/hle/libnp.cpp
    src/hle/libkeystone.cpp
    src/hle/libatrac9.cpp
    src/hle/libaudioout.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(syscall_validation_tests PRIVATE src)
    target_compile_options(syscall_validation_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(syscall_validation_tests PRIVATE glfw Vulkan::Headers nlohmann_json::nlohmann_json atrac9)
    add_test(NAME syscall_validation COMMAND syscall_validation_tests)

    # ------------------------------------------------------------------------
    # Phase 2 — kernel sync primitive tests: real pthread mutex/cond/rwlock,
    # TLS keys, kernel semaphores, event flags, equeue, and the shared guest
    # clock (src/hle/libkernel_sync.cpp + src/kernel/guest_clock.cpp).
    # ------------------------------------------------------------------------
    add_executable(kernel_sync_tests
        tests/kernel_sync_tests.cpp
        src/common/log.cpp
        src/common/crypto.cpp
        src/config/config.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
        src/loader/module_resolver.cpp
        src/loader/module_graph.cpp
        ${KERNEL_SRC}
        ${CPU_SRC}
        src/gpu/vulkan_backend.cpp
        src/gpu/vk_context.cpp
        src/gpu/vk_present.cpp
        src/gpu/gfx10_state.cpp
        src/gpu/vk_draw.cpp
    src/gpu/vk_context.cpp
    src/gpu/vk_present.cpp
    src/gpu/gfx10_state.cpp
    src/gpu/vk_draw.cpp
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libkernel_sync.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        ${SHADER_TRANSLATE_SRC}
        src/hle/libscepad.cpp
        src/hle/libsavedata.cpp
        src/hle/libuser_service.cpp
    src/hle/libsysmodule.cpp
    src/hle/guest_printf.cpp
    src/hle/liblibc.cpp
    src/hle/libsystem_service.cpp
    src/hle/libnp.cpp
    src/hle/libkeystone.cpp
    src/hle/libatrac9.cpp
    src/hle/libaudioout.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(kernel_sync_tests PRIVATE src)
    target_compile_options(kernel_sync_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(kernel_sync_tests PRIVATE glfw Vulkan::Headers nlohmann_json::nlohmann_json atrac9)
    add_test(NAME kernel_sync COMMAND kernel_sync_tests)

    # ------------------------------------------------------------------------
    # libkernel file-I/O tests: POSIX open/close/read/write/fstat/stat
    # -1/errno convention and sceKernelMapDirectMemory2 (src/hle/libkernel.cpp,
    # driven via src/hle/libkernel_file.h).
    # ------------------------------------------------------------------------
    add_executable(libkernel_file_tests
        tests/libkernel_file_tests.cpp
        src/common/log.cpp
        src/common/crypto.cpp
        src/config/config.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
        src/loader/module_resolver.cpp
        src/loader/module_graph.cpp
        ${KERNEL_SRC}
        ${CPU_SRC}
        src/gpu/vulkan_backend.cpp
        src/gpu/vk_context.cpp
        src/gpu/vk_present.cpp
        src/gpu/gfx10_state.cpp
        src/gpu/vk_draw.cpp
    src/gpu/vk_context.cpp
    src/gpu/vk_present.cpp
    src/gpu/gfx10_state.cpp
    src/gpu/vk_draw.cpp
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libkernel_sync.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        ${SHADER_TRANSLATE_SRC}
        src/hle/libscepad.cpp
        src/hle/libsavedata.cpp
        src/hle/libuser_service.cpp
    src/hle/libsysmodule.cpp
    src/hle/guest_printf.cpp
    src/hle/liblibc.cpp
    src/hle/libsystem_service.cpp
    src/hle/libnp.cpp
    src/hle/libkeystone.cpp
    src/hle/libatrac9.cpp
    src/hle/libaudioout.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(libkernel_file_tests PRIVATE src)
    target_compile_options(libkernel_file_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(libkernel_file_tests PRIVATE glfw Vulkan::Headers nlohmann_json::nlohmann_json atrac9)
    add_test(NAME libkernel_file COMMAND libkernel_file_tests)

    # ------------------------------------------------------------------------
    # Phase 3 — HLE module tests: libSceSysmodule id registry, libc guest
    # heap, libSceSystemService canned parameters, NP offline-signed-in set,
    # libSceVideoOut port/flip model with equeue notifications, and the
    # NID-database gap-filler stubs (tests/hle_phase3_tests.cpp).
    # ------------------------------------------------------------------------
    add_executable(hle_phase3_tests
        tests/hle_phase3_tests.cpp
        src/common/log.cpp
        src/common/crypto.cpp
        src/config/config.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
        src/loader/module_resolver.cpp
        src/loader/module_graph.cpp
        ${KERNEL_SRC}
        ${CPU_SRC}
        src/gpu/vulkan_backend.cpp
        src/gpu/vk_context.cpp
        src/gpu/vk_present.cpp
        src/gpu/gfx10_state.cpp
        src/gpu/vk_draw.cpp
    src/gpu/vk_context.cpp
    src/gpu/vk_present.cpp
    src/gpu/gfx10_state.cpp
    src/gpu/vk_draw.cpp
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libkernel_sync.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        ${SHADER_TRANSLATE_SRC}
        src/hle/libscepad.cpp
        src/hle/libsavedata.cpp
        src/hle/libuser_service.cpp
        src/hle/libsysmodule.cpp
        src/hle/guest_printf.cpp
    src/hle/liblibc.cpp
        src/hle/libsystem_service.cpp
        src/hle/libnp.cpp
        src/hle/libkeystone.cpp
        src/hle/libatrac9.cpp
    src/hle/libaudioout.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(hle_phase3_tests PRIVATE src)
    target_compile_options(hle_phase3_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(hle_phase3_tests PRIVATE glfw Vulkan::Headers nlohmann_json::nlohmann_json atrac9)
    add_test(NAME hle_phase3 COMMAND hle_phase3_tests)

    # ------------------------------------------------------------------------
    # L1 — audio + pad HLE tests: libSceAudioOut port state machine
    # (Init/Open/Output paced silence/SetVolume/GetPortState/Close,
    # PORT_FULL) and libScePad handle validation + ScePadData reads
    # (tests/hle_audio_pad_tests.cpp).
    # ------------------------------------------------------------------------
    add_executable(hle_audio_pad_tests
        tests/hle_audio_pad_tests.cpp
        src/common/log.cpp
        src/common/crypto.cpp
        src/config/config.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
        src/loader/module_resolver.cpp
        src/loader/module_graph.cpp
        ${KERNEL_SRC}
        ${CPU_SRC}
        src/gpu/vulkan_backend.cpp
        src/gpu/vk_context.cpp
        src/gpu/vk_present.cpp
        src/gpu/gfx10_state.cpp
        src/gpu/vk_draw.cpp
    src/gpu/vk_context.cpp
    src/gpu/vk_present.cpp
    src/gpu/gfx10_state.cpp
    src/gpu/vk_draw.cpp
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libkernel_sync.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        ${SHADER_TRANSLATE_SRC}
        src/hle/libscepad.cpp
        src/hle/libsavedata.cpp
        src/hle/libuser_service.cpp
    src/hle/libsysmodule.cpp
    src/hle/guest_printf.cpp
    src/hle/liblibc.cpp
        src/hle/libsystem_service.cpp
        src/hle/libnp.cpp
        src/hle/libkeystone.cpp
        src/hle/libatrac9.cpp
    src/hle/libaudioout.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(hle_audio_pad_tests PRIVATE src)
    target_compile_options(hle_audio_pad_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(hle_audio_pad_tests PRIVATE glfw Vulkan::Headers nlohmann_json::nlohmann_json atrac9)
    add_test(NAME hle_audio_pad COMMAND hle_audio_pad_tests)

    # ------------------------------------------------------------------------
    # Phase 5 M0 — AGC HLE tests: sceAgcCreateShader shader ABI (relocation,
    # PGM patch, handle contract), Gen5 register-defaults blobs, sceAgcDcb*
    # PM4 builders, and the sceAgcDriverSubmitDcb packet-stream walker
    # (tests/hle_agc_tests.cpp).
    # ------------------------------------------------------------------------
    add_executable(hle_agc_tests
        tests/hle_agc_tests.cpp
        src/common/log.cpp
        src/common/crypto.cpp
        src/config/config.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
        src/loader/module_resolver.cpp
        src/loader/module_graph.cpp
        ${KERNEL_SRC}
        ${CPU_SRC}
        src/gpu/vulkan_backend.cpp
        src/gpu/vk_context.cpp
        src/gpu/vk_present.cpp
        src/gpu/gfx10_state.cpp
        src/gpu/vk_draw.cpp
    src/gpu/vk_context.cpp
    src/gpu/vk_present.cpp
    src/gpu/gfx10_state.cpp
    src/gpu/vk_draw.cpp
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libkernel_sync.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        ${SHADER_TRANSLATE_SRC}
        src/hle/libscepad.cpp
        src/hle/libsavedata.cpp
        src/hle/libuser_service.cpp
        src/hle/libsysmodule.cpp
        src/hle/guest_printf.cpp
    src/hle/liblibc.cpp
        src/hle/libsystem_service.cpp
        src/hle/libnp.cpp
        src/hle/libkeystone.cpp
        src/hle/libatrac9.cpp
    src/hle/libaudioout.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(hle_agc_tests PRIVATE src)
    target_compile_options(hle_agc_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(hle_agc_tests PRIVATE glfw Vulkan::Headers nlohmann_json::nlohmann_json atrac9)
    add_test(NAME hle_agc COMMAND hle_agc_tests)

    # ------------------------------------------------------------------------
    # Phase 5 M1 — guest printf family tests: SysV varargs formatting
    # (%s/%d/%x/%u/%%, width, precision, length modifiers, '*' + stack
    # overflow args) and the registered libc::sprintf/libc::memcmp symbols
    # (tests/guest_printf_tests.cpp).
    # ------------------------------------------------------------------------
    add_executable(guest_printf_tests
        tests/guest_printf_tests.cpp
        src/common/log.cpp
        src/common/crypto.cpp
        src/config/config.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
        src/loader/module_resolver.cpp
        src/loader/module_graph.cpp
        ${KERNEL_SRC}
        ${CPU_SRC}
        src/gpu/vulkan_backend.cpp
        src/gpu/vk_context.cpp
        src/gpu/vk_present.cpp
        src/gpu/gfx10_state.cpp
        src/gpu/vk_draw.cpp
    src/gpu/vk_context.cpp
    src/gpu/vk_present.cpp
    src/gpu/gfx10_state.cpp
    src/gpu/vk_draw.cpp
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libkernel_sync.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        ${SHADER_TRANSLATE_SRC}
        src/hle/libscepad.cpp
        src/hle/libsavedata.cpp
        src/hle/libuser_service.cpp
        src/hle/libsysmodule.cpp
        src/hle/guest_printf.cpp
        src/hle/liblibc.cpp
        src/hle/libsystem_service.cpp
        src/hle/libnp.cpp
        src/hle/libkeystone.cpp
        src/hle/libatrac9.cpp
    src/hle/libaudioout.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(guest_printf_tests PRIVATE src)
    target_compile_options(guest_printf_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(guest_printf_tests PRIVATE glfw Vulkan::Headers nlohmann_json::nlohmann_json atrac9)
    add_test(NAME guest_printf COMMAND guest_printf_tests)

    # ------------------------------------------------------------------------
    # H3 — guest C++ exception ABI tests: __cxa_allocate_exception layout,
    # begin/end-catch bookkeeping, guard variables, _Unwind context accessors
    # (tests/libcxxabi_tests.cpp, surface implemented in src/hle/liblibc.cpp).
    # ------------------------------------------------------------------------
    add_executable(libcxxabi_tests
        tests/libcxxabi_tests.cpp
        src/common/log.cpp
        src/common/crypto.cpp
        src/config/config.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
        src/loader/module_resolver.cpp
        src/loader/module_graph.cpp
        ${KERNEL_SRC}
        ${CPU_SRC}
        src/gpu/vulkan_backend.cpp
        src/gpu/vk_context.cpp
        src/gpu/vk_present.cpp
        src/gpu/gfx10_state.cpp
        src/gpu/vk_draw.cpp
        ${SHADER_TRANSLATE_SRC}
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libkernel_sync.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        src/hle/libscepad.cpp
        src/hle/libsavedata.cpp
        src/hle/libuser_service.cpp
        src/hle/libsysmodule.cpp
        src/hle/guest_printf.cpp
        src/hle/liblibc.cpp
        src/hle/libsystem_service.cpp
        src/hle/libnp.cpp
        src/hle/libkeystone.cpp
        src/hle/libatrac9.cpp
        src/hle/libaudioout.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(libcxxabi_tests PRIVATE src)
    target_compile_options(libcxxabi_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(libcxxabi_tests PRIVATE glfw Vulkan::Headers nlohmann_json::nlohmann_json atrac9)
    add_test(NAME libcxxabi COMMAND libcxxabi_tests)

    # ------------------------------------------------------------------------
    # Phase 5 M2 slice 1 — shader recompiler foundation: RDNA2 (GCN Gen5)
    # instruction decoder, AGC shader metadata reader, SPIR-V module builder
    # (tests/shader_tests.cpp).  shader_dump is the offline corpus tool — a
    # CLI target only, NOT a ctest.
    # ------------------------------------------------------------------------
    set(SHADER_SRC
        ${SHADER_TRANSLATE_SRC}
    )

    add_executable(shader_tests
        tests/shader_tests.cpp
        ${SHADER_SRC}
        src/memory/memory.cpp
        src/common/log.cpp
    )
    target_include_directories(shader_tests PRIVATE src)
    target_compile_options(shader_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME shader COMMAND shader_tests)

    # ------------------------------------------------------------------------
    # Phase 5 M3.2c — Gfx10 guest render-state decoders: topology, blend,
    # raster, viewport/scissor, render-target + image-descriptor decode
    # (tests/gfx10_state_tests.cpp).  Pure logic, no Vulkan device.
    # ------------------------------------------------------------------------
    add_executable(gfx10_state_tests
        tests/gfx10_state_tests.cpp
        src/gpu/gfx10_state.cpp
        src/common/log.cpp
    )
    target_include_directories(gfx10_state_tests PRIVATE src)
    target_compile_options(gfx10_state_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(gfx10_state_tests PRIVATE Vulkan::Headers)
    add_test(NAME gfx10_state COMMAND gfx10_state_tests)

    add_executable(shader_dump
        tests/shader_dump.cpp
        ${SHADER_SRC}
        src/memory/memory.cpp
        src/common/log.cpp
    )
    target_include_directories(shader_dump PRIVATE src)
    target_compile_options(shader_dump PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(shader_dump PRIVATE Vulkan::Headers)

    # ------------------------------------------------------------------------
    # Phase 5 validation — PM4 capture/replay tool + golden-image ctest
    # (tools/pm4_replay.cpp).  Replays a capture (PCSX5_PM4_CAPTURE format or
    # its own record-synth output) through the real HLE submit walker +
    # videoout flip path on a hidden Vulkan window, reads each presented
    # frame back via the vk_present readback hook, and compares against
    # golden PNGs (tests/golden/pm4_synth).  The ctest exits 0 with
    # PM4_GOLDEN_SKIP when no usable Vulkan device exists (CI without GPU);
    # set PCSX5_GOLDEN_REQUIRED=1 to turn the skip into a failure.
    # ------------------------------------------------------------------------
    add_executable(pm4_replay
        tools/pm4_replay.cpp
        src/common/log.cpp
        src/common/crypto.cpp
        src/config/config.cpp
        src/common/nid.cpp
        src/memory/memory.cpp
        src/loader/elf.cpp
        src/loader/module_resolver.cpp
        src/loader/module_graph.cpp
        ${KERNEL_SRC}
        ${CPU_SRC}
        src/gpu/vulkan_backend.cpp
        src/gpu/vk_context.cpp
        src/gpu/vk_present.cpp
        src/gpu/gfx10_state.cpp
        src/gpu/vk_draw.cpp
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libkernel_sync.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        ${SHADER_TRANSLATE_SRC}
        src/hle/libscepad.cpp
        src/hle/libsavedata.cpp
        src/hle/libuser_service.cpp
        src/hle/libsysmodule.cpp
        src/hle/guest_printf.cpp
        src/hle/liblibc.cpp
        src/hle/libsystem_service.cpp
        src/hle/libnp.cpp
        src/hle/libkeystone.cpp
        src/hle/libatrac9.cpp
        src/hle/libaudioout.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(pm4_replay PRIVATE src)
    target_compile_options(pm4_replay PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(pm4_replay PRIVATE glfw Vulkan::Headers nlohmann_json::nlohmann_json atrac9)
    add_test(NAME pm4_golden
        COMMAND pm4_replay replay ${CMAKE_SOURCE_DIR}/tests/golden/pm4_synth
                --golden ${CMAKE_SOURCE_DIR}/tests/golden/pm4_synth)

endif()
