cmake_minimum_required(VERSION 3.24)
project(MiniEverything VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(MINI_SQLITE_ARCHIVE "https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip"
    CACHE STRING "SQLite archive URL or local path (SHA256 is always verified)")
string(REPLACE "\\" "/" sqlite_archive "${MINI_SQLITE_ARCHIVE}")
FetchContent_Declare(sqlite_source
    URL "${sqlite_archive}"
    URL_HASH SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite_source)
add_library(sqlite3 STATIC "${sqlite_source_SOURCE_DIR}/sqlite3.c")
target_include_directories(sqlite3 SYSTEM PUBLIC "${sqlite_source_SOURCE_DIR}")
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_DQS=0)
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

add_library(mini_core src/index.cpp)
target_include_directories(mini_core PUBLIC include)
target_link_libraries(mini_core PRIVATE sqlite3)
add_executable(mini-everything src/main.cpp)
target_link_libraries(mini-everything PRIVATE mini_core)
if(WIN32)
    target_compile_definitions(mini_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(mini-everything PRIVATE shell32)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(mini_tests tests/index_tests.cpp)
    target_link_libraries(mini_tests PRIVATE mini_core sqlite3)
    foreach(case IN ITEMS scan_tree persistence rescan literal_search filters unicode overlap invalid_root database_inside deep_tree rollback foreign_database schema_version database_hardlink)
        add_test(NAME ${case} COMMAND mini_tests ${case})
    endforeach()
    add_test(NAME cli COMMAND ${CMAKE_COMMAND}
        -DAPP=$<TARGET_FILE:mini-everything>
        -DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-fixture
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_test.cmake)
    if(WIN32)
        find_program(POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)
        add_test(NAME windows_paths COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_paths.ps1
            -App $<TARGET_FILE:mini-everything>)
    endif()
endif()

foreach(target IN ITEMS mini_core mini-everything mini_tests)
    if(TARGET ${target})
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endif()
endforeach()
install(TARGETS mini-everything RUNTIME DESTINATION bin)
