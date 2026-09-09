cmake_minimum_required(VERSION 3.25)

project(pcsx5_clean_rebuild VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(ANDROID)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

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

if(NOT ANDROID)
    install(FILES LICENSE ARCHITECTURE.md REBUILD_STATUS.md DESTINATION share/pcsx5)
    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGE_NAME pcsx5-experimental)
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_FILE_NAME "pcsx5-experimental-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_PACKAGE_CHECKSUM SHA256)
    include(CPack)
    if(BUILD_TESTING)
        add_test(NAME pcsx5_package_acceptance COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DCPACK_COMMAND=${CMAKE_CPACK_COMMAND}"
            "-DEXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
            -P "${CMAKE_SOURCE_DIR}/tests/package_acceptance.cmake")
        set_tests_properties(pcsx5_package_acceptance PROPERTIES
            TIMEOUT 60 LABELS "frontend;packaging;release")
    endif()
endif()
