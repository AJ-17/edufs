cmake_minimum_required(VERSION 3.16)
project(EduFS VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(EDUFS_SANITIZE "Build with AddressSanitizer + UBSan" OFF)

# 1. The library: all filesystem code lives here, so the CLI and the tests share it.
add_library(edufs_core STATIC
  src/version.cpp
  src/errors.cpp
  src/block_device.cpp
  src/superblock.cpp
  src/fs_core.cpp
  src/inspect.cpp
)
target_include_directories(edufs_core PUBLIC include)
target_compile_options(edufs_core PUBLIC -Wall -Wextra -Wpedantic)
if(EDUFS_SANITIZE)
  target_compile_options(edufs_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(edufs_core PUBLIC -fsanitize=address,undefined)
endif()

# 2. The program users run.
add_executable(edufs src/main.cpp)
target_link_libraries(edufs PRIVATE edufs_core)

# 3. The test program: every tests/*.cpp file is picked up automatically.
file(GLOB EDUFS_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
add_executable(edufs_tests ${EDUFS_TEST_SOURCES})
target_include_directories(edufs_tests PRIVATE third_party)
target_link_libraries(edufs_tests PRIVATE edufs_core)

enable_testing()
add_test(NAME edufs_tests COMMAND edufs_tests)