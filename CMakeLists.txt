cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)
project(sunshine_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(PRINCIPIA_ROOT "" CACHE PATH "Path to the Principia source repository")
if(NOT PRINCIPIA_ROOT)
    get_filename_component(PRINCIPIA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
include("${PRINCIPIA_ROOT}/cmake/CasLangDependency.cmake")

set(PRINCIPIA_IPC_SRC
    "${PRINCIPIA_ROOT}/common/ipc/IpcManager.cpp"
    "${PRINCIPIA_ROOT}/common/ipc/IpcStream.cpp")
if(WIN32)
    list(APPEND PRINCIPIA_IPC_SRC "${PRINCIPIA_ROOT}/common/ipc/SMSwapBuffer_Win32.cpp")
else()
    list(APPEND PRINCIPIA_IPC_SRC "${PRINCIPIA_ROOT}/common/ipc/SMSwapBuffer_POSIX.cpp")
endif()

add_library(sunshine_bridge SHARED "src/Main.cpp" ${PRINCIPIA_IPC_SRC})
target_include_directories(sunshine_bridge PRIVATE
    "src"
    "${PRINCIPIA_ROOT}/common/ipc"
    "${PRINCIPIA_ROOT}/common/inc"
    "${PRINCIPIA_ROOT}/thirdparty")
target_link_libraries(sunshine_bridge PRIVATE CasLang::compat)
if(MSVC)
    set_property(TARGET sunshine_bridge PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
