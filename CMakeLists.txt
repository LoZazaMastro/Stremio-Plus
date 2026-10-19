cmake_minimum_required(VERSION 3.20)
project(stremio-ps5 VERSION 0.5.18 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
option(PS5_NATIVE "Build native PS5 library for native/build.sh" OFF)
option(STREMIO_USE_LIBVLC "Use the static VLC playback engine" ${PS5_NATIVE})
set(PS5_OPENGL_PREFIX "" CACHE PATH "ps5-opengl SDK directory")
find_package(SDL2 REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
if(STREMIO_USE_LIBVLC)
    include(cmake/vlc.cmake)
    # The application and VLC must use the same FFmpeg headers and archives.
    add_library(stremio_ffmpeg INTERFACE)
    target_include_directories(stremio_ffmpeg INTERFACE "${STREMIO_VLC_PREFIX}/include")
    foreach(component avformat avcodec avutil swresample swscale)
        set(archive "${STREMIO_VLC_PREFIX}/lib/lib${component}.a")
        if(NOT EXISTS "${archive}")
            message(FATAL_ERROR "Missing VLC's FFmpeg dependency: ${archive}")
        endif()
        target_link_libraries(stremio_ffmpeg INTERFACE "${archive}")
    endforeach()
    set(STREMIO_FFMPEG_TARGET stremio_ffmpeg)
else()
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswresample libswscale)
    set(STREMIO_FFMPEG_TARGET PkgConfig::FFMPEG)
endif()
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
set(HUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ps5-homebrew-ui")
file(GLOB_RECURSE HUI_SOURCES CONFIGURE_DEPENDS "${HUI_DIR}/src/gfx/*.cpp" "${HUI_DIR}/src/ui/*.cpp" "${HUI_DIR}/src/core/*.cpp" "${HUI_DIR}/src/audio/*.cpp")
# Playback uses FFmpeg. The gallery's background-music decoder is unused.
list(FILTER HUI_SOURCES EXCLUDE REGEX "/audio/music\\.cpp$")
set(STREMIO_SOURCES
    src/main.cpp src/app.cpp src/app_board.cpp src/app_browse.cpp src/app_detail.cpp src/app_preview_meta.cpp
    src/app_settings.cpp src/app_preferences.cpp src/app_watch.cpp src/home_ui.cpp src/download_directory.cpp
    src/calendar_date.cpp src/controller_ambient.cpp src/ui_language.cpp
    src/media_remote.cpp src/display_refresh.cpp
    src/app_downloads.cpp src/download_manager.cpp src/download_transfer.cpp src/download_text.cpp
    src/growing_file_stream.cpp
    src/artcache.cpp src/bidi.cpp src/http.cpp src/hwdec_ps5.cpp src/player.cpp src/player_yuv.cpp
    src/netstream.cpp src/stremio.cpp src/stream_presentation.cpp src/subtitles.cpp src/tasks.cpp src/util.cpp
    src/yuv_convert.cpp src/video_gl.cpp src/video_dovi.cpp src/texture_cache.cpp src/diagnostics.cpp src/display_output.cpp
    src/torrent/bencode.cpp src/torrent/engine.cpp src/torrent/torrent_stream.cpp
    ${HUI_SOURCES})
if(STREMIO_USE_LIBVLC)
    list(REMOVE_ITEM STREMIO_SOURCES src/player.cpp)
    list(APPEND STREMIO_SOURCES src/player_vlc.cpp src/player_vlc_http.cpp src/player_vlc_subtitles.cpp
        src/player_vlc_copy.cpp src/player_vlc_dovi.cpp
        "${STREMIO_VLC_STATIC}/static_modules.c")
    if(PS5)
        list(APPEND STREMIO_SOURCES vendor/vlc-ps5/modules/ps5vdec.c
            vendor/vlc-ps5/modules/ps5http.c src/player_vlc_vp9.cpp
            src/player_vlc_passthrough.cpp src/player_iec61937.cpp)
    endif()
endif()
if(PS5 AND PS5_NATIVE)
    if(NOT EXISTS "${PS5_OPENGL_PREFIX}/include/GL/glcorearb.h")
        message(FATAL_ERROR "Set PS5_OPENGL_PREFIX to the ps5-opengl SDK")
    endif()
    add_library(stremio STATIC ${STREMIO_SOURCES} "${HUI_DIR}/src/platform/ps5/display_egl.cpp" "${HUI_DIR}/src/platform/ps5/pad.cpp" "${HUI_DIR}/src/platform/ps5/system.cpp")
    target_sources(stremio PRIVATE native/ps5_storage.cpp native/filesystem/elevation.cpp
        native/download_writer/client.cpp)
    target_include_directories(stremio BEFORE PRIVATE "${PS5_OPENGL_PREFIX}/include")
    target_compile_definitions(stremio PRIVATE PLATFORM_PS5=1 PLATFORM_PS5_NATIVE=1 HAVE_DHT=1 HAVE_UPNP=1 MINIUPNP_STATICLIB HAVE_FRIBIDI=1)
    # Line tables stay in the matching diagnostic ELF. The native converter
    # excludes non-allocated debug sections from the installed executable.
    target_compile_options(stremio PRIVATE -gline-tables-only)
elseif(PS5)
    message(FATAL_ERROR "Use native/build.sh to build the PS5 title")
else()
    add_executable(stremio ${STREMIO_SOURCES} src/preview_fixture.cpp "${HUI_DIR}/host/platform_host.cpp")
    find_library(STREMIO_GL_LIBRARY NAMES GL OpenGL REQUIRED)
    find_library(STREMIO_EGL_LIBRARY NAMES EGL REQUIRED)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(stremio PRIVATE ${STREMIO_GL_LIBRARY} ${STREMIO_EGL_LIBRARY} OpenSSL::Crypto)
    find_library(DHT_LIBRARY dht)
    if(DHT_LIBRARY)
        target_compile_definitions(stremio PRIVATE HAVE_DHT=1)
        target_link_libraries(stremio PRIVATE ${DHT_LIBRARY})
    endif()
    pkg_check_modules(FRIBIDI IMPORTED_TARGET fribidi)
    if(FRIBIDI_FOUND)
        target_compile_definitions(stremio PRIVATE HAVE_FRIBIDI=1)
        target_link_libraries(stremio PRIVATE PkgConfig::FRIBIDI)
    endif()
endif()
target_include_directories(stremio PRIVATE src native third_party "${HUI_DIR}/src")
if(STREMIO_USE_LIBVLC)
    target_compile_definitions(stremio PRIVATE STREMIO_USE_LIBVLC=1)
    target_link_libraries(stremio PRIVATE stremio_vlc)
endif()
target_compile_definitions(stremio PRIVATE GL_GLEXT_PROTOTYPES=1 STREMIO_VERSION="${PROJECT_VERSION}" STREMIO_TITLE_ID="PPSA74126")
target_compile_options(stremio PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter)
target_link_libraries(stremio PRIVATE SDL2::SDL2 ${STREMIO_FFMPEG_TARGET} PkgConfig::CURL PkgConfig::WEBP PkgConfig::ZLIB Threads::Threads m)
