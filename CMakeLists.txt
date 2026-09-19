cmake_minimum_required(VERSION 3.24)

project(Protodd VERSION 0.1.0 LANGUAGES CXX)

option(PROTODD_BUILD_TESTS "Build deterministic core tests" ON)
option(PROTODD_BUILD_BWAPI_MODULE "Build the 32-bit BWAPI tournament DLL" OFF)
set(BWAPI_ROOT "" CACHE PATH "Path to an unpacked BWAPI 4.4.0 release")
set(BWAPI_LIBRARY "" CACHE FILEPATH "Path to a 32-bit BWAPILIB.lib")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProtoddWarnings)

add_library(protodd_core OBJECT
    src/core/GameState.cpp
    src/core/UnitCatalog.cpp
    src/core/Technology.cpp
    src/core/Information.cpp
    src/core/InfluenceMap.cpp
    src/core/Navigation.cpp
    src/core/Strategy.cpp
    src/core/MacroPlanner.cpp
    src/core/Combat.cpp
    src/core/CommandBus.cpp
    src/core/Workers.cpp
    src/core/Scouting.cpp
    src/core/Learning.cpp
    src/core/PolicyLearning.cpp
    src/core/Runtime.cpp
    src/core/Squads.cpp
    src/core/Transport.cpp
    src/core/Operations.cpp
    src/core/Harassment.cpp
)
add_library(Protodd::Core ALIAS protodd_core)

target_include_directories(protodd_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(protodd_core PUBLIC cxx_std_20)
protodd_enable_warnings(protodd_core)

if(PROTODD_BUILD_BWAPI_MODULE)
    if(NOT WIN32)
        message(FATAL_ERROR "The injected BWAPI module must be built for 32-bit Windows")
    endif()
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "BWAPI modules must be configured with a 32-bit generator (-A Win32)")
    endif()
    if(EXISTS "${BWAPI_ROOT}/include/BWAPI.h")
        set(BWAPI_INCLUDE_DIR "${BWAPI_ROOT}/include")
    elseif(EXISTS "${BWAPI_ROOT}/bwapi/include/BWAPI.h")
        set(BWAPI_INCLUDE_DIR "${BWAPI_ROOT}/bwapi/include")
    else()
        message(FATAL_ERROR "Set BWAPI_ROOT to an unpacked BWAPI 4.4.0 release")
    endif()

    if(NOT EXISTS "${BWAPI_LIBRARY}")
        find_library(BWAPI_LIBRARY_DISCOVERED
            NAMES BWAPILIB BWAPI
            PATHS
                "${BWAPI_ROOT}/lib"
                "${BWAPI_ROOT}/lib/x86"
                "${BWAPI_ROOT}/bwapi/Release"
                "${BWAPI_ROOT}/bwapi/BWAPILIB/Release"
            NO_DEFAULT_PATH
        )
        if(NOT BWAPI_LIBRARY_DISCOVERED)
            message(FATAL_ERROR
                "BWAPILIB.lib was not found. Build the official BWAPILIB project "
                "for Release|Win32, then set BWAPI_LIBRARY to its output path.")
        endif()
        set(BWAPI_LIBRARY "${BWAPI_LIBRARY_DISCOVERED}")
    endif()

    add_library(protodd_bwapi MODULE
        src/bwapi/ProtoddModule.cpp
        src/bwapi/BwapiBridge.cpp
        src/bwapi/Entry.cpp
        src/bwapi/PolicyRuntime.cpp
    )
    target_include_directories(protodd_bwapi PRIVATE
        "${BWAPI_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bwapi"
    )
    target_link_libraries(protodd_bwapi PRIVATE Protodd::Core "${BWAPI_LIBRARY}")
    target_compile_features(protodd_bwapi PRIVATE cxx_std_20)
    target_compile_definitions(protodd_bwapi PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    protodd_enable_warnings(protodd_bwapi)
    set_target_properties(protodd_bwapi PROPERTIES
        OUTPUT_NAME "Protodd"
        PREFIX ""
        SUFFIX ".dll"
    )
    foreach(race IN ITEMS Terran Zerg)
        if(race STREQUAL "Terran")
            set(race_id 1)
        else()
            set(race_id 2)
        endif()
        add_library(${race}Todd MODULE src/bwapi/RaceBotModule.cpp
            src/bwapi/RaceBotEntry.cpp src/bwapi/PolicyRuntime.cpp)
        target_include_directories(${race}Todd PRIVATE "${BWAPI_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src/bwapi")
        target_link_libraries(${race}Todd PRIVATE Protodd::Core "${BWAPI_LIBRARY}")
        target_compile_features(${race}Todd PRIVATE cxx_std_20)
        target_compile_definitions(${race}Todd PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX PROTODD_BOT_RACE=${race_id})
        protodd_enable_warnings(${race}Todd)
        set_target_properties(${race}Todd PROPERTIES PREFIX "" SUFFIX ".dll")
    endforeach()
endif()

add_executable(policy_trainer tools/policy_trainer.cpp)
target_link_libraries(policy_trainer PRIVATE Protodd::Core)
target_compile_features(policy_trainer PRIVATE cxx_std_20)

if(PROTODD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    if(PROTODD_BUILD_BWAPI_MODULE)
        add_executable(protodd_bwapi_catalog_tests tests/bwapi_catalog.cpp)
        target_include_directories(protodd_bwapi_catalog_tests PRIVATE "${BWAPI_INCLUDE_DIR}")
        target_link_libraries(protodd_bwapi_catalog_tests PRIVATE Protodd::Core "${BWAPI_LIBRARY}")
        target_compile_features(protodd_bwapi_catalog_tests PRIVATE cxx_std_20)
        protodd_enable_warnings(protodd_bwapi_catalog_tests)
        add_test(NAME protodd_bwapi_catalog_tests COMMAND protodd_bwapi_catalog_tests)
    endif()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME protodd_policy_dataset_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_train_policy.py")
        add_test(NAME protodd_training_validation_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_train_openings.py")
        add_test(NAME protodd_policy_pipeline_test
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_policy_pipeline.py"
                "$<TARGET_FILE:policy_trainer>")
        add_test(NAME protodd_decision_report_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/decision_report.py" --self-test)
        add_test(
            NAME protodd_log_analyzer_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/log_analyzer.py" --self-test
        )
        add_test(
            NAME protodd_direct_report_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/direct_report.py" --self-test
        )
    endif()
endif()
