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

# Header-only libraries for the history API (`minigit serve`). cpp-httplib is
# fetched with git because it publishes no release archives with a stable hash.
include(FetchContent)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)  # plain HTTP on localhost only
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.18.7
  GIT_SHALLOW TRUE
)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(httplib nlohmann_json)

# Core library: everything except main(), so tests can link against it.
add_library(minigit_core
  src/add.cpp
  src/branch.cpp
  src/checkout.cpp
  src/cli.cpp
  src/commit.cpp
  src/compression.cpp
  src/config.cpp
  src/create_commit.cpp
  src/file_util.cpp
  src/hash.cpp
  src/history_api.cpp
  src/log.cpp
  src/object_store.cpp
  src/refs.cpp
  src/repository.cpp
  src/staging_index.cpp
  src/status.cpp
  src/tree.cpp
)
target_include_directories(minigit_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(minigit_core PRIVATE OpenSSL::Crypto ZLIB::ZLIB httplib::httplib
                                           nlohmann_json::nlohmann_json)
if(MSVC)
  target_compile_options(minigit_core PRIVATE /W4)
else()
  target_compile_options(minigit_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(minigit src/main.cpp)
target_link_libraries(minigit PRIVATE minigit_core)

if(MINIGIT_BUILD_TESTS)
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
    tests/api_tests.cpp
    tests/branch_tests.cpp
    tests/checkout_tests.cpp
    tests/cli_tests.cpp
    tests/command_tests.cpp
    tests/commit_tests.cpp
    tests/config_tests.cpp
    tests/log_tests.cpp
    tests/object_tests.cpp
    tests/refs_tests.cpp
    tests/repository_tests.cpp
    tests/staging_tests.cpp
    tests/status_tests.cpp
    tests/tree_tests.cpp
  )
  # The API tests talk to the server with cpp-httplib's client and parse JSON.
  target_link_libraries(minigit_tests PRIVATE minigit_core GTest::gtest_main httplib::httplib
                                              nlohmann_json::nlohmann_json)
  # Some tests run the real executable to prove state survives a restart.
  target_compile_definitions(minigit_tests PRIVATE MINIGIT_EXE="$<TARGET_FILE:minigit>")
  add_dependencies(minigit_tests minigit)
  include(GoogleTest)
  gtest_discover_tests(minigit_tests DISCOVERY_TIMEOUT 30)
endif()
