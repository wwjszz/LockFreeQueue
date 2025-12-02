//
// Created by admin on 25-12-1.
//

#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <cstddef>

#include "memory.h"

namespace hakle {
template <class Tp>
class HakleAllocator {
public:
    using value_type      = Tp;
    using pointer         = Tp*;
    using const_pointer   = const Tp*;
    using reference       = Tp&;
    using const_reference = const Tp&;
    using size_type       = size_t;
    using difference_type = std::ptrdiff_t;

    static constexpr Tp* Allocate() { return HAKLE_OPERATOR_NEW( Tp ); }
    static constexpr Tp* Allocate( size_type n ) { return HAKLE_OPERATOR_NEW_ARRAY( Tp, n ); }

    static constexpr void Deallocate( Tp* ptr ) noexcept { HAKLE_OPERATOR_DELETE( ptr ); }
    static constexpr void Deallocate( Tp* ptr, size_type n ) noexcept { HAKLE_OPERATOR_DELETE_ARRAY( ptr, n ); }

    template <class... Args>
    static constexpr void Construct( Tp* ptr, Args&&... args ) {
        HAKLE_CONSTRUCT( ptr, std::forward<Args>( args )... );
    }

    static constexpr void Destroy( Tp* ptr ) noexcept { HAKLE_DESTROY( ptr ); }
    static constexpr void Destroy( Tp* ptr, size_type n ) noexcept { HAKLE_DESTROY_ARRAY( ptr, n ); }
    static constexpr void Destroy( Tp* first, Tp* last ) noexcept { Destroy( first, last - first ); }
};
}  // namespace hakle

#endif  // ALLOCATOR_H
