//
// Created by admin on 25-12-1.
//

#ifndef UTILITY_H
#define UTILITY_H

#include <climits>
#include <type_traits>

namespace hakle {

template<class T>
requires std::is_unsigned_v<T>
inline constexpr bool CircularLessThan(T a, T b) noexcept {
    return static_cast<T>(a - b) > static_cast<T>(static_cast<T>(1) << (static_cast<T>(sizeof(T) * CHAR_BIT - 1)));
}

}

#endif //UTILITY_H
