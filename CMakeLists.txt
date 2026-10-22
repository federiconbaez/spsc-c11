cmake_minimum_required(VERSION 3.20)
project(spsc_c11 VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(SPSC_SANITIZER "" CACHE STRING "Sanitizer to build with: thread, address, or empty")
set(SPSC_STRESS_ITEMS 2000000 CACHE STRING "Items pushed through the two-thread stress test")

find_package(Threads REQUIRED)

add_library(spsc INTERFACE)
target_include_directories(spsc INTERFACE include)

if(SPSC_SANITIZER STREQUAL "thread")
  add_compile_options(-fsanitize=thread -g)
  add_link_options(-fsanitize=thread)
elseif(SPSC_SANITIZER STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -g)
  add_link_options(-fsanitize=address,undefined)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Werror)

enable_testing()
add_executable(test_spsc tests/test_spsc.c)
target_link_libraries(test_spsc PRIVATE spsc Threads::Threads)
add_test(NAME spsc COMMAND test_spsc ${SPSC_STRESS_ITEMS})

# Negative control: with relaxed handoff the stress test must trip ThreadSanitizer.
if(SPSC_SANITIZER STREQUAL "thread")
  add_executable(test_spsc_relaxed tests/test_spsc.c)
  target_compile_definitions(test_spsc_relaxed PRIVATE SPSC_ACQUIRE=memory_order_relaxed SPSC_RELEASE=memory_order_relaxed)
  target_link_libraries(test_spsc_relaxed PRIVATE spsc Threads::Threads)
  add_test(NAME tsan_catches_relaxed_ordering COMMAND test_spsc_relaxed 200000)
  set_tests_properties(tsan_catches_relaxed_ordering PROPERTIES
    ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1"
    PASS_REGULAR_EXPRESSION "WARNING: ThreadSanitizer: data race")
endif()

add_executable(bench_spsc bench/bench.c)
target_link_libraries(bench_spsc PRIVATE spsc Threads::Threads)
