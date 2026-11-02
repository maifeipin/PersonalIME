cmake_minimum_required(VERSION 3.20)
project(PersonalIME LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enforce static runtime (/MT in Release, /MTd in Debug) for zero external CRT dependency
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if (MSVC)
    add_compile_options(/utf-8)
endif()

add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# 1. Thin TSF In-process DLL
add_library(PersonalIME SHARED
    src/tsf_client/dllmain.cpp
    src/tsf_client/class_factory.cpp
    src/tsf_client/text_input_processor.cpp
    src/tsf_client/candidate_window.cpp
    src/tsf_client/ipc_client.cpp
    src/tsf_client/PersonalIME.def
)

target_link_libraries(PersonalIME PRIVATE
    ole32
    oleaut32
    advapi32
    user32
    gdi32
)

# 2. Local IME Background Service Prototype
add_executable(ime-service
    src/ime_service/main.cpp
)

target_link_libraries(ime-service PRIVATE
    advapi32
    user32
)

# 3. IPC Automated Integration Test Runner
add_executable(test_ipc_runner
    tests/test_ipc_runner.cpp
    src/tsf_client/ipc_client.cpp
)

target_link_libraries(test_ipc_runner PRIVATE
    advapi32
    user32
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
)

# 6. Composition Lifecycle & Fault Injection Test
add_executable(test_composition_lifecycle
    tests/test_composition_lifecycle.cpp
    src/tsf_client/text_input_processor.cpp
    src/tsf_client/candidate_window.cpp
    src/tsf_client/ipc_client.cpp
)

target_link_libraries(test_composition_lifecycle PRIVATE
    ole32
    oleaut32
    advapi32
    user32
    gdi32
)
