add_library(pcsx5_runtime INTERFACE)
target_compile_features(pcsx5_runtime INTERFACE cxx_std_23)
target_include_directories(pcsx5_runtime INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(WIN32)
    add_library(pcsx5_runtime_windows STATIC src/windows_memory.cpp src/windows_worker.cpp)
    target_link_libraries(pcsx5_runtime_windows PUBLIC pcsx5_runtime)
    pcsx5_enable_warnings(pcsx5_runtime_windows)
endif()
