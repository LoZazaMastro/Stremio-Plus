# Static VLC uses the PS5 port's callback outputs and this application's frontend.
set(STREMIO_VLC_ROOT "${CMAKE_SOURCE_DIR}/.deps/vlc" CACHE PATH "Static VLC build root")
if(PS5)
    set(STREMIO_VLC_TARGET ps5)
else()
    set(STREMIO_VLC_TARGET host)
endif()
set(STREMIO_VLC_BUILD "${STREMIO_VLC_ROOT}/build-vlc-${STREMIO_VLC_TARGET}")
set(STREMIO_VLC_PREFIX "${STREMIO_VLC_ROOT}/prefix-${STREMIO_VLC_TARGET}")
set(STREMIO_VLC_STATIC "${STREMIO_VLC_ROOT}/static-${STREMIO_VLC_TARGET}")
foreach(required
    "${STREMIO_VLC_BUILD}/config.h"
    "${STREMIO_VLC_BUILD}/src/.libs/libvlccore.a"
    "${STREMIO_VLC_STATIC}/static_modules.c"
    "${STREMIO_VLC_STATIC}/link.rsp")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Missing ${required}; run tools/setup-vlc.sh first")
    endif()
endforeach()
add_library(stremio_vlc INTERFACE)
target_include_directories(stremio_vlc INTERFACE
    "${STREMIO_VLC_ROOT}/src/vlc-3.0.24/include"
    "${STREMIO_VLC_BUILD}/include"
    "${STREMIO_VLC_BUILD}"
    "${STREMIO_VLC_PREFIX}/include")
if(NOT PS5)
    file(STRINGS "${STREMIO_VLC_STATIC}/link.rsp" STREMIO_VLC_LINK_INPUTS)
    target_link_libraries(stremio_vlc INTERFACE ${STREMIO_VLC_LINK_INPUTS})
endif()
