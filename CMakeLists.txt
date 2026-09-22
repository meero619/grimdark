cmake_minimum_required(VERSION 3.24)
project(grimdark LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
  message(FATAL_ERROR "grimdark must be built with MSVC (it links against the game's MSVC ABI)")
endif()
# Static CRT: the DLL is injected into a process whose own VC runtime is older than this toolset's, and a
# player has no reason to have the 2022 redistributable. Everything here (Detours, SQLite, the core, the
# injector) builds with /MT; prism.dll carries its own runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus)

# --- Microsoft Detours (vendored, built from source) ---
set(DETOURS_SRC ${CMAKE_SOURCE_DIR}/third_party/Detours/src)
add_library(detours STATIC
  ${DETOURS_SRC}/detours.cpp ${DETOURS_SRC}/modules.cpp ${DETOURS_SRC}/disasm.cpp
  ${DETOURS_SRC}/image.cpp ${DETOURS_SRC}/creatwth.cpp)
target_include_directories(detours PUBLIC ${DETOURS_SRC})
target_compile_options(detours PRIVATE /W0)

# --- SQLite (vendored amalgamation, public domain; the rooms dbs are read-only at runtime) ---
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_OMIT_DEPRECATED SQLITE_DQS=0 SQLITE_OMIT_SHARED_CACHE)
target_compile_options(sqlite3 PRIVATE /W0)

# --- Prism (prebuilt SDK release) ---
set(PRISM_SDK ${CMAKE_SOURCE_DIR}/third_party/prism-bin/prism-sdk-v0.18.1)
set(PRISM_BIN ${PRISM_SDK}/windows/x64/dynamic/release)
set(PRISM_LIB ${PRISM_BIN}/lib/prism.lib)
set(PRISM_DLL ${PRISM_BIN}/bin/prism.dll)
if(NOT EXISTS ${PRISM_LIB} OR NOT EXISTS ${PRISM_DLL})
  message(FATAL_ERROR "prism SDK not found under ${PRISM_BIN}")
endif()
get_filename_component(PRISM_DLL_NAME ${PRISM_DLL} NAME)

# --- engine-free core: message building, strings, nav graph, typeahead (unit-tested, no Windows headers) ---
file(GLOB GDCORE_SOURCES src/core/*.cpp)
add_library(gdcore STATIC ${GDCORE_SOURCES})
target_include_directories(gdcore PUBLIC src)

file(GLOB GDCORE_TESTS tests/*.cpp)
add_executable(gdcore_tests ${GDCORE_TESTS})
target_include_directories(gdcore_tests PRIVATE third_party)
target_link_libraries(gdcore_tests PRIVATE gdcore)
add_custom_target(check COMMAND gdcore_tests DEPENDS gdcore_tests)

# --- the injected DLL ---
add_library(grimdark SHARED
  src/dllmain.cpp src/log.cpp src/crash.cpp src/speech.cpp src/hooks.cpp src/textcap.cpp src/devserver.cpp src/audio_mute.cpp src/app.cpp src/screens/main_menu.cpp src/screens/create_character.cpp src/screens/difficulty_select.cpp
  src/audio.cpp src/world.cpp src/screens/in_game.cpp src/screens/message_box.cpp src/screens/pause_menu.cpp
  src/screens/loading.cpp src/screens/tip.cpp src/screens/conversation.cpp src/exe_ui.cpp src/screens/delete_character.cpp src/screens/options.cpp
  src/voice.cpp src/tts_onecore.cpp src/combat.cpp src/casts.cpp src/telegraph.cpp src/settings.cpp src/cues.cpp src/screens/announcements.cpp src/screens/cue_settings.cpp src/screens/mod_menu.cpp src/screens/sound_glossary.cpp src/notify.cpp src/quest_rewards.cpp src/gameapi.cpp src/gameapi_items.cpp src/gameapi_skills.cpp src/gameapi_devotion.cpp src/gameapi_pets.cpp src/screens/pets.cpp src/gameapi_loot.cpp src/screens/loot_filter.cpp src/gameapi_crafting.cpp src/screens/crafting.cpp src/screens/inventor.cpp
  src/screens/codex.cpp src/screens/factions.cpp src/screens/inventory.cpp src/screens/skills.cpp src/screens/quickbar.cpp
  src/screens/modals.cpp src/screens/vendor.cpp src/db.cpp src/rooms.cpp src/sonar.cpp src/hazard.cpp src/screens/riftgate.cpp
  src/screens/list_picker.cpp src/screens/count_prompt.cpp src/screens/reward_list.cpp src/screens/hotbar_manager.cpp src/screens/map_markers.cpp
  src/screens/mod_options.cpp src/version_gate.cpp src/travel.cpp)
# C++/WinRT (OneCore speech in src/tts_onecore.cpp): the projection headers are heavy, and windowsapp.lib's
# api-ms-win-* imports are delay-loaded so a machine without them loses combat speech, not the whole mod.
set_source_files_properties(src/tts_onecore.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
target_include_directories(grimdark PRIVATE src ${PRISM_SDK}/include third_party/miniaudio)
target_link_libraries(grimdark PRIVATE gdcore detours sqlite3 ${PRISM_LIB} user32 ws2_32 ole32 delayimp windowsapp)
target_link_options(grimdark PRIVATE /DELAYLOAD:${PRISM_DLL_NAME}
  /DELAYLOAD:api-ms-win-core-winrt-l1-1-0.dll /DELAYLOAD:api-ms-win-core-winrt-error-l1-1-1.dll /DELAYLOAD:api-ms-win-core-winrt-string-l1-1-0.dll)
target_compile_definitions(grimdark PRIVATE PRISM_DLL_NAME="${PRISM_DLL_NAME}")
add_custom_command(TARGET grimdark POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PRISM_DLL} $<TARGET_FILE_DIR:grimdark>
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:grimdark>/assets)

# --- the rooms databases: built from data/rooms (git-friendly text + compressed grids) by tools/rooms_pack.py ---
# The DLL loads the matching rooms database from next to itself; the packer is stdlib-only Python (no uv, no numpy),
# so CI and a fresh checkout build them without the game or the authoring toolchain. Re-runs when any data file or
# the packer changes. The authoring tools work on unpacked copies in build/rooms/ (docs/rooms.md "Data layout").
find_package(Python3 COMPONENTS Interpreter REQUIRED)
file(GLOB_RECURSE ROOMS_DATA CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/data/rooms/*)
set(ROOMS_DBS ${CMAKE_BINARY_DIR}/assets/rooms_gdx3.db ${CMAKE_BINARY_DIR}/assets/rooms.db ${CMAKE_BINARY_DIR}/assets/rooms_base.db)
add_custom_command(OUTPUT ${ROOMS_DBS}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/rooms_pack.py build --out ${CMAKE_BINARY_DIR}/assets
  DEPENDS ${ROOMS_DATA} ${CMAKE_SOURCE_DIR}/tools/rooms_pack.py ${CMAKE_SOURCE_DIR}/tools/gdmap/roomsdb.py
  COMMENT "packing data/rooms -> expansion and base room databases")
add_custom_target(rooms_dbs ALL DEPENDS ${ROOMS_DBS})
add_dependencies(grimdark rooms_dbs)

# --- the injector ---
add_executable(gdinject src/inject/main.cpp)
target_link_libraries(gdinject PRIVATE psapi)

# --- the player's launcher (console app; finds the Steam install, fakes Steam's environment, injects at start) ---
add_executable(gdlaunch src/launch/main.cpp)
target_include_directories(gdlaunch PRIVATE src)   # core/game_settings.h (header-only)
target_link_libraries(gdlaunch PRIVATE shell32 ole32 advapi32)
