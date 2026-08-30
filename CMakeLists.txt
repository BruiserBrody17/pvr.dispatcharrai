cmake_minimum_required(VERSION 3.15)
project(pvr.dispatcharr)

# ---------------------------------------------------------------------------
# This follows the standard out-of-tree Kodi binary-addon layout: it expects
# to be built via Kodi's own addon build harness
# (<kodi-source>/tools/depends/target/binary-addons), which supplies
# find_package(Kodi ...) and the build_addon() macro. See docs/BUILDING.md.
# ---------------------------------------------------------------------------

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}")

find_package(Kodi REQUIRED)
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
)

set(DISPATCHARR_HEADERS
    src/PVRDispatcharr.h
    src/DispatcharrClient.h
    src/XmlTvParser.h
)

set(DEPLIBS
    ${CURL_LIBRARIES}
    nlohmann_json::nlohmann_json
    pugixml::pugixml
)

build_addon(pvr.dispatcharr DISPATCHARR DEPLIBS)

include(CPack)
