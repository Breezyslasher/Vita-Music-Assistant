cmake_minimum_required(VERSION 3.16)

# Set Borealis library path FIRST (required by cmake includes)
set(BOREALIS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/borealis)
set(BOREALIS_LIBRARY ${BOREALIS_DIR}/library)

# Set platform for Borealis BEFORE including cmake files
set(PLATFORM_DESKTOP OFF CACHE BOOL "Build for Desktop" FORCE)
set(PLATFORM_SWITCH OFF CACHE BOOL "Build for Switch" FORCE)
set(PLATFORM_PSV ON CACHE BOOL "Build for PS Vita" FORCE)
set(PLATFORM_PS4 OFF CACHE BOOL "Build for PS4" FORCE)
set(PLATFORM_IOS OFF CACHE BOOL "Build for iOS" FORCE)
set(PLATFORM_ANDROID OFF CACHE BOOL "Build for Android" FORCE)

# Enable native GXM graphics for PS Vita (must be BOREALIS_USE_GXM)
set(BOREALIS_USE_GXM ON CACHE BOOL "Use GXM for PS Vita" FORCE)
# Disable VitaShark for now - requires libshacccg.suprx module which we don't have
# Uses precompiled shaders instead
set(USE_VITA_SHARK OFF CACHE BOOL "Use VitaShark for shader compilation" FORCE)

# Disable features not needed for Vita
set(USE_LIBROMFS OFF CACHE BOOL "Use libromfs" FORCE)

# Include Borealis cmake files (like switchfin does)
# This properly sets up platform options and toolchain
include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

# Project definition (AFTER toolchain include)
project(VitaMusicAssistant VERSION 2.0.0 LANGUAGES C CXX)

# C++17 required for Borealis
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable RTTI and exceptions (required by Borealis)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -frtti -fexceptions")

# App metadata
set(VITA_APP_NAME "Vita Music Assistant")
set(VITA_TITLEID "VMASS0001")
set(VITA_VERSION "02.00")
# ATTRIBUTE2=12 enables maximum heap size mode - CRITICAL for memory-intensive apps
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1 -d ATTRIBUTE2=12")

# Apply patches to borealis before building
# Fix: psv_platform.cpp uses wrong macro (__SDL2__ instead of BOREALIS_USE_GXM)
set(PSV_PLATFORM_PATCH ${CMAKE_CURRENT_SOURCE_DIR}/patches/psv_platform.cpp)
set(PSV_PLATFORM_TARGET ${BOREALIS_LIBRARY}/lib/platforms/psv/psv_platform.cpp)
if(EXISTS ${PSV_PLATFORM_PATCH})
    message(STATUS "Applying patch: psv_platform.cpp")
    configure_file(${PSV_PLATFORM_PATCH} ${PSV_PLATFORM_TARGET} COPYONLY)
else()
    message(FATAL_ERROR "Patch file not found: ${PSV_PLATFORM_PATCH}")
endif()

# Borealis fontstash patch (all platforms): caches per-glyph kerning advances
# so text layout doesn't re-query FreeType for every character every frame.
# Speeds up all label/title rendering. Purely additive to the bundled copy.
set(FONTSTASH_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/fontstash.h)
set(FONTSTASH_TARGET ${BOREALIS_LIBRARY}/include/borealis/extern/nanovg/fontstash.h)
if(EXISTS ${FONTSTASH_PATCH})
    message(STATUS "Applying patch: fontstash.h")
    configure_file(${FONTSTASH_PATCH} ${FONTSTASH_TARGET} COPYONLY)
else()
    message(FATAL_ERROR "Patch file not found: ${FONTSTASH_PATCH}")
endif()

# Borealis nanovg patch (all platforms): adds nvgTextBatchBegin/End so a run of
# nvgText calls sharing identical state flush as a single render call. Purely
# additive to the bundled copy.
set(NANOVG_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/nanovg.c)
set(NANOVG_TARGET ${BOREALIS_LIBRARY}/lib/extern/nanovg/nanovg.c)
if(EXISTS ${NANOVG_PATCH})
    message(STATUS "Applying patch: nanovg.c")
    configure_file(${NANOVG_PATCH} ${NANOVG_TARGET} COPYONLY)
else()
    message(FATAL_ERROR "Patch file not found: ${NANOVG_PATCH}")
endif()

# Borealis GXM nanovg utils patch (PS Vita): drops the
# SCE_GXM_ERROR_INVALID_AUXILIARY_SURFACE case from gxmnvg__easy_strerror(). That
# enum value is not declared in current vitasdk, so the bundled header fails to
# compile (psv_video.cpp). The switch has a default case, so removing it is safe.
set(GXM_UTILS_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/nanovg_gxm_utils.h)
set(GXM_UTILS_TARGET ${BOREALIS_LIBRARY}/include/borealis/extern/nanovg/nanovg_gxm_utils.h)
if(EXISTS ${GXM_UTILS_PATCH})
    message(STATUS "Applying patch: nanovg_gxm_utils.h")
    configure_file(${GXM_UTILS_PATCH} ${GXM_UTILS_TARGET} COPYONLY)
else()
    message(FATAL_ERROR "Patch file not found: ${GXM_UTILS_PATCH}")
endif()

# Add Borealis library subdirectory
add_subdirectory(${BOREALIS_LIBRARY})

# ---------------------------------------------------------------------------
# libdatachannel: WebRTC data channels (ICE via libjuice, SCTP via usrsctp,
# DTLS via the mbedTLS already shipped with this app). Used for Music
# Assistant remote access (signaling.music-assistant.io + P2P data channels).
set(NO_MEDIA ON CACHE BOOL "" FORCE)       # no RTP/SRTP audio-video transport
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)   # we have our own WebSocket client
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# usrsctp: userspace AF_CONN mode only (no kernel INET sockets on Vita)
set(sctp_inet OFF CACHE BOOL "" FORCE)
set(sctp_inet6 OFF CACHE BOOL "" FORCE)
set(sctp_build_programs OFF CACHE BOOL "" FORCE)
set(sctp_build_shared_lib OFF CACHE BOOL "" FORCE)
set(sctp_werror OFF CACHE BOOL "" FORCE)

# usrsctp patch (must precede add_subdirectory so usrsctp's own CMake
# check_struct_has_member sees the patched header): struct sockaddr_conn must
# include sconn_len on the Vita. vitasdk's struct sockaddr is BSD-style (sa_len
# at offset 0, sa_family at offset 1), so usrsctp is built HAVE_SA_LEN and reads
# the address family at offset 1. Without sconn_len, sockaddr_conn puts
# sconn_family at offset 0 and usrsctp_bind() reads family 0 -> EAFNOSUPPORT
# (errno 106, seen on-device). Add __vita__ to the platform list that gives
# sockaddr_conn an sconn_len field.
set(USRSCTP_H_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/usrsctp_usrsctp_h.h)
set(USRSCTP_H_TARGET ${CMAKE_CURRENT_SOURCE_DIR}/lib/libdatachannel/deps/usrsctp/usrsctplib/usrsctp.h)
if(EXISTS ${USRSCTP_H_PATCH})
    message(STATUS "Applying patch: usrsctp.h (sockaddr_conn sconn_len on Vita)")
    configure_file(${USRSCTP_H_PATCH} ${USRSCTP_H_TARGET} COPYONLY)
else()
    message(FATAL_ERROR "Patch file not found: ${USRSCTP_H_PATCH}")
endif()

add_subdirectory(lib/libdatachannel EXCLUDE_FROM_ALL)

# Vita compatibility shims for the vendored deps (see files for details)
set(VITA_RTC_COMPAT ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/vita/compat)
if(TARGET juice-static)
    # Compat headers (<ifaddrs.h>, <net/if.h>, <sys/ioctl.h>) + endianness and
    # IPv6 constants that vitasdk newlib lacks
    target_include_directories(juice-static BEFORE PRIVATE ${VITA_RTC_COMPAT})
    target_compile_options(juice-static PRIVATE
        -include ${VITA_RTC_COMPAT}/vita_juice_compat.h)
    # The MA gateway (aiortc in Docker) trickles dozens of host candidates -
    # one per virtual interface - overflowing libjuice's default arrays
    # (ICE_MAX_CANDIDATES_COUNT 30 and everything derived from it). On-device
    # logs showed hundreds of "maximum number of candidates" rejections, and
    # relay candidates being dropped. ~500B per slot; 128 ~= 64KB, fine.
    target_compile_definitions(juice-static PRIVATE ICE_MAX_CANDIDATES_COUNT=128)
endif()
if(TARGET usrsctp)
    # newlib needs BSD visibility + base headers force-included first
    target_compile_options(usrsctp PRIVATE -include ${VITA_RTC_COMPAT}/vita_bsd_types.h)
    target_include_directories(usrsctp BEFORE PRIVATE ${VITA_RTC_COMPAT})
    # sockaddr_conn now carries sconn_len on Vita (see usrsctp.h patch above)
    target_compile_definitions(usrsctp PRIVATE HAVE_SCONN_LEN)
endif()
if(TARGET datachannel-static)
    # Compat headers (<net/if.h>, <ifaddrs.h>, <sys/ioctl.h>) + socket flags
    # (MSG_NOSIGNAL) that newlib doesn't provide
    target_include_directories(datachannel-static BEFORE PRIVATE ${VITA_RTC_COMPAT})
    target_compile_options(datachannel-static PRIVATE
        -include ${VITA_RTC_COMPAT}/vita_datachannel_compat.h)
    # sockaddr_conn carries sconn_len on Vita, so SctpTransport::getSockAddrConn
    # must set it (its guard is #ifdef HAVE_SCONN_LEN); usrsctp's own CMake sets
    # this for the usrsctp target but it does not propagate to this one.
    target_compile_definitions(datachannel-static PRIVATE HAVE_SCONN_LEN)
endif()

# vitasdk links libpthread whole-archive via the toolchain already; the deps'
# find_package(Threads) adds -pthread, which vitasdk's gcc ALSO expands to
# whole-archive libpthread at link -> every pthread symbol defined twice.
# Scrub the Threads usage from the vendored targets (pthread still resolves
# through the toolchain, exactly as the rest of the app does).
foreach(_rtc_t IN ITEMS datachannel-static juice-static usrsctp)
    if(TARGET ${_rtc_t})
        foreach(_prop IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(_libs ${_rtc_t} ${_prop})
            if(_libs)
                list(REMOVE_ITEM _libs Threads::Threads -pthread pthread)
                string(REPLACE "$<LINK_ONLY:Threads::Threads>" "" _libs "${_libs}")
                set_target_properties(${_rtc_t} PROPERTIES ${_prop} "${_libs}")
            endif()
        endforeach()
        # The deps force PIC on, but -fPIC emits GOT-relative relocations
        # (R_ARM_BASE_PREL) that vita-elf-create cannot convert. Vita homebrew
        # is always non-PIC.
        set_target_properties(${_rtc_t} PROPERTIES POSITION_INDEPENDENT_CODE OFF)
    endif()
endforeach()

# usrsctp patch: its RNG selection #errors on unknown platforms; add a Vita
# branch backed by sceKernelGetRandomNumber (exported by SceLibKernel).
set(USRSCTP_ENV_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/usrsctp_user_environment.c)
set(USRSCTP_ENV_TARGET ${CMAKE_CURRENT_SOURCE_DIR}/lib/libdatachannel/deps/usrsctp/usrsctplib/user_environment.c)
if(EXISTS ${USRSCTP_ENV_PATCH})
    message(STATUS "Applying patch: usrsctp user_environment.c (Vita RNG)")
    configure_file(${USRSCTP_ENV_PATCH} ${USRSCTP_ENV_TARGET} COPYONLY)
else()
    message(FATAL_ERROR "Patch file not found: ${USRSCTP_ENV_PATCH}")
endif()

# libdatachannel patch: its mbedTLS DTLS code sets SRTP protection profiles
# unconditionally, which requires MBEDTLS_SSL_DTLS_SRTP; the patch guards those
# spots so either mbedTLS build compiles. NOTE: negotiating a DTLS-SRTP profile
# is REQUIRED in practice even though we build NO_MEDIA - aiortc-based peers
# (the MA remote-access gateway) fail the transport right after the DTLS
# handshake if no profile was negotiated. CI therefore rebuilds mbedTLS with
# MBEDTLS_SSL_DTLS_SRTP enabled (see .github/workflows/build-vpk.yml).
set(DC_DTLS_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/libdatachannel_dtlstransport.cpp)
set(DC_DTLS_TARGET ${CMAKE_CURRENT_SOURCE_DIR}/lib/libdatachannel/src/impl/dtlstransport.cpp)
if(EXISTS ${DC_DTLS_PATCH})
    message(STATUS "Applying patch: libdatachannel dtlstransport.cpp (no DTLS-SRTP)")
    configure_file(${DC_DTLS_PATCH} ${DC_DTLS_TARGET} COPYONLY)
else()
    message(FATAL_ERROR "Patch file not found: ${DC_DTLS_PATCH}")
endif()

# libdatachannel patch: certificate generation enables DRBG prediction
# resistance, which reseeds from the slow Vita entropy source on every random
# call - measured ~30s per certificate on device. PR off (mbedTLS default).
set(DC_CERT_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/libdatachannel_certificate.cpp)
set(DC_CERT_TARGET ${CMAKE_CURRENT_SOURCE_DIR}/lib/libdatachannel/src/impl/certificate.cpp)
if(EXISTS ${DC_CERT_PATCH})
    message(STATUS "Applying patch: libdatachannel certificate.cpp (no DRBG prediction resistance)")
    configure_file(${DC_CERT_PATCH} ${DC_CERT_TARGET} COPYONLY)
else()
    message(FATAL_ERROR "Patch file not found: ${DC_CERT_PATCH}")
endif()

# libjuice patch: its RNG has no Vita source and falls back to rand();
# add a random_bytes() branch backed by sceKernelGetRandomNumber.
set(JUICE_RANDOM_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/libjuice_random.c)
set(JUICE_RANDOM_TARGET ${CMAKE_CURRENT_SOURCE_DIR}/lib/libdatachannel/deps/libjuice/src/random.c)
if(EXISTS ${JUICE_RANDOM_PATCH})
    message(STATUS "Applying patch: libjuice random.c (Vita kernel RNG)")
    configure_file(${JUICE_RANDOM_PATCH} ${JUICE_RANDOM_TARGET} COPYONLY)
else()
    message(FATAL_ERROR "Patch file not found: ${JUICE_RANDOM_PATCH}")
endif()

# libjuice DIAGNOSTIC patch (temporary): log every received UDP datagram to
# localize why juice stops receiving after the DTLS handshake completes.
set(JUICE_UDP_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/libjuice_udp.c)
set(JUICE_UDP_TARGET ${CMAKE_CURRENT_SOURCE_DIR}/lib/libdatachannel/deps/libjuice/src/udp.c)
if(EXISTS ${JUICE_UDP_PATCH})
    message(STATUS "Applying patch: libjuice udp.c (recv logging)")
    configure_file(${JUICE_UDP_PATCH} ${JUICE_UDP_TARGET} COPYONLY)
else()
    message(FATAL_ERROR "Patch file not found: ${JUICE_UDP_PATCH}")
endif()

# libjuice FIX: vitasdk poll() does not wake on incoming socket data during a
# long wait, so once ICE goes idle (poll timeout jumps from ~10ms to seconds)
# incoming packets sit unread and the SCTP handshake stalls. Cap the poll
# timeout in every conn backend so the socket is drained promptly.
foreach(_j conn_poll conn_mux conn_thread)
    set(_src ${CMAKE_CURRENT_SOURCE_DIR}/patches/libjuice_${_j}.c)
    set(_dst ${CMAKE_CURRENT_SOURCE_DIR}/lib/libdatachannel/deps/libjuice/src/${_j}.c)
    if(EXISTS ${_src})
        message(STATUS "Applying patch: libjuice ${_j}.c (poll timeout cap)")
        configure_file(${_src} ${_dst} COPYONLY)
    else()
        message(FATAL_ERROR "Patch file not found: ${_src}")
    endif()
endforeach()

# libdatachannel patch: SctpTransport::doRecv() puts a 64KB receive buffer on
# the stack. On the Vita, pthread stacks are heap-allocated, so that frame
# overruns the SCTP worker thread's stack into adjacent heap chunks and
# corrupts the global heap (crash: main thread aborts in free() right after the
# SCTP handshake). Move the buffer to a thread_local heap allocation.
set(DC_SCTP_PATCH  ${CMAKE_CURRENT_SOURCE_DIR}/patches/libdatachannel_sctptransport.cpp)
set(DC_SCTP_TARGET ${CMAKE_CURRENT_SOURCE_DIR}/lib/libdatachannel/src/impl/sctptransport.cpp)
if(EXISTS ${DC_SCTP_PATCH})
    message(STATUS "Applying patch: libdatachannel sctptransport.cpp (off-stack recv buffer)")
    configure_file(${DC_SCTP_PATCH} ${DC_SCTP_TARGET} COPYONLY)
else()
    message(FATAL_ERROR "Patch file not found: ${DC_SCTP_PATCH}")
endif()

# Application source files
set(APP_SOURCES
    # Main entry
    src/main.cpp

    # Application
    src/app/application.cpp
    src/app/ma_client.cpp
    src/app/sendspin_client.cpp
    src/app/websocket_client.cpp
    src/app/webrtc_client.cpp
    src/app/music_queue.cpp

    # Activities
    src/activity/main_activity.cpp
    src/activity/login_activity.cpp
    src/activity/player_activity.cpp

    # Views
    src/view/home_tab.cpp
    src/view/library_tab.cpp
    src/view/library_section_tab.cpp
    src/view/search_tab.cpp
    src/view/settings_tab.cpp
    src/view/debug_tab.cpp
    src/view/media_detail_view.cpp
    src/view/media_item_cell.cpp
    src/view/recycling_grid.cpp
    src/view/music_tab.cpp
    src/view/now_playing_view.cpp
    src/view/progress_dialog.cpp
    src/view/overlay_dialog.cpp
    src/view/horizontal_scroll_row.cpp
    src/view/queue_tab.cpp

    # Player
    src/player/mpv_player.cpp

    # Utils
    src/utils/http_client.cpp
    src/utils/image_loader.cpp
    src/utils/audio_stream_server.cpp
    src/utils/vita_stubs.c

    # Vita platform shims for libdatachannel deps
    src/platform/vita/compat/vita_ifaddrs.c
)

# Include directories
set(APP_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${BOREALIS_LIBRARY}/include
    ${BOREALIS_LIBRARY}/include/borealis/extern
)

# Create executable
add_executable(${PROJECT_NAME} ${APP_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE ${APP_INCLUDES})

# Wrap pthread_create to enforce a minimum 512KB stack size.
# Vita's default pthread stack is only 32KB, which can be too small for
# audio decoder threads.
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--wrap=pthread_create")

# Safety net for the -pthread/whole-archive duplication described above: any
# residual duplicate libpthread members are identical, first-wins is correct.
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--allow-multiple-definition")

# Link libraries - borealis + app platform libs from toolchain
# Order matters: libraries that PROVIDE symbols must come AFTER libraries that NEED them
target_link_libraries(${PROJECT_NAME}
    borealis
    ${APP_PLATFORM_LIB}

    # WebRTC data channels (pulls in juice-static + usrsctp transitively;
    # mbedTLS symbols resolve from the mbedtls libs listed further down)
    datachannel-static

    # External libraries
    curl
    mpv
    ass
    harfbuzz
    fribidi
    avfilter
    avformat
    avcodec
    swscale
    swresample
    avutil
    avcodec
    archive
    zstd
    freetype
    png
    bz2
    lzma
    mbedtls
    mbedcrypto
    mbedx509

    # Vita SDK stubs (provide symbols for above libraries)
    vita2d
    SceNet_stub
    SceNetCtl_stub
    SceHttp_stub
    SceSsl_stub
    ScePgf_stub
    SceRtc_stub
    SceShellSvc_stub
    SceAudiodec_stub
    SceAudio_stub
    SceVshBridge_stub
    SceAppMgr_stub
    SceVideodec_stub
    SceCodecEngine_stub

    # Standard libraries last (m is provided by libstdc++)
    z
    c
)

# Create Vita SELF (UNSAFE NOASLR flags required for homebrew compatibility)
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE NOASLR)

# Create VPK
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}

    # System files
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/pic0.png sce_sys/pic0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml

    # Borealis resources
    FILE ${BOREALIS_DIR}/resources/i18n/en-US/hints.json resources/i18n/en-US/hints.json
    FILE resources/i18n/en-US/main.json resources/i18n/en-US/main.json

    # App resources - XML layouts
    FILE resources/xml/activity/main.xml resources/xml/activity/main.xml
    FILE resources/xml/activity/login.xml resources/xml/activity/login.xml
    FILE resources/xml/activity/player.xml resources/xml/activity/player.xml
    FILE resources/xml/activity/reader.xml resources/xml/activity/reader.xml
    FILE resources/xml/tabs/home.xml resources/xml/tabs/home.xml
    FILE resources/xml/tabs/library.xml resources/xml/tabs/library.xml
    FILE resources/xml/tabs/search.xml resources/xml/tabs/search.xml
    FILE resources/xml/tabs/settings.xml resources/xml/tabs/settings.xml
    FILE resources/xml/tabs/extensions.xml resources/xml/tabs/extensions.xml
    FILE resources/xml/view/media_detail.xml resources/xml/view/media_detail.xml
    FILE resources/xml/view/media_item.xml resources/xml/view/media_item.xml
    FILE resources/xml/view/chapter_item.xml resources/xml/view/chapter_item.xml
    FILE resources/xml/view/manga_detail.xml resources/xml/view/manga_detail.xml
    FILE resources/xml/view/manga_item.xml resources/xml/view/manga_item.xml

    # Icons
    FILE resources/icons/a.png resources/icons/a.png
    FILE resources/icons/az.png resources/icons/az.png
    FILE resources/icons/back.png resources/icons/back.png
    FILE resources/icons/book-arrow-down.png resources/icons/book-arrow-down.png
    FILE resources/icons/book-multiple.png resources/icons/book-multiple.png
    FILE resources/icons/book-open-page-variant.png resources/icons/book-open-page-variant.png
    FILE resources/icons/checkbox.png resources/icons/checkbox.png
    FILE resources/icons/checkbox_checked.png resources/icons/checkbox_checked.png
    FILE resources/icons/checkbox_crossed.png resources/icons/checkbox_crossed.png
    FILE resources/icons/circle.png resources/icons/circle.png
    FILE resources/icons/cross.png resources/icons/cross.png
    FILE resources/icons/download.png resources/icons/download.png
    FILE resources/icons/filter-menu-outline.png resources/icons/filter-menu-outline.png
    FILE resources/icons/format-list-group.png resources/icons/format-list-group.png
    FILE resources/icons/hide.png resources/icons/hide.png
    FILE resources/icons/history.png resources/icons/history.png
    FILE resources/icons/hot.png resources/icons/hot.png
    FILE resources/icons/import.png resources/icons/import.png
    FILE resources/icons/left.png resources/icons/left.png
    FILE resources/icons/menu.png resources/icons/menu.png
    FILE resources/icons/mini_star.png resources/icons/mini_star.png
    FILE resources/icons/options.png resources/icons/options.png
    FILE resources/icons/radio.png resources/icons/radio.png
    FILE resources/icons/radio_checked.png resources/icons/radio_checked.png
    FILE resources/icons/refresh.png resources/icons/refresh.png
    FILE resources/icons/right.png resources/icons/right.png
    FILE resources/icons/search.png resources/icons/search.png
    FILE resources/icons/show.png resources/icons/show.png
    FILE resources/icons/sort-1-9.png resources/icons/sort-1-9.png
    FILE resources/icons/sort-9-1.png resources/icons/sort-9-1.png
    FILE resources/icons/sort-calendar-ascending.png resources/icons/sort-calendar-ascending.png
    FILE resources/icons/sort-calendar-descending.png resources/icons/sort-calendar-descending.png
    FILE resources/icons/sort-clock-ascending.png resources/icons/sort-clock-ascending.png
    FILE resources/icons/sort-clock-descending.png resources/icons/sort-clock-descending.png
    FILE resources/icons/star.png resources/icons/star.png
    FILE resources/icons/tag.png resources/icons/tag.png
    FILE resources/icons/web.png resources/icons/web.png
    FILE resources/icons/za.png resources/icons/za.png
    FILE resources/icons/play.png resources/icons/play.png
    FILE resources/icons/pause.png resources/icons/pause.png
    FILE resources/icons/skip-next.png resources/icons/skip-next.png
    FILE resources/icons/skip-previous.png resources/icons/skip-previous.png
    FILE resources/icons/subtitles.png resources/icons/subtitles.png
    FILE resources/icons/video-image.png resources/icons/video-image.png
    FILE resources/icons/translate.png resources/icons/translate.png
    FILE resources/icons/fast-forward-5.png resources/icons/fast-forward-5.png
    FILE resources/icons/fast-forward-10.png resources/icons/fast-forward-10.png
    FILE resources/icons/fast-forward-15.png resources/icons/fast-forward-15.png
    FILE resources/icons/fast-forward-30.png resources/icons/fast-forward-30.png
    FILE resources/icons/fast-forward-45.png resources/icons/fast-forward-45.png
    FILE resources/icons/fast-forward-60.png resources/icons/fast-forward-60.png
    FILE resources/icons/rewind-5.png resources/icons/rewind-5.png
    FILE resources/icons/rewind-10.png resources/icons/rewind-10.png
    FILE resources/icons/rewind-15.png resources/icons/rewind-15.png
    FILE resources/icons/rewind-30.png resources/icons/rewind-30.png
    FILE resources/icons/rewind-45.png resources/icons/rewind-45.png
    FILE resources/icons/rewind-60.png resources/icons/rewind-60.png
    FILE resources/icons/shuffle-variant.png resources/icons/shuffle-variant.png
    FILE resources/icons/shuffle-disabled.png resources/icons/shuffle-disabled.png
    FILE resources/icons/repeat.png resources/icons/repeat.png
    FILE resources/icons/repeat-once.png resources/icons/repeat-once.png
    FILE resources/icons/repeat-off.png resources/icons/repeat-off.png

    # Images
    FILE resources/images/circle_button.png resources/images/circle_button.png
    FILE resources/images/cross_button.png resources/images/cross_button.png
    FILE resources/images/dpad.png resources/images/dpad.png
    FILE resources/images/gradient.png resources/images/gradient.png
    FILE resources/images/l_button.png resources/images/l_button.png
    FILE resources/images/logo-small.png resources/images/logo-small.png
    FILE resources/images/logo.png resources/images/logo.png
    FILE resources/images/r_button.png resources/images/r_button.png
    FILE resources/images/select_button.png resources/images/select_button.png
    FILE resources/images/square_button.png resources/images/square_button.png
    FILE resources/images/start_button.png resources/images/start_button.png
    FILE resources/images/triangle_button.png resources/images/triangle_button.png

    # Fonts (using simpler DejaVu fonts for Vita compatibility)
    FILE resources/font/font.ttf resources/font/font.ttf
    FILE resources/font/icon.ttf resources/font/icon.ttf
    FILE resources/font/switch_font.ttf resources/font/switch_font.ttf
    FILE resources/font/switch_icons.ttf resources/font/switch_icons.ttf
    FILE resources/material/MaterialIcons-Regular.ttf resources/material/MaterialIcons-Regular.ttf
)
