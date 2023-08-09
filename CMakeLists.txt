add_library(sdcard INTERFACE)

target_sources(sdcard INTERFACE
 ${CMAKE_CURRENT_LIST_DIR}/fs_fatfs.c
 ${CMAKE_CURRENT_LIST_DIR}/fs_littlefs.c
 ${CMAKE_CURRENT_LIST_DIR}/macros.c
 ${CMAKE_CURRENT_LIST_DIR}/sdcard.c
 ${CMAKE_CURRENT_LIST_DIR}/ymodem.c
)

target_include_directories(sdcard INTERFACE ${CMAKE_CURRENT_LIST_DIR})
