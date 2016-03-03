#------------------------------------------------------------------------------#
# CMakeLists.txt
# Rémi Attab (remi.attab@gmail.com), 03 Mar 2016
# FreeBSD-style copyright and disclaimer apply
#------------------------------------------------------------------------------#



cmake_minimum_required(VERSION 3.0)
project(optics)

enable_testing()

#------------------------------------------------------------------------------#
# BASICS
#------------------------------------------------------------------------------#

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_definitions("-pipe -g -O3 -Werror -march=native")
add_definitions("-Wall")
add_definitions("-Wextra")
add_definitions("-Wundef")
add_definitions("-Wcast-align")
add_definitions("-Wwrite-strings")
add_definitions("-Wunreachable-code")
add_definitions("-Wformat=2")
add_definitions("-Wswitch-enum")
add_definitions("-Wswitch-default")
add_definitions("-Winit-self")
add_definitions("-Wno-strict-aliasing")

# clang rejects standard compliant `{0}` struct initializers. It's a known bug.
add_definitions("-Wno-missing-field-initializers")

# Our log utils needs the format to be passed through a function and I really
# don't see the issue with that so...
add_definitions("-Wno-format-nonliteral")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -std=gnu11 -pthread -D_GNU_SOURCE")


#------------------------------------------------------------------------------#
# DEPS
#------------------------------------------------------------------------------#

include(FindPkgConfig)

# Not installed on prod machine and is therefore optional
pkg_check_modules(CMOCKA cmocka)
if(CMOCKA_FOUND)
    message("CMOCKA: ON")
else()
    message("CMOCKA: OFF")
endif()


#------------------------------------------------------------------------------#
# LIBS
#------------------------------------------------------------------------------#

include_directories("${PROJECT_SOURCE_DIR}/src")

add_library(optics SHARED
    src/optics.c
    src/poller.c
    src/utils/utils.c
    src/backend_stdout.c
    src/backend_carbon.c)
target_link_libraries(optics rt)

add_library(optics_tests SHARED
    test/bench.c
    test/test.c)
target_link_libraries(optics_tests optics ${CMOCKA_LIBRARIES})


#------------------------------------------------------------------------------#
# BINS
#------------------------------------------------------------------------------#

function(optics_exec name)
    add_executable(${name} src/${name}.c)
    target_link_libraries(${name} optics)
endfunction()

# optics_exec(optics)


#------------------------------------------------------------------------------#
# TESTS
#------------------------------------------------------------------------------#

find_program(VALGRIND_EXEC "valgrind")
if(VALGRIND_EXEC)
    message("VALGRIND: ${VALGRIND_EXEC}")
    set(VALGRIND_ARGS "--track-origins=yes")
else()
    message("VALGRIND: DISABLED - install valgrind to enable")
endif()

function(optics_cmocka_test name)
    if(CMOCKA_FOUND)
        add_executable(${name}_test test/${name}_test.c)
        target_link_libraries(${name}_test optics_tests)
        set_property(TARGET ${name}_test APPEND PROPERTY COMPILE_OPTIONS ${CMOCKA_CFLAGS})

        add_test(${name}_test bin/${name}_test)
        set_tests_properties(${name}_test PROPERTIES LABELS "test")

        # \todo valgrind is SUPER slow when we use large shm regions so it's
        # currently impractical. Should be re-enabled once we have resizing
        # regions.
        #
        # if(VALGRIND_EXEC)
        #     add_test(${name}_valgrind ${VALGRIND_EXEC} ${VALGRIND_ARGS} bin/${name}_test)
        #     set_tests_properties(${name}_valgrind PROPERTIES LABELS "valgrind")
        # endif()
    endif()
endfunction()

optics_cmocka_test(region)
optics_cmocka_test(lens)
optics_cmocka_test(lens_gauge)
optics_cmocka_test(lens_counter)
optics_cmocka_test(lens_dist)


#------------------------------------------------------------------------------#
# BENCHS
#------------------------------------------------------------------------------#

function(optics_cmocka_bench name)
    if(CMOCKA_FOUND)
        add_executable(${name}_bench test/${name}_bench.c)
        target_link_libraries(${name}_bench optics_tests)
        set_property(TARGET ${name}_bench APPEND PROPERTY COMPILE_OPTIONS ${CMOCKA_CFLAGS})

        add_test(${name}_bench bin/${name}_bench)
        set_tests_properties(${name}_bench PROPERTIES LABELS "bench")
    endif()
endfunction()

optics_cmocka_bench(bench)
optics_cmocka_bench(lens)
optics_cmocka_bench(lens_gauge)
optics_cmocka_bench(lens_counter)
optics_cmocka_bench(lens_dist)


#------------------------------------------------------------------------------#
# UBSAN
#------------------------------------------------------------------------------#

option(ENABLE_UBSAN
    "enable undefined behaviour sanitizer (affects performance)" OFF)
if(ENABLE_UBSAN)
    message("UBSAN: ENABLED")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=undefined")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=undefined")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=undefined")
else()
    message("UBSAN: DISABLED")
endif()


#------------------------------------------------------------------------------#
# CLANG-TIDY
#------------------------------------------------------------------------------#

find_program(CLANG_TIDY_EXEC "clang-tidy")
if(CLANG_TIDY_EXEC)
    message("CLANG-TIDY: ${CLANG_TIDY_EXEC}")
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    add_custom_target(clang-tidy ALL
        ${CLANG_TIDY_EXEC} -header-filter='.*' src/*.c src/utils/*.c test/*.c)
else()
    message("CLANG-TIDY: DISABLED - install clang to enable")
endif()