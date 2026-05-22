add_library(pcecore INTERFACE)

target_sources(pcecore INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/h6280.c
    ${CMAKE_CURRENT_SOURCE_DIR}/pce.c
    ${CMAKE_CURRENT_SOURCE_DIR}/pce-go.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gfx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/psg.c
)

target_include_directories(pcecore INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
