cmake_minimum_required(VERSION 3.25)

project(pcsx5_clean_rebuild VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

function(pcsx5_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    endif()
endfunction()

add_subdirectory(core)
add_subdirectory(runtime)
add_subdirectory(execution)
add_subdirectory(graphics)
add_subdirectory(frontend)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

include(cmake/CoreBoundary.cmake)
# Run after all subdirectories and their deferred work, so later sibling
# changes to the real target are checked too.
cmake_language(DEFER CALL pcsx5_check_core_target pcsx5_core "${CMAKE_SOURCE_DIR}/core")
add_custom_target(pcsx5_core_boundary_check
    COMMAND "${CMAKE_COMMAND}" "-DCORE_ROOT=${CMAKE_SOURCE_DIR}/core"
        -P "${CMAKE_SOURCE_DIR}/cmake/CheckCoreIncludes.cmake"
    VERBATIM)
add_dependencies(pcsx5_core pcsx5_core_boundary_check)
