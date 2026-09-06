add_executable(protodd_tests
    test_main.cpp
)
target_link_libraries(protodd_tests PRIVATE Protodd::Core)
target_compile_features(protodd_tests PRIVATE cxx_std_20)
protodd_enable_warnings(protodd_tests)

add_test(NAME protodd_core_tests COMMAND protodd_tests)

