cmake_minimum_required(VERSION 3.24)

project(AstraBot VERSION 0.1.0 LANGUAGES CXX)

option(ASTRA_BUILD_TESTS "Build deterministic core tests" ON)
option(ASTRA_BUILD_BWAPI_MODULE "Build the 32-bit BWAPI tournament DLL" OFF)
set(BWAPI_ROOT "" CACHE PATH "Path to an unpacked BWAPI 4.4.0 release")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AstraWarnings)

add_library(astra_core STATIC
    src/core/GameState.cpp
)
add_library(Astra::Core ALIAS astra_core)

target_include_directories(astra_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(astra_core PUBLIC cxx_std_20)
astra_enable_warnings(astra_core)

if(ASTRA_BUILD_BWAPI_MODULE)
    if(NOT WIN32)
        message(FATAL_ERROR "The injected BWAPI module must be built for 32-bit Windows")
    endif()
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "BWAPI modules must be configured with a 32-bit generator (-A Win32)")
    endif()
    if(NOT EXISTS "${BWAPI_ROOT}/include/BWAPI.h")
        message(FATAL_ERROR "Set BWAPI_ROOT to an unpacked BWAPI 4.4.0 release")
    endif()

    find_library(BWAPI_LIBRARY
        NAMES BWAPILIB BWAPI
        PATHS "${BWAPI_ROOT}/lib" "${BWAPI_ROOT}/lib/x86"
        NO_DEFAULT_PATH
        REQUIRED
    )

    add_library(astra_bwapi MODULE
        src/bwapi/AstraModule.cpp
        src/bwapi/BwapiBridge.cpp
    )
    target_include_directories(astra_bwapi PRIVATE "${BWAPI_ROOT}/include")
    target_link_libraries(astra_bwapi PRIVATE Astra::Core "${BWAPI_LIBRARY}")
    target_compile_features(astra_bwapi PRIVATE cxx_std_20)
    target_compile_definitions(astra_bwapi PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    astra_enable_warnings(astra_bwapi)
    set_target_properties(astra_bwapi PROPERTIES
        OUTPUT_NAME "AstraBot"
        PREFIX ""
        SUFFIX ".dll"
    )
endif()

if(ASTRA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

