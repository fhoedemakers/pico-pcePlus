add_library(pcecore INTERFACE)

target_sources(pcecore INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/h6280.c
    ${CMAKE_CURRENT_SOURCE_DIR}/pce.c
    ${CMAKE_CURRENT_SOURCE_DIR}/pce-go.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gfx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/psg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cd_chd.c
)

# cd_chd.c is always compiled but its libchdr-touching body is guarded by
# ENABLE_CHD; when off it expands to trivial stubs. The libchdr_pico link
# (and CHD header path) is only added when ENABLE_CHD is on.
if(ENABLE_CHD)
    target_link_libraries(pcecore INTERFACE libchdr_pico)
endif()

target_include_directories(pcecore INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
