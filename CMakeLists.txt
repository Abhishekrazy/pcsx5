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
    target_compile_options(loader_validation_tests PRIVATE /W4 /WX)
    add_test(NAME loader_validation COMMAND loader_validation_tests)

    add_executable(tls_context_tests
        tests/tls_context.cpp
        src/kernel/tls.cpp
    )
    target_include_directories(tls_context_tests PRIVATE src)
    target_compile_options(tls_context_tests PRIVATE /W4 /WX)
    add_test(NAME tls_context COMMAND tls_context_tests)
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
