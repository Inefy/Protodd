cmake_minimum_required(VERSION 3.24)
project(protodd_replay_checkpoints LANGUAGES CXX)
set(OPENBW_SOURCE_DIR "" CACHE PATH "Local alexpineda/openbw checkout")
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${OPENBW_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE OPENBW_REV OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT OPENBW_REV STREQUAL "86fc7f6da3c7a01542471f2ea303eb4bde0f9f04")
    message(FATAL_ERROR "Replay adapter requires the documented, pinned OpenBW revision")
endif()
set(PROTODD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_executable(replay_extract extract.cpp
    "${PROTODD_ROOT}/src/core/ObservationEncoder.cpp"
    "${PROTODD_ROOT}/src/core/GameState.cpp"
    "${PROTODD_ROOT}/src/core/UnitCatalog.cpp"
    "${PROTODD_ROOT}/src/core/Technology.cpp")
target_compile_features(replay_extract PRIVATE cxx_std_20)
target_include_directories(replay_extract SYSTEM PRIVATE "${OPENBW_SOURCE_DIR}")
target_include_directories(replay_extract PRIVATE "${PROTODD_ROOT}/include")
add_executable(replay_extract_selftest selftest.cpp
    "${PROTODD_ROOT}/src/core/ObservationEncoder.cpp" "${PROTODD_ROOT}/src/core/GameState.cpp"
    "${PROTODD_ROOT}/src/core/UnitCatalog.cpp" "${PROTODD_ROOT}/src/core/Technology.cpp")
target_compile_features(replay_extract_selftest PRIVATE cxx_std_20)
target_include_directories(replay_extract_selftest SYSTEM PRIVATE "${OPENBW_SOURCE_DIR}")
target_include_directories(replay_extract_selftest PRIVATE "${PROTODD_ROOT}/include")
if(MSVC)
    target_compile_options(replay_extract_selftest PRIVATE /bigobj /w /permissive)
    target_compile_definitions(replay_extract_selftest PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX
        _HAS_DEPRECATED_RESULT_OF=1 _HAS_DEPRECATED_IS_LITERAL_TYPE=1 _HAS_DEPRECATED_ALLOCATOR_MEMBERS=1)
    target_compile_options(replay_extract PRIVATE /bigobj /w /permissive)
    target_compile_definitions(replay_extract PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX
        _HAS_DEPRECATED_RESULT_OF=1 _HAS_DEPRECATED_IS_LITERAL_TYPE=1 _HAS_DEPRECATED_ALLOCATOR_MEMBERS=1)
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${OPENBW_SOURCE_DIR}" diff --exit-code HEAD
    RESULT_VARIABLE OPENBW_DIRTY OUTPUT_QUIET)
if(NOT OPENBW_DIRTY EQUAL 0)
    message(FATAL_ERROR "Replay engine has local modifications; record and validate a new pin first")
endif()
add_executable(replay_checkpoints main.cpp)
target_compile_features(replay_checkpoints PRIVATE cxx_std_17)
target_include_directories(replay_checkpoints SYSTEM PRIVATE "${OPENBW_SOURCE_DIR}")
if(MSVC)
    target_compile_options(replay_checkpoints PRIVATE /bigobj /w /permissive)
    target_compile_definitions(replay_checkpoints PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
