cmake_minimum_required(VERSION 3.25)

# Dependency-free policy check for the eight clean-build presets. CMake itself
# validates preset schema/expansion; this verifies project-specific invariants.
file(READ "${CMAKE_CURRENT_LIST_DIR}/../CMakePresets.json" presets)

function(expect expected)
    string(JSON actual GET "${presets}" ${ARGN})
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "Preset field [${ARGN}]: expected '${expected}', got '${actual}'")
    endif()
endfunction()

function(expect_missing)
    string(JSON ignored ERROR_VARIABLE error GET "${presets}" ${ARGN})
    if(NOT error MATCHES "not found")
        message(FATAL_ERROR "Preset field [${ARGN}] must be absent")
    endif()
endfunction()

expect(6 version)
expect(base configurePresets 0 name)
expect(ON configurePresets 0 hidden)
expect(Ninja configurePresets 0 generator)
expect("\${sourceDir}/out/build/\${presetName}" configurePresets 0 binaryDir)
expect(ON configurePresets 0 cacheVariables BUILD_TESTING)

foreach(section configurePresets buildPresets testPresets)
    string(JSON count LENGTH "${presets}" ${section})
    if(section STREQUAL "configurePresets")
        set(expected_count 9)
    else()
        set(expected_count 8)
    endif()
    if(NOT count EQUAL expected_count)
        message(FATAL_ERROR "Unexpected ${section} count: ${count}")
    endif()
endforeach()
# Graphics opt-in presets inherit each host's toolchain, but keep isolated output.
set(index 4)
foreach(platform windows linux)
    foreach(config debug release)
        set(name "${platform}-x64-graphics-${config}")
        math(EXPR configure_index "${index} + 1")
        expect("${name}" configurePresets ${configure_index} name)
        expect("${platform}-x64-${config}" configurePresets ${configure_index} inherits)
        expect(ON configurePresets ${configure_index} cacheVariables PCSX5_BUILD_VULKAN)
        foreach(section buildPresets testPresets)
            expect("${name}" ${section} ${index} name)
            expect("${name}" ${section} ${index} configurePreset)
        endforeach()
        expect(error testPresets ${index} execution noTestsAction)
        expect(30 testPresets ${index} execution timeout)
        expect(ON testPresets ${index} output outputOnFailure)
        expect("\${sourceDir}/out/build/\${presetName}/ctest.log" testPresets ${index} output outputLogFile)
        expect("\${sourceDir}/out/build/\${presetName}/junit.xml" testPresets ${index} output outputJUnitFile)
        math(EXPR index "${index} + 1")
    endforeach()
endforeach()

set(index 0)
foreach(platform windows linux)
    if(platform STREQUAL "windows")
        set(system Windows)
    else()
        set(system Linux)
    endif()
    foreach(config Debug Release)
        string(TOLOWER "${config}" lower_config)
        set(name "${platform}-x64-${lower_config}")
        math(EXPR configure_index "${index} + 1")
        expect("${name}" configurePresets ${configure_index} name)
        expect(base configurePresets ${configure_index} inherits)
        expect(equals configurePresets ${configure_index} condition type)
        expect("\${hostSystemName}" configurePresets ${configure_index} condition lhs)
        expect("${system}" configurePresets ${configure_index} condition rhs)
        expect("${config}" configurePresets ${configure_index} cacheVariables CMAKE_BUILD_TYPE)
        # Let each activated host environment select its compiler. A short name
        # such as "cl" becomes a different cache value after CMake resolves the
        # executable and can force a configure loop on newer CMake versions.
        expect_missing(configurePresets ${configure_index} cacheVariables CMAKE_CXX_COMPILER)
        if(platform STREQUAL "windows")
            expect(x64 configurePresets ${configure_index} architecture value)
            expect(external configurePresets ${configure_index} architecture strategy)
        endif()
        foreach(section buildPresets testPresets)
            expect("${name}" ${section} ${index} name)
            expect("${name}" ${section} ${index} configurePreset)
        endforeach()
        expect(error testPresets ${index} execution noTestsAction)
        expect(30 testPresets ${index} execution timeout)
        expect(ON testPresets ${index} output outputOnFailure)
        expect("\${sourceDir}/out/build/\${presetName}/ctest.log" testPresets ${index} output outputLogFile)
        expect("\${sourceDir}/out/build/\${presetName}/junit.xml" testPresets ${index} output outputJUnitFile)
        math(EXPR index "${index} + 1")
    endforeach()
endforeach()
# A standalone -P process does not inherit the root project's policy version.
# CMake 4 can mask missing declarations that still fail on supported CMake 3.
foreach(script IN ITEMS
        ../cmake/CheckCoreIncludes.cmake
        ../cmake/EmbedFrameShaders.cmake
        boundary/run_case.cmake
        guest_range_repeatability.cmake
        guest_memory_repeatability.cmake
        graphics_repeatability.cmake
        package_acceptance.cmake
        runtime_memory_termination.cmake
        runtime_worker_termination.cmake)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/${script}" source)
    if(NOT source MATCHES "^cmake_minimum_required\\(VERSION 3\\.25\\)")
        message(FATAL_ERROR "Standalone script lacks the 3.25 policy baseline: ${script}")
    endif()
endforeach()
message(STATUS "Eight isolated presets and standalone policy baselines match the clean-build contract")
