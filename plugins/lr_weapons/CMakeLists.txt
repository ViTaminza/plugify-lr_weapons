# lr_weapons — Plugify C++ port of the Pisex LR module "[LR] ExStats Weapons"
# (the original LR_ExStatsWeapons module, Metamod plugin lr_weapons).
#
# Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no guard library. The engine reads are
# the "attacker"/"weapon" fields of the game event the levels_ranks kill hook hands over and the
# client queries of src/plugin.cpp; the map gate and the s2sdk binding checks live there too. The
# exstats_weapons.ini KeyValues reader is the plugin's own src/weapons_core.cpp. plgdb is the
# asynchronous database layer (the original's `<lr_table>_weapons` table).
#
#   lr_weapons_core       engine-free core (the KV1 reader + exstats_weapons.ini model, per-slot
#                         counters, SQL; also used by tests/lr_weapons_tests.cpp)
#   plugify-lr_weapons    the plugin shared library (entry "bin/lr_weapons")

set(LR_WEAPONS_VERSION "1.1.0")

# --- core ---------------------------------------------------------------------
add_library(lr_weapons_core STATIC src/weapons_core.cpp)
target_include_directories(lr_weapons_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# Engine-free: the KV1 reader and the SQL builders have no plugify/s2sdk dependency; plgdb is the
# database layer the statements are built for.
target_link_libraries(lr_weapons_core PUBLIC plgdb)
target_compile_options(lr_weapons_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_weapons_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- generated levels_ranks bindings -------------------------------------------
# The __levels_ranks_* globals have to be compiled into every consumer plugin; plugins/levels_ranks
# builds them as the object library `lr_bindings` (it sorts before this directory), this is the
# fallback for a build of this directory alone / an exported repository.
if(NOT TARGET lr_bindings AND EXISTS
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    add_library(lr_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    target_include_directories(lr_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(lr_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

# --- plugin -------------------------------------------------------------------
add_library(plugify-lr_weapons SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:lr_bindings>
)
target_include_directories(plugify-lr_weapons PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated lr/plg/s2sdk headers and lr_api.hpp must not fail -Werror on
# our sources.
target_include_directories(plugify-lr_weapons SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include"
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-lr_weapons PRIVATE
        plugify_sdk
        lr_weapons_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-lr_weapons PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-lr_weapons PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="lr_weapons"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-lr_weapons PROPERTIES OUTPUT_NAME lr_weapons)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-lr_weapons PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/lr_weapons.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/lr_weapons.pplugin"
)

# --- tests ---------------------------------------------------------------------
# Engine-free tests (the shipped exstats_weapons.ini through the plugin's own KV1 reader, the
# config fallback and the SQL against SQLite and the test MySQL of ./test.sh); build/tests so
# ../test.sh runs them. Nothing here links pisexcompat, plugify_sdk or an engine binding.
add_executable(lr_weapons_tests tests/lr_weapons_tests.cpp)
target_link_libraries(lr_weapons_tests PRIVATE lr_weapons_core -static-libstdc++ -static-libgcc)
target_compile_options(lr_weapons_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_weapons_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        LR_WEAPONS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
set_target_properties(lr_weapons_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# lr_weapons_plugin_tests: the raw-game-event path of the plugin itself (the vip_af_tests pattern).
# tests/lr_weapons_plugin_tests.cpp includes ../src/plugin.cpp, so the real LR kill hook is driven
# with a fake raw s2sdk event: the "attacker" slot must come from GetEventPlayerSlot (the field is a
# player_controller_and_pawn one; GetEventInt is a different engine call that answers the raw event
# value, not a slot), -1/world drops the kill and a missing player-slot binding fails soft. The
# __s2sdk_* / __levels_ranks_* binding globals are the compiled-in ones, filled with recording fakes.
# No pisexcompat.
add_executable(lr_weapons_plugin_tests
        tests/lr_weapons_plugin_tests.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:lr_bindings>
)
target_include_directories(lr_weapons_plugin_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated lr/plg/s2sdk headers and lr_api.hpp must not fail -Werror on
# our sources.
target_include_directories(lr_weapons_plugin_tests SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include"
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(lr_weapons_plugin_tests PRIVATE
        plugify_sdk
        lr_weapons_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(lr_weapons_plugin_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_weapons_plugin_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="lr_weapons"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
        LR_WEAPONS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)
set_target_properties(lr_weapons_plugin_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
