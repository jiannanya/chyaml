cmake_minimum_required(VERSION 3.21)

project(chyaml VERSION 2.0.0 LANGUAGES CXX)

include(CheckIPOSupported)

set(CHYAML_STANDALONE OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(CHYAML_STANDALONE ON)
endif()

option(CHYAML_BUILD_TESTS "Build functional tests" ${CHYAML_STANDALONE})
option(CHYAML_BUILD_BENCHMARKS "Build performance and memory benchmark" ${CHYAML_STANDALONE})
option(CHYAML_BUILD_CONFORMANCE "Build the YAML Test Suite acceptance runner" ${CHYAML_STANDALONE})
option(CHYAML_BUILD_SIZE_PROBE "Build the minimum linked-size probe" ${CHYAML_STANDALONE})
option(CHYAML_BUILD_COMPARISON "Build the optional rapidyaml comparison benchmark" OFF)
option(CHYAML_ENABLE_IPO "Enable interprocedural optimization in release builds" ON)
set(CHYAML_RAPIDYAML_SOURCE_DIR "" CACHE PATH "Path to a rapidyaml source tree for comparison")
set(CHYAML_OPTIMIZE_FOR "BALANCED" CACHE STRING "Optimization profile: SPEED, BALANCED, or SIZE")
set_property(CACHE CHYAML_OPTIMIZE_FOR PROPERTY STRINGS SPEED BALANCED SIZE)

add_library(chyaml STATIC chyaml.cpp)
add_library(chyaml::chyaml ALIAS chyaml)

target_compile_features(chyaml PUBLIC cxx_std_20)
target_include_directories(chyaml
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# std::thread backs the opt-in parallel document path; the flags are empty on
# platforms whose C++ runtime already carries thread support.
find_package(Threads REQUIRED)
target_link_libraries(chyaml PUBLIC Threads::Threads)

set_target_properties(chyaml PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME chyaml)

string(TOUPPER "${CHYAML_OPTIMIZE_FOR}" CHYAML_OPTIMIZE_FOR_UPPER)
if(NOT CHYAML_OPTIMIZE_FOR_UPPER MATCHES "^(SPEED|BALANCED|SIZE)$")
    message(FATAL_ERROR "CHYAML_OPTIMIZE_FOR must be SPEED, BALANCED, or SIZE")
endif()

set(CHYAML_OPTIMIZED_TARGETS chyaml)

foreach(target_name IN LISTS CHYAML_OPTIMIZED_TARGETS)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /Gy /Gw /utf-8)
        if(CHYAML_OPTIMIZE_FOR_UPPER STREQUAL "SIZE")
            target_compile_options(${target_name} PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O1>)
        else()
            target_compile_options(${target_name} PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2>)
        endif()
    else()
        target_compile_options(${target_name} PRIVATE -ffunction-sections -fdata-sections)
        if(CHYAML_OPTIMIZE_FOR_UPPER STREQUAL "SIZE")
            target_compile_options(${target_name} PRIVATE $<$<NOT:$<CONFIG:Debug>>:-Os>)
        elseif(CHYAML_OPTIMIZE_FOR_UPPER STREQUAL "SPEED")
            target_compile_options(${target_name} PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O3>)
        endif()
    endif()
endforeach()

if(CHYAML_ENABLE_IPO)
    check_ipo_supported(RESULT CHYAML_IPO_SUPPORTED OUTPUT CHYAML_IPO_ERROR)
    if(CHYAML_IPO_SUPPORTED)
        foreach(target_name IN LISTS CHYAML_OPTIMIZED_TARGETS)
            set_property(TARGET ${target_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
            set_property(TARGET ${target_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
        endforeach()
    else()
        message(STATUS "IPO unavailable: ${CHYAML_IPO_ERROR}")
    endif()
endif()

function(chyaml_optimize_executable target_name)
    target_link_libraries(${target_name} PRIVATE chyaml::chyaml)
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive- /utf-8)
        target_link_options(${target_name} PRIVATE $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
        if(APPLE)
            target_link_options(${target_name} PRIVATE -Wl,-dead_strip)
        else()
            target_link_options(${target_name} PRIVATE -Wl,--gc-sections)
        endif()
    endif()
    if(CHYAML_ENABLE_IPO AND CHYAML_IPO_SUPPORTED)
        set_property(TARGET ${target_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET ${target_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
    endif()
endfunction()

if(CHYAML_BUILD_TESTS)
    enable_testing()
    add_executable(chyaml_tests tests/test_chyaml.cpp)
    chyaml_optimize_executable(chyaml_tests)
    add_test(NAME chyaml.functional COMMAND chyaml_tests)
endif()

if(CHYAML_BUILD_CONFORMANCE)
    add_executable(chyaml_conformance tests/conformance.cpp)
    chyaml_optimize_executable(chyaml_conformance)
endif()

if(CHYAML_BUILD_BENCHMARKS)
    add_executable(chyaml_benchmark tests/benchmark.cpp)
    chyaml_optimize_executable(chyaml_benchmark)
    if(WIN32)
        target_link_libraries(chyaml_benchmark PRIVATE psapi)
    endif()

    add_executable(chyaml_bench_suite tests/bench_suite.cpp)
    chyaml_optimize_executable(chyaml_bench_suite)
    if(WIN32)
        target_link_libraries(chyaml_bench_suite PRIVATE psapi)
    endif()
endif()

if(CHYAML_BUILD_SIZE_PROBE)
    add_executable(chyaml_size_probe tests/size_probe.cpp)
    chyaml_optimize_executable(chyaml_size_probe)
    add_executable(chyaml_event_size_probe tests/event_size_probe.cpp)
    chyaml_optimize_executable(chyaml_event_size_probe)
    if(CHYAML_BUILD_TESTS)
        add_test(NAME chyaml.size_probe COMMAND chyaml_size_probe)
        add_test(NAME chyaml.event_size_probe COMMAND chyaml_event_size_probe)
    endif()
endif()

if(CHYAML_BUILD_COMPARISON)
    if(NOT IS_DIRECTORY "${CHYAML_RAPIDYAML_SOURCE_DIR}")
        message(FATAL_ERROR "CHYAML_RAPIDYAML_SOURCE_DIR must point to a rapidyaml source tree")
    endif()
    function(chyaml_add_comparison_target)
        set(BUILD_SHARED_LIBS OFF)
        set(RYML_BUILD_TESTS OFF)
        set(RYML_BUILD_BENCHMARKS OFF)
        set(RYML_BUILD_TOOLS OFF)
        set(RYML_INSTALL OFF)
        add_subdirectory(
            "${CHYAML_RAPIDYAML_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/_compare/rapidyaml"
            EXCLUDE_FROM_ALL)
    endfunction()
    chyaml_add_comparison_target()

    add_executable(chyaml_compare tests/compare_chyaml.cpp)
    chyaml_optimize_executable(chyaml_compare)
    if(WIN32)
        target_link_libraries(chyaml_compare PRIVATE psapi)
    endif()

    add_executable(rapidyaml_compare tests/compare_rapidyaml.cpp)
    target_link_libraries(rapidyaml_compare PRIVATE ryml::ryml)
    target_compile_features(rapidyaml_compare PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(rapidyaml_compare PRIVATE /W4 /permissive- /utf-8)
        target_link_options(rapidyaml_compare PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)
    else()
        target_compile_options(rapidyaml_compare PRIVATE -Wall -Wextra -Wpedantic)
        if(APPLE)
            target_link_options(rapidyaml_compare PRIVATE -Wl,-dead_strip)
        else()
            target_link_options(rapidyaml_compare PRIVATE -Wl,--gc-sections)
        endif()
    endif()
    if(WIN32)
        target_link_libraries(rapidyaml_compare PRIVATE psapi)
    endif()

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_custom_target(chyaml_comparison_suite
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_comparison.py"
                --chyaml $<TARGET_FILE:chyaml_compare>
                --rapidyaml $<TARGET_FILE:rapidyaml_compare>
                --runs 7
                --iterations 10
            DEPENDS chyaml_compare rapidyaml_compare
            USES_TERMINAL
            VERBATIM)
    endif()

    add_executable(rapidyaml_size_probe tests/rapidyaml_size_probe.cpp)
    target_link_libraries(rapidyaml_size_probe PRIVATE ryml::ryml)
    target_compile_features(rapidyaml_size_probe PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(rapidyaml_size_probe PRIVATE /W4 /permissive- /utf-8)
        target_link_options(rapidyaml_size_probe PRIVATE
            $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)
    else()
        target_compile_options(rapidyaml_size_probe PRIVATE -Wall -Wextra -Wpedantic)
        if(APPLE)
            target_link_options(rapidyaml_size_probe PRIVATE -Wl,-dead_strip)
        else()
            target_link_options(rapidyaml_size_probe PRIVATE -Wl,--gc-sections)
        endif()
    endif()
    if(CHYAML_ENABLE_IPO AND CHYAML_IPO_SUPPORTED)
        set_property(TARGET rapidyaml_compare PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET rapidyaml_compare PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
        set_property(TARGET rapidyaml_size_probe PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET rapidyaml_size_probe PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
    endif()
endif()
