if(WIN32)
    message(STATUS "kcpmux demo is a POSIX socket demo and is not built on Windows")
    return()
endif()

set(KCPMUX_DEMO_COMMON_SOURCES
    demo_common.c
)

add_executable(demo_server
    ${KCPMUX_DEMO_COMMON_SOURCES}
    demo_server.c
)

add_executable(demo_client
    ${KCPMUX_DEMO_COMMON_SOURCES}
    demo_client.c
)

target_link_libraries(demo_server PRIVATE kcpmux)
target_link_libraries(demo_client PRIVATE kcpmux)
