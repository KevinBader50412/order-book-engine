cmake_minimum_required(VERSION 3.16)
project(order_book_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OB_TSAN "Build with ThreadSanitizer (for the concurrency tests)" OFF)
option(OB_BUILD_TESTS "Build the GoogleTest suite" ON)

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-O1 -g -fno-omit-frame-pointer")

if(OB_TSAN)
  add_compile_options(-fsanitize=thread -g)
  add_link_options(-fsanitize=thread)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

# Core library: everything except the CLI and the benchmark.
add_library(orderbook
  src/OrderBook.cpp
  src/MatchingEngine.cpp
  src/CommandParser.cpp)
target_include_directories(orderbook PUBLIC include)
target_compile_options(orderbook PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(orderbook PUBLIC Threads::Threads)

add_executable(engine src/main.cpp)
target_compile_options(engine PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(engine PRIVATE orderbook)

add_executable(bench bench/bench.cpp)
target_compile_options(bench PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(bench PRIVATE orderbook)

if(OB_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  include(GoogleTest)

  add_executable(unit_tests
    tests/test_orderbook.cpp
    tests/test_parser.cpp
    tests/test_randomized.cpp
    tests/test_concurrency.cpp)
  target_compile_options(unit_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(unit_tests PRIVATE orderbook GTest::gtest_main)
  gtest_discover_tests(unit_tests DISCOVERY_TIMEOUT 60)
endif()
