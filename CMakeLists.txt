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

