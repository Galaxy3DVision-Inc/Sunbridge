cmake_minimum_required(VERSION 3.8)
project("sunshine_bridge")

# The XLang repo resides as a sibling directory:
# C:\Galaxy3DVision\Projects\xlang
include_directories("D:/CantorAI/xlang/Api")
include_directories("src")

FILE(GLOB Xlang_Api_SRC "D:/CantorAI/xlang/Api/*.cpp")
FILE(GLOB Module_SRC "src/*.cpp")

add_library(${PROJECT_NAME} 
	SHARED
	${Xlang_Api_SRC}
	${Module_SRC}
)

if(MSVC)
    # Use the static C library for all build types to avoid MSVCRT.dll runtime conflicts
    foreach(var
        CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
        CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO
        CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO
      )
      if(${var} MATCHES "/MD")
        string(REGEX REPLACE "/MD" "/MT" ${var} "${${var}}")
      endif()
    endforeach()
endif(MSVC)
