cmake_minimum_required(VERSION 3.20)
project(pcsx5 LANGUAGES CXX)

# Force C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configure ASM MASM for x64 Windows assembly compilation
if(MSVC)
    enable_language(ASM_MASM)
    set(DISPATCHER_SRC src/hle/dispatcher.asm)
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
    src/kernel/kernel.h
    src/kernel/kernel.cpp
    src/kernel/tls.h
    src/kernel/tls.cpp
    src/gpu/gpu.h
    src/gpu/vulkan_backend.cpp
    ${DISPATCHER_SRC}
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

    add_executable(memory_validation_tests
        tests/memory_validation.cpp
        src/common/log.cpp
        src/memory/memory.cpp
    )
    target_include_directories(memory_validation_tests PRIVATE src)
    target_compile_options(memory_validation_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    add_test(NAME memory_validation COMMAND memory_validation_tests)

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
        src/memory/memory.cpp
        src/loader/elf.cpp
        src/kernel/tls.cpp
        src/kernel/kernel.cpp
        src/gpu/vulkan_backend.cpp
        src/hle/hle.cpp
        src/hle/libkernel.cpp
        src/hle/libpad.cpp
        src/hle/libvideoout.cpp
        src/hle/libagc.cpp
        ${DISPATCHER_SRC}
    )
    target_include_directories(hle_import_report_tests PRIVATE src)
    target_compile_options(hle_import_report_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_libraries(hle_import_report_tests PRIVATE glfw Vulkan::Headers)
    add_test(NAME hle_import_report COMMAND hle_import_report_tests)

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

    if(CLANG_EXECUTABLE)
        # Copy every freestanding test source next to the build tree so the
        # .bat file can operate on a stable working directory.
        file(COPY ${CMAKE_SOURCE_DIR}/tests/test_elf/main.cpp
                  ${CMAKE_SOURCE_DIR}/tests/test_elf/tls_main.cpp
                  ${CMAKE_SOURCE_DIR}/tests/test_elf/build_all.bat
                  ${CMAKE_SOURCE_DIR}/tests/test_elf/build_elf.bat
                  ${CMAKE_SOURCE_DIR}/tests/test_elf/build_tls_elf.bat
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
endif()
# Link GLFW and Vulkan Header dependencies
target_link_libraries(pcsx5 PRIVATE glfw Vulkan::Headers)

# Enable high compiler warnings and warnings-as-errors to maintain code cleanliness for C++ files
if(MSVC)
    target_compile_options(pcsx5 PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
    target_link_options(pcsx5 PRIVATE "/MAP")
endif()

# Set binary output directories
set_target_properties(pcsx5 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
