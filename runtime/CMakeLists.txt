add_library(pcsx5_runtime INTERFACE)
target_compile_features(pcsx5_runtime INTERFACE cxx_std_23)
target_include_directories(pcsx5_runtime INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(pcsx5_runtime_core_bridge STATIC src/guest_memory_backing.cpp)
target_link_libraries(pcsx5_runtime_core_bridge PUBLIC pcsx5_runtime pcsx5_core)
pcsx5_enable_warnings(pcsx5_runtime_core_bridge)

if(WIN32)
    add_library(pcsx5_runtime_windows STATIC src/windows_memory.cpp src/windows_worker.cpp
        src/windows_timing.cpp src/windows_child_process.cpp src/windows_executable_code.cpp)
    target_link_libraries(pcsx5_runtime_windows PUBLIC pcsx5_runtime)
    pcsx5_enable_warnings(pcsx5_runtime_windows)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    add_library(pcsx5_runtime_linux STATIC src/linux_memory.cpp src/linux_worker.cpp src/linux_timing.cpp src/linux_child_process.cpp src/posix_executable_code.cpp)
    target_link_libraries(pcsx5_runtime_linux PUBLIC pcsx5_runtime PRIVATE Threads::Threads)
    pcsx5_enable_warnings(pcsx5_runtime_linux)
endif()
