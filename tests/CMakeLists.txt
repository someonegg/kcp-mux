enable_language(CXX)

include(FetchContent)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    googletest
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    URL https://github.com/google/googletest/releases/download/v1.15.0/googletest-1.15.0.tar.gz
)
FetchContent_MakeAvailable(googletest)

add_executable(kcpmux_tests
    kcpmux_data_test.cc
    kcpmux_e2e_basic_test.cc
    kcpmux_e2e_stream_test.cc
    kcpmux_e2e_transfer_test.cc
    kcpmux_e2e_utils.cc
    kcpmux_lifecycle_test.cc
    kcpmux_protocol_test.cc
    kcpmux_stats_test.cc
)

target_compile_features(kcpmux_tests PRIVATE cxx_std_17)
target_include_directories(kcpmux_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include/kcpmux
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/internal
)
target_link_libraries(kcpmux_tests PRIVATE
    kcpmux
    GTest::gtest_main
    GTest::gmock_main
)

if(WIN32)
    target_link_libraries(kcpmux_tests PRIVATE ws2_32)
endif()

include(GoogleTest)
gtest_discover_tests(kcpmux_tests)

if(TARGET demo_server AND TARGET demo_client)
    add_test(
        NAME kcpmux_demo_smoke
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.sh
                $<TARGET_FILE:demo_server>
                $<TARGET_FILE:demo_client>
    )
endif()
