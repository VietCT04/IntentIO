cmake_minimum_required(VERSION 3.20)

project(IntentIO
    VERSION 0.1.0
    DESCRIPTION "Intent-aware asynchronous I/O runtime for Linux"
    LANGUAGES CXX
)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "IntentIO currently supports Linux only")
endif()

option(INTENTIO_BUILD_TESTS "Build IntentIO tests" ON)
option(INTENTIO_BUILD_BENCHMARKS "Build IntentIO benchmarks" ON)
option(INTENTIO_BUILD_EXAMPLES "Build IntentIO examples" ON)
option(INTENTIO_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)
option(INTENTIO_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

find_package(Threads REQUIRED)

add_library(intentio STATIC
    src/backends/io_uring_backend.cpp
    src/backends/sync_backend.cpp
    src/backends/threaded_pread_backend.cpp
    src/concurrency.cpp
    src/execution_policy.cpp
    src/group.cpp
    src/kernel_io.cpp
    src/policy.cpp
    src/planner.cpp
    src/runtime.cpp
    src/submission.cpp
)

add_library(intentio::intentio ALIAS intentio)

target_link_libraries(intentio PUBLIC Threads::Threads)

target_compile_features(intentio PUBLIC cxx_std_20)
target_include_directories(intentio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(intentio PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if (INTENTIO_ENABLE_SANITIZERS AND INTENTIO_ENABLE_TSAN)
    message(FATAL_ERROR "Choose INTENTIO_ENABLE_SANITIZERS or INTENTIO_ENABLE_TSAN, not both")
endif()

if (INTENTIO_ENABLE_SANITIZERS)
    target_compile_options(intentio PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(intentio PUBLIC -fsanitize=address,undefined)
endif()

if (INTENTIO_ENABLE_TSAN)
    target_compile_options(intentio PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(intentio PUBLIC -fsanitize=thread)
endif()

if (INTENTIO_BUILD_TESTS)
    enable_testing()
    add_executable(intentio_tests tests/intentio_tests.cpp)
    target_link_libraries(intentio_tests PRIVATE intentio)
    add_test(NAME intentio_tests COMMAND intentio_tests)
endif()

if (INTENTIO_BUILD_BENCHMARKS)
    add_executable(intentio-info benchmarks/intentio_info.cpp)

    add_executable(intentio_benchmark benchmarks/intentio_bench.cpp)
    target_link_libraries(intentio_benchmark PRIVATE intentio)

    add_executable(intentio_phase4_benchmark benchmarks/intentio_phase4_bench.cpp)
    target_link_libraries(intentio_phase4_benchmark PRIVATE intentio)

    add_executable(intentio_planner_benchmark benchmarks/planner_bench.cpp)
    target_link_libraries(intentio_planner_benchmark PRIVATE intentio)
    target_include_directories(intentio_planner_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    add_executable(intentio_phase5_benchmark benchmarks/intentio_phase5_bench.cpp)
    target_link_libraries(intentio_phase5_benchmark PRIVATE intentio)

    add_executable(intentio_phase6_benchmark benchmarks/intentio_phase6_bench.cpp)
    target_link_libraries(intentio_phase6_benchmark PRIVATE intentio)

    add_executable(intentio_phase7_benchmark benchmarks/intentio_phase7_bench.cpp)
    target_link_libraries(intentio_phase7_benchmark PRIVATE intentio)

    add_executable(intentio_phase9_benchmark benchmarks/intentio_phase9_bench.cpp)
    target_link_libraries(intentio_phase9_benchmark PRIVATE intentio)
endif()

if (INTENTIO_BUILD_EXAMPLES)
    add_executable(intentio_example examples/basic.cpp)
    target_link_libraries(intentio_example PRIVATE intentio)
endif()

include(GNUInstallDirs)
install(TARGETS intentio
    EXPORT intentioTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT intentioTargets
    FILE intentioTargets.cmake
    NAMESPACE intentio::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/IntentIO
)
