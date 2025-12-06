cmake_minimum_required(VERSION 3.11)
project(LockFreeQueue CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

# ============================================================================
# Compiler Options
# ============================================================================

# Include CheckCXXCompilerFlag module to check compiler flag support
include(CheckCXXCompilerFlag)

# Check if compiler supports -Wno-interference-size option
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Check if the warning option is supported
    check_cxx_compiler_flag("-Wno-interference-size" COMPILER_SUPPORTS_WNO_INTERFERENCE_SIZE)
    
    if(COMPILER_SUPPORTS_WNO_INTERFERENCE_SIZE)
        add_compile_options(-Wno-interference-size)
        message(STATUS "Compiler supports -Wno-interference-size option, added")
    else()
        message(STATUS "Compiler does not support -Wno-interference-size option, skipped")
    endif()
endif()

# ============================================================================
# include directories
# ============================================================================

include_directories(${PROJECT_SOURCE_DIR})

# ============================================================================
# target
# ============================================================================

add_executable(normaltest
        tests/test.cpp
        tests/simplethread.cpp
)

add_executable(unittest
        tests/unittests.cpp
        tests/simplethread.cpp
)