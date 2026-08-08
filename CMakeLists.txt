cmake_minimum_required(VERSION 3.16)
project(hft_orderbook CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

include_directories(include)

add_executable(orderbook_bench src/main.cpp)
target_link_libraries(orderbook_bench PRIVATE Threads::Threads)

enable_testing()
add_executable(orderbook_tests tests/test_orderbook.cpp)
target_link_libraries(orderbook_tests PRIVATE Threads::Threads)
add_test(NAME orderbook_tests COMMAND orderbook_tests)
