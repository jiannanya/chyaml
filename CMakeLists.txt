cmake_minimum_required(VERSION 3.21)

project(chyaml VERSION 2.0.0 LANGUAGES C CXX)

include(FetchContent)
include(CheckIPOSupported)

option(CHYAML_BUILD_TESTS "Build functional tests" ON)
option(CHYAML_BUILD_BENCHMARKS "Build performance and memory benchmark" ON)
option(CHYAML_BUILD_CONFORMANCE "Build the YAML Test Suite acceptance runner" ON)
option(CHYAML_BUILD_SIZE_PROBE "Build the minimum linked-size probe" ON)
option(CHYAML_USE_SYSTEM_LIBFYAML "Use an installed libfyaml package" OFF)
option(CHYAML_ENABLE_IPO "Enable interprocedural optimization in release builds" ON)
set(CHYAML_OPTIMIZE_FOR "BALANCED" CACHE STRING "Optimization profile: SPEED, BALANCED, or SIZE")
set_property(CACHE CHYAML_OPTIMIZE_FOR PROPERTY STRINGS SPEED BALANCED SIZE)

if(CHYAML_USE_SYSTEM_LIBFYAML)
    find_package(libfyaml 0.9.6 CONFIG REQUIRED)
else()
    function(chyaml_add_bundled_core)
        set(BUILD_SHARED_LIBS OFF)
        set(BUILD_TESTING OFF)
        set(ENABLE_NETWORK OFF)
        set(ENABLE_LIBCLANG OFF)

        FetchContent_Declare(
            libfyaml
            GIT_REPOSITORY https://github.com/pantoniou/libfyaml.git
            GIT_TAG 1ed9bae3b1e5fc57e31a14a2ea12810fdf423473
            GIT_SHALLOW TRUE
            GIT_PROGRESS TRUE
        )
        FetchContent_GetProperties(libfyaml)
        if(NOT libfyaml_POPULATED)
            if(POLICY CMP0169)
                cmake_policy(SET CMP0169 OLD)
            endif()
            FetchContent_Populate(libfyaml)
            add_subdirectory(
                "${libfyaml_SOURCE_DIR}"
                "${libfyaml_BINARY_DIR}"
                EXCLUDE_FROM_ALL)
        endif()
    endfunction()
    chyaml_add_bundled_core()

    foreach(unused_target fyaml_static fy-tool fy-thread fy-b3sum fy-allocators)
        if(TARGET ${unused_target})
            set_target_properties(${unused_target} PROPERTIES
                EXCLUDE_FROM_ALL TRUE
                EXCLUDE_FROM_DEFAULT_BUILD TRUE)
        endif()
    endforeach()
endif()

add_library(chyaml STATIC chyaml.cpp)
add_library(chyaml::chyaml ALIAS chyaml)

target_compile_features(chyaml PUBLIC cxx_std_20)
target_include_directories(chyaml
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(chyaml PRIVATE libfyaml::libfyaml)

set_target_properties(chyaml PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME chyaml)

string(TOUPPER "${CHYAML_OPTIMIZE_FOR}" CHYAML_OPTIMIZE_FOR_UPPER)
if(NOT CHYAML_OPTIMIZE_FOR_UPPER MATCHES "^(SPEED|BALANCED|SIZE)$")
    message(FATAL_ERROR "CHYAML_OPTIMIZE_FOR must be SPEED, BALANCED, or SIZE")
endif()

set(CHYAML_OPTIMIZED_TARGETS chyaml)
if(TARGET fyaml)
    list(APPEND CHYAML_OPTIMIZED_TARGETS fyaml)
endif()

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
        target_link_options(${target_name} PRIVATE -Wl,--gc-sections)
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
endif()

if(CHYAML_BUILD_SIZE_PROBE)
    add_executable(chyaml_size_probe tests/size_probe.cpp)
    chyaml_optimize_executable(chyaml_size_probe)
endif()
