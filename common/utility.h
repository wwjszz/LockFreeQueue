//
// Created by admin on 25-12-1.
//

#ifndef UTILITY_H
#define UTILITY_H

#include <climits>
#include <type_traits>

namespace hakle {

template <class T>
    requires std::is_unsigned_v<T>
inline constexpr bool CircularLessThan( T a, T b ) noexcept {
    return static_cast<T>( a - b ) > static_cast<T>( static_cast<T>( 1 ) << ( static_cast<T>( sizeof( T ) * CHAR_BIT - 1 ) ) );
}

static std::size_t CeilToPow2( std::size_t X ) noexcept {
    --X;
    X |= X >> 1;
    X |= X >> 2;
    X |= X >> 4;
    constexpr std::size_t N = sizeof( std::size_t );
    for ( std::size_t i = 1; i < N; ++i ) {
        X |= X >> ( i << 3 );
    }
    ++X;
    return X;
}

}  // namespace hakle

#endif  // UTILITY_H
