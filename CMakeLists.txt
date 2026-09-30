cmake_minimum_required(VERSION 3.24)

project(Protodd VERSION 0.1.0 LANGUAGES CXX)

option(PROTODD_BUILD_TESTS "Build deterministic core tests" ON)
option(PROTODD_BUILD_BWAPI_MODULE "Build the 32-bit BWAPI tournament DLL" OFF)
option(PROTODD_PRODUCTION_LOCAL_EVALUATION "Enable opt-in early train-unit control in isolated local evaluations" OFF)
option(PROTODD_TACTICAL_LOCAL_EVALUATION "Enable opt-in replay-trained target selection in isolated local evaluations" OFF)
option(PROTODD_PVZ_GATEWAY_OPENING "Evaluate a gateway-first PvZ opening in isolated local games" OFF)
option(PROTODD_PVZ_EARLY_SPLASH "Evaluate an early evidence-triggered PvZ Reaver transition in isolated local games" OFF)
option(PROTODD_PVP_FOG_DETECTION "Evaluate an early Observer against a poorly scouted PvP two-Gateway opening" OFF)
option(PROTODD_PVP_SCOUTED_TWO_GATE_ANCHOR "Evaluate an earlier Forge after scouting two PvP Gateways" OFF)
option(PROTODD_THREATENED_NATURAL_RALLY "Evaluate reinforcement rally for a threatened outer base" OFF)
option(PROTODD_COVERED_PRESSURE_RELEASE "Evaluate releasing a large covered PvT army from an empty policy defend override" OFF)
option(PROTODD_PVT_CONTAIN_BREAK "Evaluate detector-covered PvT breakout against a reachable enemy economy" OFF)
option(PROTODD_FORWARD_DETECTOR_ESCORT "Evaluate centering blocked main-army Observers for immediate detection coverage" OFF)
option(PROTODD_DIRECT_DETECTOR_RENDEZVOUS "Evaluate direct Observer rendezvous through a safe air corridor" OFF)
option(PROTODD_CONTESTED_DETECTOR_RESERVE "Evaluate using the scout Observer when multiple main squads need detection" OFF)
option(PROTODD_FORWARD_THIRD_SCREEN "Evaluate escorting a forward PvT third Nexus with the field army" OFF)
option(PROTODD_BASE_THREAT_EXPANSION_GUARD "Evaluate keeping the field army near a threatened surviving base" OFF)
option(PROTODD_ABANDONED_BASE_WORKER_EVACUATION "Evaluate evacuating workers from a threatened lost base" OFF)
option(PROTODD_SAFE_REMOTE_MINING "Evaluate bounded mining at a planned neutral site after owned minerals exhaust" OFF)
option(PROTODD_BASE_DEFENSE_CONSOLIDATION "Evaluate committing the field army to a heavily threatened economy" OFF)
option(PROTODD_STATIC_DEFENSE_COVERAGE "Evaluate limiting static-defense allocation credit to covered attackers" OFF)
option(PROTODD_DETECTOR_WAIT_VOLLEY "Evaluate stationary legal shots while the main army waits for detection" OFF)
option(PROTODD_PVZ_REPLAY_OPENING "Evaluate a replay-derived economic PvZ opening" OFF)
option(PROTODD_PVZ_ARCHIVES_FIRST "Evaluate replay-derived PvZ Archives before optional Reaver drops" OFF)
option(PROTODD_LATE_ECONOMY_RECOVERY "Evaluate protected late worker and expansion recovery" OFF)
option(PROTODD_STALLED_ARMY_ROUTING "Evaluate terrain waypoints for a stalled attacking army" OFF)
option(PROTODD_PVZ_MINERAL_FALLBACK "Evaluate Zealot spending from otherwise idle midgame Gateways" OFF)
option(PROTODD_PVZ_EARLY_MINERAL_FALLBACK "Evaluate the mineral fallback before the midgame Hydra attack" OFF)
option(PROTODD_PVZ_POWERED_CANNON_SCREEN "Evaluate powered natural Cannons after the first two complete" OFF)
option(PROTODD_PVZ_PROACTIVE_REAVER "Evaluate first Reaver behind the completed natural Cannon screen" OFF)
option(PROTODD_PVZ_ARMY_FLOOR "Evaluate a two-base PvZ ground army floor before further expansion" OFF)
option(PROTODD_PVZ_DETECTOR_SURGE "Evaluate using the scout Observer as an emergency Lurker escort" OFF)
set(BWAPI_ROOT "" CACHE PATH "Path to an unpacked BWAPI 4.4.0 release")
set(BWAPI_LIBRARY "" CACHE FILEPATH "Path to a 32-bit BWAPILIB.lib")
set(PROTODD_WHOLE_GAME_WEIGHTS "" CACHE FILEPATH "Whole-game model weights to embed in Protodd.dll")
option(PROTODD_WHOLE_GAME_CONTROL "Allow the embedded whole-game policy to issue BWAPI commands" OFF)
option(PROTODD_WHOLE_GAME_EVALUATION_BUILD
    "Build a six-slot controller for local evaluation before it has a promotion receipt" OFF)
set(PROTODD_WHOLE_GAME_PROMOTION_RECEIPT "" CACHE FILEPATH
    "Promotion receipt binding a six-slot tournament policy to its exact weights")

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
    src/core/TacticalTargetModel.cpp
    src/core/CommandBus.cpp
    src/core/Workers.cpp
    src/core/Scouting.cpp
    src/core/Learning.cpp
    src/core/PolicyLearning.cpp
    src/core/ObservationEncoder.cpp
    src/core/LearnedPolicy.cpp
    src/core/ProductionDemandModel.cpp
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
    if((PROTODD_PVZ_GATEWAY_OPENING AND PROTODD_PVZ_EARLY_SPLASH) OR
       (PROTODD_PVZ_GATEWAY_OPENING AND PROTODD_PVZ_REPLAY_OPENING) OR
       (PROTODD_PVZ_EARLY_SPLASH AND PROTODD_PVZ_REPLAY_OPENING) OR
       (PROTODD_PVZ_GATEWAY_OPENING AND PROTODD_PVZ_ARCHIVES_FIRST) OR
       (PROTODD_PVZ_EARLY_SPLASH AND PROTODD_PVZ_ARCHIVES_FIRST))
        message(FATAL_ERROR "Unsupported combination of PvZ strategy interventions")
    endif()
    if(PROTODD_PVP_FOG_DETECTION AND
       (PROTODD_PVZ_GATEWAY_OPENING OR PROTODD_PVZ_EARLY_SPLASH OR
        PROTODD_PVZ_REPLAY_OPENING OR PROTODD_PVZ_ARCHIVES_FIRST))
        message(FATAL_ERROR "Evaluate one opt-in strategy intervention at a time")
    endif()
    if(PROTODD_LATE_ECONOMY_RECOVERY AND
       (PROTODD_PVZ_GATEWAY_OPENING OR PROTODD_PVZ_EARLY_SPLASH OR
        PROTODD_PVP_FOG_DETECTION))
        message(FATAL_ERROR "Unsupported combination with late economy recovery")
    endif()
    if(PROTODD_PVZ_POWERED_CANNON_SCREEN AND
       (NOT PROTODD_PVZ_REPLAY_OPENING OR PROTODD_PVZ_ARCHIVES_FIRST OR
        PROTODD_LATE_ECONOMY_RECOVERY OR PROTODD_PVZ_GATEWAY_OPENING OR
        PROTODD_PVZ_EARLY_SPLASH OR PROTODD_PVP_FOG_DETECTION OR
        PROTODD_STALLED_ARMY_ROUTING))
        message(FATAL_ERROR "Powered Cannon screen requires the replay PvZ opening")
    endif()
    if(PROTODD_PVZ_EARLY_MINERAL_FALLBACK AND
       (NOT PROTODD_PVZ_REPLAY_OPENING OR NOT PROTODD_PVZ_MINERAL_FALLBACK OR
        PROTODD_PVZ_POWERED_CANNON_SCREEN))
        message(FATAL_ERROR "Early mineral fallback requires its base flag and replay PvZ opening")
    endif()
    if(PROTODD_PVZ_PROACTIVE_REAVER AND
       (NOT PROTODD_PVZ_POWERED_CANNON_SCREEN OR PROTODD_PVZ_MINERAL_FALLBACK))
        message(FATAL_ERROR "Proactive Reaver requires the isolated powered Cannon screen")
    endif()
    if(PROTODD_PVZ_ARMY_FLOOR AND
       (NOT PROTODD_PVZ_POWERED_CANNON_SCREEN OR PROTODD_PVZ_MINERAL_FALLBACK OR
        PROTODD_PVZ_PROACTIVE_REAVER))
        message(FATAL_ERROR "PvZ army floor requires the isolated powered Cannon screen")
    endif()
    if(PROTODD_PVZ_DETECTOR_SURGE AND NOT PROTODD_PVZ_ARMY_FLOOR)
        message(FATAL_ERROR "PvZ detector surge requires the frozen army-floor reference")
    endif()
    if(PROTODD_WHOLE_GAME_EVALUATION_BUILD AND NOT PROTODD_WHOLE_GAME_CONTROL)
        message(FATAL_ERROR "Evaluation build requires whole-game control")
    endif()
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
        src/bwapi/ModelRuntime.cpp
        src/bwapi/ProductionRuntime.cpp
        src/bwapi/ProductionFeedback.cpp
        src/bwapi/WholeGameRuntime.cpp
        src/bwapi/WholeGameAction.cpp
        src/cpu/WholeGameCpu.cpp
        src/cpu/WholeGameEncoder.cpp
        src/cpu/WholeGameIntent.cpp
    )
    if(PROTODD_WHOLE_GAME_WEIGHTS)
        if(NOT EXISTS "${PROTODD_WHOLE_GAME_WEIGHTS}")
            message(FATAL_ERROR "Whole-game weight file does not exist: ${PROTODD_WHOLE_GAME_WEIGHTS}")
        endif()
        enable_language(RC)
        file(TO_CMAKE_PATH "${PROTODD_WHOLE_GAME_WEIGHTS}" PROTODD_WHOLE_GAME_WEIGHTS_RC)
        set(PROTODD_WHOLE_GAME_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/whole-game-weights.rc")
        file(WRITE "${PROTODD_WHOLE_GAME_RESOURCE}"
            "101 RCDATA \"${PROTODD_WHOLE_GAME_WEIGHTS_RC}\"\n")
        target_sources(protodd_bwapi PRIVATE "${PROTODD_WHOLE_GAME_RESOURCE}")
        set_property(SOURCE "${PROTODD_WHOLE_GAME_RESOURCE}" APPEND PROPERTY
            OBJECT_DEPENDS "${PROTODD_WHOLE_GAME_WEIGHTS}")
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_EMBED_WHOLE_GAME_WEIGHTS=1)
    endif()
    if(PROTODD_PRODUCTION_LOCAL_EVALUATION)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_PRODUCTION_LOCAL_EVALUATION=1)
    endif()
    if(PROTODD_TACTICAL_LOCAL_EVALUATION)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_TACTICAL_LOCAL_EVALUATION=1)
    endif()
    if(PROTODD_PVZ_GATEWAY_OPENING)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_PVZ_GATEWAY_OPENING=1)
    endif()
    if(PROTODD_PVZ_EARLY_SPLASH)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_PVZ_EARLY_SPLASH=1)
    endif()
    if(PROTODD_PVP_FOG_DETECTION)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_PVP_FOG_DETECTION=1)
    endif()
    if(PROTODD_PVP_SCOUTED_TWO_GATE_ANCHOR)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_PVP_SCOUTED_TWO_GATE_ANCHOR=1)
    endif()
    if(PROTODD_THREATENED_NATURAL_RALLY)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_THREATENED_NATURAL_RALLY=1)
    endif()
    if(PROTODD_COVERED_PRESSURE_RELEASE)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_COVERED_PRESSURE_RELEASE=1)
    endif()
    if(PROTODD_PVT_CONTAIN_BREAK)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_PVT_CONTAIN_BREAK=1)
    endif()
    if(PROTODD_FORWARD_DETECTOR_ESCORT)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_FORWARD_DETECTOR_ESCORT=1)
    endif()
    if(PROTODD_DIRECT_DETECTOR_RENDEZVOUS)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_DIRECT_DETECTOR_RENDEZVOUS=1)
    endif()
    if(PROTODD_CONTESTED_DETECTOR_RESERVE)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_CONTESTED_DETECTOR_RESERVE=1)
    endif()
    if(PROTODD_FORWARD_THIRD_SCREEN)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_FORWARD_THIRD_SCREEN=1)
    endif()
    if(PROTODD_BASE_THREAT_EXPANSION_GUARD)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_BASE_THREAT_EXPANSION_GUARD=1)
    endif()
    if(PROTODD_ABANDONED_BASE_WORKER_EVACUATION)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_ABANDONED_BASE_WORKER_EVACUATION=1)
    endif()
    if(PROTODD_SAFE_REMOTE_MINING)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_SAFE_REMOTE_MINING=1)
    endif()
    if(PROTODD_BASE_DEFENSE_CONSOLIDATION)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_BASE_DEFENSE_CONSOLIDATION=1)
    endif()
    if(PROTODD_DETECTOR_WAIT_VOLLEY)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_DETECTOR_WAIT_VOLLEY=1)
    endif()
    if(PROTODD_PVZ_REPLAY_OPENING)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_PVZ_REPLAY_OPENING=1)
    endif()
    if(PROTODD_PVZ_ARCHIVES_FIRST)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_PVZ_ARCHIVES_FIRST=1)
    endif()
    if(PROTODD_LATE_ECONOMY_RECOVERY)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_LATE_ECONOMY_RECOVERY=1)
    endif()
    if(PROTODD_STALLED_ARMY_ROUTING)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_STALLED_ARMY_ROUTING=1)
    endif()
    if(PROTODD_PVZ_MINERAL_FALLBACK)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_PVZ_MINERAL_FALLBACK=1)
    endif()
    if(PROTODD_PVZ_EARLY_MINERAL_FALLBACK)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_PVZ_EARLY_MINERAL_FALLBACK=1)
    endif()
    if(PROTODD_PVZ_POWERED_CANNON_SCREEN)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_PVZ_POWERED_CANNON_SCREEN=1)
    endif()
    if(PROTODD_PVZ_PROACTIVE_REAVER)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_PVZ_PROACTIVE_REAVER=1)
    endif()
    if(PROTODD_PVZ_ARMY_FLOOR)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_PVZ_ARMY_FLOOR=1)
    endif()
    if(PROTODD_PVZ_DETECTOR_SURGE)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_PVZ_DETECTOR_SURGE=1)
    endif()
    if(PROTODD_STATIC_DEFENSE_COVERAGE)
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_STATIC_DEFENSE_COVERAGE=1)
    endif()
    if(PROTODD_WHOLE_GAME_CONTROL)
        if(NOT PROTODD_WHOLE_GAME_WEIGHTS)
            message(FATAL_ERROR "Whole-game control requires embedded weights")
        endif()
        file(READ "${PROTODD_WHOLE_GAME_WEIGHTS}" PROTODD_WEIGHT_VERSION_HEX
            OFFSET 8 LIMIT 4 HEX)
        if(PROTODD_WHOLE_GAME_EVALUATION_BUILD AND
           NOT PROTODD_WEIGHT_VERSION_HEX STREQUAL "02000000")
            message(FATAL_ERROR "Evaluation build requires six-slot weights")
        endif()
        if(PROTODD_WEIGHT_VERSION_HEX STREQUAL "02000000")
            if(NOT PROTODD_WHOLE_GAME_EVALUATION_BUILD AND
               (NOT PROTODD_WHOLE_GAME_PROMOTION_RECEIPT OR
                NOT EXISTS "${PROTODD_WHOLE_GAME_PROMOTION_RECEIPT}"))
                message(FATAL_ERROR
                    "Six-slot whole-game control requires a promotion receipt for these weights")
            endif()
            if(NOT PROTODD_WHOLE_GAME_EVALUATION_BUILD)
                file(READ "${PROTODD_WHOLE_GAME_PROMOTION_RECEIPT}" PROTODD_PROMOTION_JSON)
                string(JSON PROTODD_PROMOTION_SCHEMA GET "${PROTODD_PROMOTION_JSON}" schema)
                string(JSON PROTODD_PROMOTION_READY GET "${PROTODD_PROMOTION_JSON}" tournament_ready)
                string(JSON PROTODD_PROMOTION_SHA GET "${PROTODD_PROMOTION_JSON}" weights_sha256)
                file(SHA256 "${PROTODD_WHOLE_GAME_WEIGHTS}" PROTODD_EMBEDDED_SHA)
                if(NOT PROTODD_PROMOTION_SCHEMA STREQUAL "protodd-whole-game-promotion-v1" OR
                   NOT PROTODD_PROMOTION_READY OR
                   NOT PROTODD_PROMOTION_SHA STREQUAL PROTODD_EMBEDDED_SHA)
                    message(FATAL_ERROR
                        "Six-slot promotion receipt is not ready or does not match embedded weights")
                endif()
                find_package(Python3 COMPONENTS Interpreter REQUIRED)
                execute_process(
                    COMMAND "${Python3_EXECUTABLE}" -m training.whole_game_multislot_promotion
                            verify "${PROTODD_WHOLE_GAME_PROMOTION_RECEIPT}"
                            "${PROTODD_WHOLE_GAME_WEIGHTS}"
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    RESULT_VARIABLE PROTODD_PROMOTION_RESULT
                    OUTPUT_VARIABLE PROTODD_PROMOTION_OUTPUT
                    ERROR_VARIABLE PROTODD_PROMOTION_ERROR)
                if(NOT PROTODD_PROMOTION_RESULT EQUAL 0)
                    message(FATAL_ERROR "Six-slot promotion evidence failed verification:\n${PROTODD_PROMOTION_ERROR}")
                endif()
            endif()
        endif()
        target_compile_definitions(protodd_bwapi PRIVATE PROTODD_WHOLE_GAME_CONTROL=1)
    endif()
    target_include_directories(protodd_bwapi PRIVATE
        "${BWAPI_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bwapi"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu"
    )
    target_link_libraries(protodd_bwapi PRIVATE Protodd::Core "${BWAPI_LIBRARY}")
    target_compile_features(protodd_bwapi PRIVATE cxx_std_20)
    target_compile_definitions(protodd_bwapi PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_compile_options(protodd_bwapi PRIVATE /arch:SSE2)
    endif()
    protodd_enable_warnings(protodd_bwapi)
    set_target_properties(protodd_bwapi PROPERTIES
        OUTPUT_NAME "Protodd"
        PREFIX ""
        SUFFIX ".dll"
    )
    if(PROTODD_WHOLE_GAME_EVALUATION_BUILD)
        set_target_properties(protodd_bwapi PROPERTIES OUTPUT_NAME "ProtoddEvaluation")
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_custom_command(TARGET protodd_bwapi POST_BUILD
            COMMAND "${Python3_EXECUTABLE}" -m training.whole_game_multislot_promotion
                    record-evaluation "$<TARGET_FILE:protodd_bwapi>"
                    "$<TARGET_FILE_DIR:protodd_bwapi>/ProtoddEvaluation.build.json"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Record the evaluated DLL and frozen source fingerprint")
    endif()
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

add_executable(model_tool tools/model_tool.cpp)
target_link_libraries(model_tool PRIVATE Protodd::Core)
target_compile_features(model_tool PRIVATE cxx_std_20)
protodd_enable_warnings(model_tool)

if(PROTODD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    find_program(PROTODD_NODE_EXECUTABLE node)
    if(PROTODD_NODE_EXECUTABLE)
        add_test(NAME protodd_replay_comparison_tests
            COMMAND "${PROTODD_NODE_EXECUTABLE}" --test "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_replay_comparison.mjs")
    endif()
    if(PROTODD_BUILD_BWAPI_MODULE)
        add_library(protodd_audit_scenario MODULE tests/bwapi_audit_scenario.cpp src/bwapi/BwapiBridge.cpp)
        target_include_directories(protodd_audit_scenario PRIVATE "${BWAPI_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src/bwapi")
        target_link_libraries(protodd_audit_scenario PRIVATE Protodd::Core "${BWAPI_LIBRARY}")
        target_compile_features(protodd_audit_scenario PRIVATE cxx_std_20)
        target_compile_definitions(protodd_audit_scenario PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        set_target_properties(protodd_audit_scenario PROPERTIES PREFIX "" OUTPUT_NAME "AuditScenario" SUFFIX ".dll")
        add_library(protodd_audit_load MODULE tests/bwapi_audit_load.cpp)
        target_include_directories(protodd_audit_load PRIVATE "${BWAPI_INCLUDE_DIR}")
        target_link_libraries(protodd_audit_load PRIVATE "${BWAPI_LIBRARY}")
        target_compile_features(protodd_audit_load PRIVATE cxx_std_20)
        target_compile_definitions(protodd_audit_load PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        set_target_properties(protodd_audit_load PROPERTIES PREFIX "" OUTPUT_NAME "AuditLoad" SUFFIX ".dll")
        add_executable(protodd_bwapi_catalog_tests tests/bwapi_catalog.cpp)
        target_include_directories(protodd_bwapi_catalog_tests PRIVATE "${BWAPI_INCLUDE_DIR}")
        target_link_libraries(protodd_bwapi_catalog_tests PRIVATE Protodd::Core "${BWAPI_LIBRARY}")
        target_compile_features(protodd_bwapi_catalog_tests PRIVATE cxx_std_20)
        protodd_enable_warnings(protodd_bwapi_catalog_tests)
        add_test(NAME protodd_bwapi_catalog_tests COMMAND protodd_bwapi_catalog_tests)
    endif()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        foreach(PROTODD_NEW_TEST IN ITEMS data_release dataset_analysis ladder arena whole_game_pilot whole_game_labels whole_game_parity whole_game_model whole_game_sequences)
            add_test(NAME "protodd_${PROTODD_NEW_TEST}_tests"
                COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                    -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_${PROTODD_NEW_TEST}.py")
            set_tests_properties("protodd_${PROTODD_NEW_TEST}_tests" PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        endforeach()
        add_test(NAME protodd_replay_pipeline_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_replay_pipeline.py")
        add_test(NAME protodd_tactical_target_fit_tests
            COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p test_tactical_target_fit.py)
        set_tests_properties(protodd_tactical_target_fit_tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME protodd_replay_bundle_tests
            COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p test_replay_bundle.py)
        set_tests_properties(protodd_replay_bundle_tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME protodd_parallel_validation_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_parallel_validation.py")
        add_test(NAME protodd_replay_catalog_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_replay_catalog.py")
        add_test(NAME protodd_replay_playback_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_replay_playback.py")
        add_test(NAME protodd_replay_audit_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_replay_audit.py")
        add_test(NAME protodd_model_dataset_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_model_dataset.py"
                "$<TARGET_FILE:model_tool>")
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import torch, numpy"
            RESULT_VARIABLE PROTODD_MODEL_TRAINING_AVAILABLE OUTPUT_QUIET ERROR_QUIET)
        if(PROTODD_MODEL_TRAINING_AVAILABLE EQUAL 0)
            add_test(NAME protodd_tensor_shards_tests
                COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                    -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p test_tensor_shards.py)
            set_tests_properties(protodd_tensor_shards_tests PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
            add_test(NAME protodd_shard_training_tests
                COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                    -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p test_shard_training.py)
            set_tests_properties(protodd_shard_training_tests PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
            add_test(NAME protodd_model_training_tests
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_model_training.py"
                    "$<TARGET_FILE:model_tool>")
        endif()
        add_test(NAME protodd_policy_dataset_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_train_policy.py")
        add_test(NAME protodd_training_validation_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_train_openings.py")
        add_test(NAME protodd_policy_pipeline_test
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_policy_pipeline.py"
                "$<TARGET_FILE:policy_trainer>")
        add_test(NAME protodd_decision_report_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/decision_report.py" --self-test)
        add_test(NAME protodd_control_campaign_review_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_control_campaign_review.py")
        add_test(NAME protodd_control_outcome_audit_tests
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_control_outcome_audit.py")
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
