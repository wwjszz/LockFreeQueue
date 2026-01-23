//
// Created by admin on 25-11-24.
//

#ifndef MEMORY_H
#define MEMORY_H

#include <new>
#include <utility>

#include "common.h"

#if __cpp_lib_hardware_interference_size >= 201703L
#include <new>
#define HAKLE_CACHE_LINE_SIZE std::hardware_destructive_interference_size
#else
#define HAKLE_CACHE_LINE_SIZE 64
#endif

#endif  // MEMORY_H
