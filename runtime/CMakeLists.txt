add_library(pcsx5_runtime INTERFACE)
target_compile_features(pcsx5_runtime INTERFACE cxx_std_23)
target_include_directories(pcsx5_runtime INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
