cmake_minimum_required(VERSION 3.10)

project(kcp-mux
    VERSION 1.0.0
    DESCRIPTION "Multiplexed stream transport over KCP"
    LANGUAGES C
)

include(GNUInstallDirs)

option(KCPMUX_BUILD_TESTS "Build kcpmux tests" OFF)
option(KCPMUX_BUILD_DEMOS "Build kcpmux demos" OFF)

if(KCPMUX_BUILD_TESTS)
    set(KCPMUX_BUILD_DEMOS ON CACHE BOOL "Build kcpmux demos" FORCE)
endif()

add_library(kcpmux STATIC
    third_party/kcp/ikcp.c
    src/internal/kcpmux_hash.c
    src/internal/kcpmux_hashtable.c
    src/kcpmux_kcp.c
    src/kcpmux_engine.c
    src/kcpmux_conn.c
    src/kcpmux_stream.c
    src/kcpmux_protocol.c
    src/kcpmux_timer.c
)

add_library(kcp-mux::kcpmux ALIAS kcpmux)

target_compile_features(kcpmux PUBLIC c_std_99)
target_include_directories(kcpmux
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/kcpmux
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kcp
)

set_target_properties(kcpmux PROPERTIES
    OUTPUT_NAME kcpmux
    EXPORT_NAME kcpmux
)

if(KCPMUX_BUILD_DEMOS)
    add_subdirectory(demo)
endif()

if(KCPMUX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS kcpmux
    EXPORT kcpmuxTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/kcpmux
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

include(CMakePackageConfigHelpers)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/kcpmuxConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    cmake/kcpmuxConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/kcpmuxConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kcpmux
)

install(EXPORT kcpmuxTargets
    NAMESPACE kcp-mux::
    FILE kcpmuxTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kcpmux
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/kcpmuxConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/kcpmuxConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kcpmux
)
