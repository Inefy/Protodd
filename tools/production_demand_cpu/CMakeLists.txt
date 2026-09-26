cmake_minimum_required(VERSION 3.24)
project(ProductionDemandCpu LANGUAGES CXX)
add_executable(production_demand_cpu main.cpp
    ../../src/core/ProductionDemandModel.cpp ../../src/core/ObservationEncoder.cpp
    ../../src/core/GameState.cpp ../../src/core/UnitCatalog.cpp ../../src/core/Technology.cpp)
target_include_directories(production_demand_cpu PRIVATE ../../include)
target_compile_features(production_demand_cpu PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(production_demand_cpu PRIVATE /W4 /fp:precise)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_compile_options(production_demand_cpu PRIVATE /arch:SSE2)
    endif()
endif()
