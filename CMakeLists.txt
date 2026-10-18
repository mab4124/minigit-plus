cmake_minimum_required(VERSION 3.20)
project(MiniGitPlus VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MINIGIT_BUILD_TESTS "Build the GoogleTest suite" ON)
option(MINIGIT_SANITIZE "Build with AddressSanitizer and UBSan (GCC/Clang)" OFF)

if(MINIGIT_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(ZLIB REQUIRED)

# Core library: everything except main(), so tests can link against it.
add_library(minigit_core
  src/cli.cpp
  src/compression.cpp
  src/hash.cpp
  src/repository.cpp
)
target_include_directories(minigit_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(minigit_core PRIVATE OpenSSL::Crypto ZLIB::ZLIB)
if(MSVC)
  target_compile_options(minigit_core PRIVATE /W4)
else()
  target_compile_options(minigit_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(minigit src/main.cpp)
target_link_libraries(minigit PRIVATE minigit_core)

if(MINIGIT_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(minigit_tests
    tests/cli_tests.cpp
    tests/object_tests.cpp
    tests/repository_tests.cpp
  )
  target_link_libraries(minigit_tests PRIVATE minigit_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(minigit_tests DISCOVERY_TIMEOUT 30)
endif()
