add_executable(astra_tests
    test_main.cpp
)
target_link_libraries(astra_tests PRIVATE Astra::Core)
target_compile_features(astra_tests PRIVATE cxx_std_20)
astra_enable_warnings(astra_tests)

add_test(NAME astra_core_tests COMMAND astra_tests)

