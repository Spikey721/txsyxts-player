cmake_minimum_required(VERSION 3.14)
project(txsyxts VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── fetch dependencies ──────────────────────────────────────────
include(FetchContent)

# FTXUI — modern C++ TUI
FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG v6.1.9
)
FetchContent_MakeAvailable(ftxui)

# nlohmann/json — header-only JSON
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# ── find system libraries ───────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED mpv)
pkg_check_modules(CURL REQUIRED libcurl)

# ── build txsyxts ───────────────────────────────────────────────
add_executable(txsyxts
  src/main.cpp
  src/config.cpp
  src/spotify.cpp
  src/youtube.cpp
  src/player.cpp
  src/queue.cpp
  src/commands.cpp
  src/app.cpp
  src/ascii_art.cpp
  src/local_playlists.cpp
)

target_include_directories(txsyxts PRIVATE
  ${MPV_INCLUDE_DIRS}
  ${CURL_INCLUDE_DIRS}
)

target_link_libraries(txsyxts PRIVATE
  ftxui::screen
  ftxui::dom
  ftxui::component
  nlohmann_json::nlohmann_json
  ${MPV_LIBRARIES}
  ${CURL_LIBRARIES}
  pthread
)

target_compile_options(txsyxts PRIVATE -Wall -Wextra -O2)

# ── login helper (webkit2gtk) ───────────────────────────────────
pkg_check_modules(WEBKIT2GTK webkit2gtk-4.1)
pkg_check_modules(GTK3 gtk+-3.0)

if(WEBKIT2GTK_FOUND AND GTK3_FOUND)
  message(STATUS "Building login helper (webkit2gtk found)")
  add_executable(txsyxts-login
    src/login_window.cpp
    src/config.cpp
  )
  target_include_directories(txsyxts-login PRIVATE
    ${WEBKIT2GTK_INCLUDE_DIRS}
    ${GTK3_INCLUDE_DIRS}
  )
  target_link_libraries(txsyxts-login PRIVATE
    nlohmann_json::nlohmann_json
    ${WEBKIT2GTK_LIBRARIES}
    ${GTK3_LIBRARIES}
  )
  target_compile_options(txsyxts-login PRIVATE -Wall -O2)
  install(TARGETS txsyxts-login DESTINATION bin)
else()
  message(STATUS "webkit2gtk not found — login helper disabled (manual sp_dc only)")
endif()

# ── install ─────────────────────────────────────────────────────
install(TARGETS txsyxts DESTINATION bin)
