cmake_minimum_required(VERSION 3.16)
project(poolconductor_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(poolconductor_tests
    hal_mock.cpp
    main.cpp
    test_integration_pentair.cpp
    test_integration_relays.cpp
    test_pentair.cpp
    test_relays.cpp
    test_sensors.cpp
    test_state.cpp
)

target_include_directories(poolconductor_tests PRIVATE .)

# Run tests as a post-build step
add_custom_command(TARGET poolconductor_tests POST_BUILD
    COMMAND $<TARGET_FILE:poolconductor_tests>
    COMMENT "Running unit tests..."
)
