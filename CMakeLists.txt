cmake_minimum_required(VERSION 3.20)
project(esp32_cam_livestream LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ECL_ENABLE_ZLIB "Enable host GENERIC_ZLIB negative-control codec mode" ON)
option(ECL_BUILD_TESTS "Build host unit tests, fuzz harness, and benchmark tools" ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

if(ECL_ENABLE_ZLIB)
    find_package(ZLIB QUIET)
endif()

add_subdirectory(codec)
add_subdirectory(transport)

if(ECL_BUILD_TESTS)
    enable_testing()

    # Locate libjpeg / libjpeg-turbo (Homebrew Apple Silicon, Homebrew Intel, or system)
    find_path(JPEG_INCLUDE_DIR jpeglib.h
        HINTS /opt/homebrew/opt/jpeg-turbo/include /usr/local/opt/jpeg-turbo/include /opt/homebrew/include
    )
    find_library(JPEG_LIBRARY NAMES jpeg
        HINTS /opt/homebrew/opt/jpeg-turbo/lib /usr/local/opt/jpeg-turbo/lib /opt/homebrew/lib
    )
    if(NOT JPEG_INCLUDE_DIR OR NOT JPEG_LIBRARY)
        message(FATAL_ERROR "libjpeg / libjpeg-turbo is required for host tests and fixtures")
    endif()
    message(STATUS "Found libjpeg: ${JPEG_LIBRARY} (${JPEG_INCLUDE_DIR})")

    add_library(ecl_host_jpeg INTERFACE)
    target_include_directories(ecl_host_jpeg INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/host/common
        ${JPEG_INCLUDE_DIR}
    )
    target_link_libraries(ecl_host_jpeg INTERFACE ${JPEG_LIBRARY})

    # Unit tests
    add_executable(test_transport transport/tests/test_transport.cpp)
    target_link_libraries(test_transport PRIVATE ecl_transport)
    add_test(NAME test_transport COMMAND test_transport)

    add_executable(test_jpeg_parser codec/tests/test_jpeg_parser.cpp)
    target_link_libraries(test_jpeg_parser PRIVATE ecl_codec ecl_host_jpeg)
    add_test(NAME test_jpeg_parser COMMAND test_jpeg_parser)

    add_executable(test_temporal_codec codec/tests/test_temporal_codec.cpp)
    target_link_libraries(test_temporal_codec PRIVATE ecl_transport ecl_host_jpeg)
    add_test(NAME test_temporal_codec COMMAND test_temporal_codec)

    # Deterministic standalone mutation fuzz harness (also supports -DECL_LIBFUZZER=1)
    add_executable(fuzz_jpeg_parser codec/fuzz/fuzz_jpeg_parser.cpp)
    target_link_libraries(fuzz_jpeg_parser PRIVATE ecl_codec ecl_host_jpeg)
    add_test(NAME fuzz_jpeg_parser_smoke COMMAND fuzz_jpeg_parser 5000)

    # Host CLI tools
    add_executable(ecl_generate_fixtures host/fixtures/generate_fixtures.cpp)
    target_link_libraries(ecl_generate_fixtures PRIVATE ecl_host_jpeg)

    add_executable(ecl_replay host/replay/replay_cli.cpp)
    target_link_libraries(ecl_replay PRIVATE ecl_transport ecl_host_jpeg)

    add_executable(ecl_benchmark host/benchmark/benchmark_cli.cpp)
    target_link_libraries(ecl_benchmark PRIVATE ecl_transport ecl_host_jpeg)

    add_executable(ecl_receiver host/receiver/receiver_cli.cpp)
    target_link_libraries(ecl_receiver PRIVATE ecl_transport ecl_host_jpeg)
endif()
