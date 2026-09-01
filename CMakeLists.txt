cmake_minimum_required(VERSION 3.15)
project(pvr.dispatcharrai)

# ---------------------------------------------------------------------------
# This follows the standard out-of-tree Kodi binary-addon layout: it expects
# to be built via Kodi's own addon build harness
# (<kodi-source>/tools/depends/target/binary-addons), which supplies
# find_package(Kodi ...) and the build_addon() macro. See docs/BUILDING.md.
# ---------------------------------------------------------------------------

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}")

find_package(Kodi REQUIRED)

# Skip FindCURL's config-mode delegation: some prebuilt curl packages (e.g.
# Kodi's own Windows dependency) ship a CURLConfig.cmake whose transitive
# link dependencies (nghttp2) bake in an absolute path from the machine that
# built the package, which doesn't exist here. Plain module-mode find_path/
# find_library discovery avoids that.
set(CURL_NO_CURL_CMAKE ON)
find_package(CURL REQUIRED)

include(FetchContent)

# Header-only JSON library (vendored at configure time, not committed to
# this repo, to avoid bundling/redistributing third-party source).
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(json)

# Small XML parser used to read the XMLTV EPG document Dispatcharr serves
# at /output/epg.
FetchContent_Declare(
  pugixml
  GIT_REPOSITORY https://github.com/zeux/pugixml.git
  GIT_TAG        v1.14
)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(pugixml)

# BUILD_SHARED_LIBS is a CACHE variable, so the OFF forced above for pugixml
# would otherwise also apply to build_addon()'s own add_library() call below,
# silently producing a static pvr.dispatcharrai.a instead of the shared object
# Kodi actually loads.
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)

include_directories(
  ${KODI_INCLUDE_DIR}/.. # KODI_INCLUDE_DIR already ends in "kodi", but addon sources
                         # #include <kodi/...>; pvr.hts's CMakeLists does the same.
  ${CURL_INCLUDE_DIRS}
)

set(DISPATCHARR_SOURCES
    src/addon.cpp
    src/PVRDispatcharr.cpp
    src/DispatcharrClient.cpp
    src/XmlTvParser.cpp
    src/WebSocketClient.cpp
)

set(DISPATCHARR_HEADERS
    src/PVRDispatcharr.h
    src/DispatcharrClient.h
    src/XmlTvParser.h
    src/WebSocketClient.h
)

# Modern FindCURL provides the CURL::libcurl imported target; ${CURL_LIBRARIES}
# isn't reliably populated when CURL is found via a CURLConfig.cmake package
# (e.g. the prebuilt Windows dependency), so prefer the target when available.
if(TARGET CURL::libcurl)
  set(CURL_DEPLIB CURL::libcurl)
else()
  set(CURL_DEPLIB ${CURL_LIBRARIES})
endif()

set(DEPLIBS
    ${CURL_DEPLIB}
    nlohmann_json::nlohmann_json
    pugixml::pugixml
)

# WebSocketClient.cpp calls select()/SOCKET APIs directly (see its own
# comment for why: curl's native WebSocket support needs curl >= 7.86,
# newer than the prebuilt Windows curl dependency this addon links
# against). curl handles its own Winsock linkage internally and doesn't
# propagate it to consumers, so this addon needs its own explicit link
# against ws2_32 now that it also calls raw Winsock functions itself.
if(WIN32)
  list(APPEND DEPLIBS ws2_32)
endif()

# Unlike Linux (system libcurl.so, resolved against whatever the user's distro
# already has) and Kodi's own Windows build (which bundles libcurl.dll itself,
# so it may already be present), a standalone Windows addon zip can't assume
# the runtime DLLs this was linked against are already on the target machine.
# Ship the exact ones from the same dependency tree Kodi's own headers came
# from, so the addon loads regardless of what the host Kodi install carries.
if(WIN32)
  get_filename_component(DISPATCHARR_DEPENDS_ROOT "${KODI_INCLUDE_DIR}/../.." ABSOLUTE)
  set(DISPATCHARR_ADDITIONAL_BINARY
      ${DISPATCHARR_DEPENDS_ROOT}/bin/libcurl.dll
      ${DISPATCHARR_DEPENDS_ROOT}/bin/zlib.dll
  )
endif()

build_addon(pvr.dispatcharrai DISPATCHARR DEPLIBS)

include(CPack)
