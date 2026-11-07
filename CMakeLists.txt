cmake_minimum_required(VERSION 3.20)
project(PersonalIME LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    # Enforce static runtime (/MT in Release, /MTd in Debug) for zero external CRT dependency
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8)
endif()

if (WIN32)
    add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# --- Third Party Dependencies ---
# SQLite 3 Amalgamation
add_library(sqlite3 STATIC
    src/third_party/sqlite/sqlite3.c
)
target_include_directories(sqlite3 PUBLIC
    src/third_party/sqlite
)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_RTREE
)

# --- Stage 2: Core IME Engine Library ---
add_library(ime-core STATIC
    src/core/storage/sqlite_db.cpp
    src/core/storage/migration.cpp
    src/core/storage/entry_repository.cpp
    src/core/storage/plugin_repository.cpp
    src/core/storage/json_backup.cpp
    src/core/encoding/pinyin_segmenter.cpp
    src/core/encoding/shuangpin_decoder.cpp
    src/core/strategy/input_coordinator.cpp
    src/core/dictionary/builtin_dict.cpp
    src/core/ranking/ranker.cpp
    src/core/engine/ime_engine.cpp
)

target_include_directories(ime-core PUBLIC
    src
    src/third_party
)

target_link_libraries(ime-core PUBLIC
    sqlite3
)

# --- Stage 2: CLI Debugger & REPL Tool ---
add_executable(ime-cli
    src/cli/main.cpp
)
target_link_libraries(ime-cli PRIVATE
    ime-core
)

# --- Stage 2: ime-core Test Suite ---
add_executable(test_ime_core
    tests/test_ime_core.cpp
)
target_link_libraries(test_ime_core PRIVATE
    ime-core
)

if (WIN32)
# --- Stage 1 Targets ---

# 1. Thin TSF In-process DLL
add_library(PersonalIME SHARED
    src/tsf_client/dllmain.cpp
    src/tsf_client/class_factory.cpp
    src/tsf_client/text_input_processor.cpp
    src/tsf_client/candidate_window.cpp
    src/tsf_client/lang_bar_item.cpp
    src/tsf_client/ipc_client.cpp
    src/tsf_client/PersonalIME.def
    src/tsf_client/PersonalIME.rc
)

target_link_libraries(PersonalIME PRIVATE
    ole32
    oleaut32
    advapi32
    user32
    gdi32
    shell32
)

# 2. Local IME Background Service
add_executable(ime-service
    src/ime_service/main.cpp
    src/ime_service/admin_server.cpp
)

if (MSVC)
    set_target_properties(ime-service PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
    )
endif()

target_link_libraries(ime-service PRIVATE
    ime-core
    advapi32
    user32
    shell32
)

# Stage 3 End-to-End Integration Test Runner
add_executable(test_stage3_integration
    tests/test_stage3_integration.cpp
    src/tsf_client/ipc_client.cpp
    src/tsf_client/text_input_processor.cpp
    src/tsf_client/candidate_window.cpp
    src/tsf_client/lang_bar_item.cpp
)

target_link_libraries(test_stage3_integration PRIVATE
    ime-core
    advapi32
    user32
    ole32
    oleaut32
    gdi32
    shell32
)

# 3. IPC Automated Integration Test Runner
add_executable(test_ipc_runner
    tests/test_ipc_runner.cpp
    src/tsf_client/ipc_client.cpp
)

target_link_libraries(test_ipc_runner PRIVATE
    advapi32
    user32
    shell32
)

# 4. COM DLL Lifecycle Test Runner
add_executable(test_com_lifecycle
    tests/test_com_lifecycle.cpp
)

target_link_libraries(test_com_lifecycle PRIVATE
    ole32
)

# 5. Candidate Window Class RefCount Safety Test
add_executable(test_candidate_window
    tests/test_candidate_window.cpp
    src/tsf_client/candidate_window.cpp
)

target_link_libraries(test_candidate_window PRIVATE
    user32
    gdi32
    shell32
)

# 6. Composition Lifecycle & Fault Injection Test
add_executable(test_composition_lifecycle
    tests/test_composition_lifecycle.cpp
    src/tsf_client/text_input_processor.cpp
    src/tsf_client/candidate_window.cpp
    src/tsf_client/lang_bar_item.cpp
    src/tsf_client/ipc_client.cpp
)

target_link_libraries(test_composition_lifecycle PRIVATE
    ole32
    oleaut32
    advapi32
    user32
    gdi32
    shell32
)

add_executable(test_receipt_journal tests/test_receipt_journal.cpp)
target_link_libraries(test_receipt_journal PRIVATE user32)

add_executable(test_receipt_handover tests/test_receipt_handover.cpp src/tsf_client/text_input_processor.cpp src/tsf_client/candidate_window.cpp src/tsf_client/lang_bar_item.cpp src/tsf_client/ipc_client.cpp)
target_link_libraries(test_receipt_handover PRIVATE ole32 oleaut32 advapi32 user32 gdi32 shell32)

add_executable(test_receipt_durability tests/test_receipt_durability.cpp)
target_link_libraries(test_receipt_durability PRIVATE user32)

add_executable(test_tip_candidate_snapshot tests/test_tip_candidate_snapshot.cpp tests/tip_snapshot_driver.cpp src/tsf_client/candidate_window.cpp src/tsf_client/lang_bar_item.cpp src/tsf_client/ipc_client.cpp)
target_link_libraries(test_tip_candidate_snapshot PRIVATE ole32 oleaut32 advapi32 user32 gdi32 shell32)
endif() # WIN32

# Stage 4A: Input Policy & Shuangpin Test
add_executable(test_input_policy tests/test_input_policy.cpp)
target_link_libraries(test_input_policy PRIVATE ime-core sqlite3)

# Stage 4B: Admin IPC & Service Integration Test
if (WIN32)
add_executable(test_admin_ipc
    tests/test_admin_ipc.cpp
    src/ime_service/admin_server.cpp
)
target_link_libraries(test_admin_ipc PRIVATE ime-core sqlite3 advapi32 user32)
endif()

