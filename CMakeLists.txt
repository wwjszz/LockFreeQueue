cmake_minimum_required(VERSION 3.22)
project(LockFreeStructures CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    string(APPEND CMAKE_CXX_FLAGS " -Wall -Wextra -Wpedantic -Werror -Wno-error=self-move")
    # gcc bug
    string(APPEND CMAKE_CXX_FLAGS " -Wno-error=maybe-uninitialized")
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    string(APPEND CMAKE_CXX_FLAGS " /W3 /utf-8")
    # to avoid using the debug runtime library, which is not supported by the static runtime library
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}/MTd /Zi /Ob0 /Od /RTC1")
endif ()

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " -O3")
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " /O2")
endif ()

include(FetchContent)

macro(fetch_or_use_local name url tag local_dir)
    # 检查是否存在完整的源码（CMakeLists.txt + .git 目录）
    if (EXISTS ${local_dir}/CMakeLists.txt AND EXISTS ${local_dir}/.git)
        message(STATUS "Found existing ${name} at ${local_dir}, skip downloading")
        add_subdirectory(${local_dir} ${CMAKE_BINARY_DIR}/${name}-build)
    else ()
        # 如果目录存在但不完整，先删除
        if (EXISTS ${local_dir})
            message(STATUS "Removing incomplete ${name} at ${local_dir}...")
            file(REMOVE_RECURSE ${local_dir})
        endif ()
        message(STATUS "Downloading ${name}...")
        FetchContent_Declare(${name}
                GIT_REPOSITORY ${url}
                GIT_TAG ${tag}
                SOURCE_DIR ${local_dir}
        )
        FetchContent_MakeAvailable(${name})
    endif ()
endmacro()

fetch_or_use_local(googletest https://github.com/google/googletest.git v1.14.0 ${CMAKE_SOURCE_DIR}/thirdparty/googletest)
fetch_or_use_local(benchmark https://github.com/google/benchmark.git v1.9.0 ${CMAKE_SOURCE_DIR}/thirdparty/benchmark)

set(BENCHMARK_ENABLE_TESTING off)  # to suppress benchmark internal tests

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

include_directories(${PROJECT_SOURCE_DIR}/)

# ============================================================================
# targets
# ============================================================================

add_executable(normaltest tests/test.cpp)
add_executable(unittest tests/unittests.cpp tests/simplethread.cpp)
add_executable(samples_linearsearchmaptest tests/samples_linearsearchmaptest.cpp)
add_executable(samples_hashtabletest tests/samples_hashtabletest.cpp)
add_executable(hashtabletest tests/hashtabletest.cpp)
add_executable(freelisttest tests/freelisttest.cpp)
add_executable(blockpooltest tests/blockpooltest.cpp)
add_executable(fastqueuetest tests/fastqueuetest.cpp tests/Debug/ThrowOn.cpp)
add_executable(fastqueuetest_exception tests/fastqueuetest_exception.cpp tests/Debug/ThrowOn.cpp)
add_executable(slowqueuetest tests/slowqueuetest.cpp tests/Debug/ThrowOn.cpp)
add_executable(slowqueuetest_exception tests/slowqueuetest_exception.cpp tests/Debug/ThrowOn.cpp)
add_executable(fastqueuetest_leaks tests/fastqueuetest_leaks.cpp)
add_executable(slowqueuetest_leaks tests/slowqueuetest_leaks.cpp)
add_executable(other_test tests/other_test.cpp)
add_executable(concurrentqueuetest_benchmarks tests/concurrentqueuetest_benchmarks.cpp)
add_executable(concurrentqueuetest_check tests/concurrentqueuetest_check.cpp)
add_executable(concurrentqueuetest_swap_check tests/concurrentqueuetest_swap_check.cpp)
add_executable(concurrentqueuetest_obj_check tests/cq_obj_check.cpp)
add_executable(concurrentqueuetest_exception_obj_check tests/cq_exception_obj_check.cpp)
add_executable(int_bench tests/main_bench.cpp)
add_executable(obj_bench tests/obj_main_bench.cpp)
add_executable(customized tests/customized.cpp)

target_link_libraries(customized PRIVATE gtest_main)
target_link_libraries(samples_linearsearchmaptest PRIVATE gtest_main)
target_link_libraries(samples_hashtabletest PRIVATE gtest_main)
target_link_libraries(hashtabletest PRIVATE gtest_main)
target_link_libraries(freelisttest PRIVATE gtest_main)
target_link_libraries(blockpooltest PRIVATE gtest_main)
target_link_libraries(fastqueuetest PRIVATE gtest_main)
target_link_libraries(fastqueuetest_exception PRIVATE gtest_main)
target_link_libraries(slowqueuetest PRIVATE gtest_main)
target_link_libraries(slowqueuetest_exception PRIVATE gtest_main)
target_link_libraries(fastqueuetest_leaks PRIVATE gtest_main)
target_link_libraries(slowqueuetest_leaks PRIVATE gtest_main)
target_link_libraries(other_test PRIVATE gtest_main)
target_link_libraries(concurrentqueuetest_benchmarks PRIVATE gtest_main)
target_link_libraries(concurrentqueuetest_check PRIVATE gtest_main)
target_link_libraries(concurrentqueuetest_swap_check PRIVATE gtest_main)
target_link_libraries(concurrentqueuetest_obj_check PRIVATE gtest_main)
target_link_libraries(concurrentqueuetest_exception_obj_check PRIVATE gtest_main)

target_link_libraries(int_bench PRIVATE benchmark::benchmark benchmark::benchmark_main)
# if use FreeList_DAS, use target_link_libraries(int_bench PRIVATE benchmark::benchmark benchmark::benchmark_main libatomic)
target_link_libraries(obj_bench PRIVATE benchmark::benchmark benchmark::benchmark_main)

target_compile_definitions(hashtabletest PRIVATE ENABLE_MEMORY_LEAK_DETECTION)
target_compile_definitions(fastqueuetest_leaks PRIVATE ENABLE_MEMORY_LEAK_DETECTION)
target_compile_definitions(slowqueuetest_leaks PRIVATE ENABLE_MEMORY_LEAK_DETECTION)
target_compile_definitions(concurrentqueuetest_benchmarks PRIVATE ENABLE_MEMORY_LEAK_DETECTION)
target_compile_definitions(concurrentqueuetest_check PRIVATE ENABLE_MEMORY_LEAK_DETECTION)
target_compile_definitions(concurrentqueuetest_swap_check PRIVATE ENABLE_MEMORY_LEAK_DETECTION)
target_compile_definitions(concurrentqueuetest_obj_check PRIVATE ENABLE_MEMORY_LEAK_DETECTION)
target_compile_definitions(concurrentqueuetest_exception_obj_check PRIVATE ENABLE_MEMORY_LEAK_DETECTION)


enable_testing()

add_test(NAME samples_linearsearchmaptest COMMAND samples_hashtabletest)
add_test(NAME samples_hashtabletest COMMAND samples_hashtabletest)
add_test(NAME hashtabletest COMMAND hashtabletest)
add_test(NAME freelisttest COMMAND freelisttest)
add_test(NAME blockpooltest COMMAND blockpooltest)
add_test(NAME fastqueuetest COMMAND fastqueuetest)
add_test(NAME fastqueuetest_exception COMMAND fastqueuetest_exception)
add_test(NAME slowqueuetest COMMAND slowqueuetest)
add_test(NAME slowqueuetest_exception COMMAND slowqueuetest_exception)
add_test(NAME slowqueuetest_leaks COMMAND slowqueuetest_leaks)
add_test(NAME fastqueuetest_leaks COMMAND fastqueuetest_leaks)
add_test(NAME concurrentqueuetest_check COMMAND concurrentqueuetest_check)
add_test(NAME concurrentqueuetest_swap_check COMMAND concurrentqueuetest_swap_check)
add_test(NAME customized COMMAND customized)