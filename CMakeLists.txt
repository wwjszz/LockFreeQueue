cmake_minimum_required(VERSION 3.14)
project(LockFreeQueue CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

include(FetchContent)

# Set the download directory to the project's source thirdparty/googletest
# (Alternatively, use ${CMAKE_BINARY_DIR}/thirdparty to keep thirdparty in the build tree)
set(FETCHCONTENT_BASE_DIR ${CMAKE_SOURCE_DIR}/thirdparty/googletest)

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0  # fixed version
)

# This will download at configure time and add_subdirectory(...), creating gtest/gmock targets
FetchContent_MakeAvailable(googletest)

# ============================================================================
# Compiler Options
# ============================================================================

# Include CheckCXXCompilerFlag module to check compiler flag support
include(CheckCXXCompilerFlag)

# Check if compiler supports -Wno-interference-size option
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Check if the warning option is supported
    check_cxx_compiler_flag("-Wno-interference-size" COMPILER_SUPPORTS_WNO_INTERFERENCE_SIZE)

    if (COMPILER_SUPPORTS_WNO_INTERFERENCE_SIZE)
        add_compile_options(-Wno-interference-size)
        message(STATUS "Compiler supports -Wno-interference-size option, added")
    else ()
        message(STATUS "Compiler does not support -Wno-interference-size option, skipped")
    endif ()
endif ()

# ============================================================================
# include directories
# ============================================================================

# ============================================================================
# targets
# ============================================================================

add_executable(normaltest
        tests/test.cpp
        tests/simplethread.cpp
)

add_executable(unittest
        tests/unittests.cpp
        tests/simplethread.cpp
)

enable_testing()

add_executable(linearsearchmaptest tests/linearsearchmaptest.cpp)
target_link_libraries(linearsearchmaptest PRIVATE gtest_main)

add_test(NAME linearsearchmaptest COMMAND linearsearchmaptest)