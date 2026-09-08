set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include outside.h")
add_library(fixture_core STATIC "${core}/src/value.cpp")
target_include_directories(fixture_core PUBLIC "${core}/include")
