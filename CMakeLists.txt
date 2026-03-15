cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
    endif()
endif()

project(VitaMusicAssistant LANGUAGES C CXX)

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Vita Music Assistant")
set(VITA_TITLEID "VMUSICAS")
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable GXM graphics for Borealis
add_compile_definitions(
    __psp2__
    __vita__
    BOREALIS_USE_GXM
    BOREALIS_RESOURCES="app0:"
)

# Wrap pthread_create to enforce minimum 512KB stack
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--wrap=pthread_create")

# Source files - Music Assistant client (no video, no Plex, no downloads)
set(SOURCES
    src/main.cpp
    src/app.cpp

    # Activities
    src/activity/login_activity.cpp
    src/activity/main_activity.cpp
    src/activity/player_activity.cpp

    # App core
    src/app/application.cpp
    src/app/ma_client.cpp
    src/app/music_queue.cpp
    src/app/websocket_client.cpp
    src/app/sendspin_client.cpp

    # Player (audio-only)
    src/player/mpv_player.cpp

    # Utilities
    src/utils/http_client.cpp
    src/utils/image_loader.cpp
    src/utils/vita_stubs.c

    # Views
    src/view/home_tab.cpp
    src/view/library_tab.cpp
    src/view/music_tab.cpp
    src/view/search_tab.cpp
    src/view/settings_tab.cpp
    src/view/media_item_cell.cpp
    src/view/recycling_grid.cpp
    src/view/overlay_dialog.cpp
    src/view/progress_dialog.cpp
    src/view/horizontal_scroll_row.cpp
    src/view/now_playing_view.cpp
    src/view/queue_tab.cpp
)

# Borealis submodule
add_subdirectory(lib/borealis)

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/borealis/library/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/borealis/library/include/borealis/extern
)

target_link_libraries(${PROJECT_NAME}
    borealis

    # Media playback (audio only)
    mpv
    avformat
    avcodec
    swresample
    avutil

    # Networking
    curl
    mbedtls
    mbedx509
    mbedcrypto

    # Text rendering
    ass
    freetype
    harfbuzz
    fribidi

    # Image handling
    png
    webp
    jpeg

    # Compression
    z
    zstd

    # Vita SDK modules
    SceDisplay_stub
    SceCtrl_stub
    SceAudio_stub
    SceAudioIn_stub
    SceSysmodule_stub
    SceGxm_stub
    SceCommonDialog_stub
    SceTouch_stub
    SceHid_stub
    SceMotion_stub
    ScePower_stub
    SceAppMgr_stub
    SceAppUtil_stub
    SceBgAppUtil_stub
    SceShellSvc_stub
    SceNet_stub
    SceNetCtl_stub
    SceSsl_stub
    SceHttp_stub
    SceIme_stub
    ScePgf_stub
    SceKernelThreadMgr_stub
    SceKernelModulemgr_stub
    SceLibKernel_stub
    SceProcessmgr_stub
    SceIofilemgr_stub
    SceFios2_stub
    SceRegistryMgr_stub
    SceMusicExport_stub
    SceNpCommon_stub
    SceNpManager_stub

    # System
    pthread
    m
    stdc++
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
    FILE sce_sys/pic0.png sce_sys/pic0.png
)

# Add resource files to VPK
file(GLOB_RECURSE RESOURCE_FILES
    "resources/i18n/*"
    "resources/xml/*"
    "resources/icons/*"
    "resources/images/*"
    "resources/font/*"
)
foreach(RES_FILE ${RESOURCE_FILES})
    file(RELATIVE_PATH REL_PATH ${CMAKE_CURRENT_SOURCE_DIR} ${RES_FILE})
    vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
        FILE ${RES_FILE} ${REL_PATH}
    )
endforeach()
