//
// Created by admin on 25-12-5.
//

#ifndef COMPRESSPAIR_H
#define COMPRESSPAIR_H

#include <type_traits>
#include <utility>

#include "common.h"

namespace hakle {

struct default_init_tag {};
struct value_init_tag {};

template <class T, bool>
struct dependent_type : T {};

template <class T, int Index, bool CanBeEmptyBase = std::is_empty<T>::value && !std::is_final<T>::value>
struct compress_pair_elem {
    using reference      = T&;
    using const_reference = const T&;

    compress_pair_elem() = default;
    constexpr explicit compress_pair_elem( default_init_tag ) {}
    constexpr explicit compress_pair_elem( value_init_tag ) : value() {}

    template <class U, std::enable_if_t<!std::is_same<compress_pair_elem, std::decay_t<U>>::value, int> = 0>
    constexpr explicit compress_pair_elem( U&& u ) : value( std::forward<U>( u ) ) {}

    reference      get() { return value; }
    const_reference get() const { return value; }

private:
    T value;
};

template <class T, int Index>
struct compress_pair_elem<T, Index, true> : private T {
    using reference      = T&;
    using const_reference = const T&;
    using value_type      = T;

    compress_pair_elem() = default;
    constexpr explicit compress_pair_elem( default_init_tag ) {}
    constexpr explicit compress_pair_elem( value_init_tag ) : value_type() {}

    template <class U, std::enable_if_t<!std::is_same<compress_pair_elem, std::decay_t<U>>::value, int> = 0>
    constexpr explicit compress_pair_elem( U&& u ) : value_type( std::forward<U>( u ) ) {}

    reference      get() { return *this; }
    const_reference get() const { return *this; }
};

// TODO: use [[no_unique_address] to replace
template <class T1, class T2>
HAKLE_REQUIRES( ( !std::same_as<T1, T2> ))
class compress_pair : private compress_pair_elem<T1, 0>, private compress_pair_elem<T2, 1> {
    static_assert( !std::is_same<T1, T2>::value, "T1 and T2 are the same" );

public:
    using Base1 = compress_pair_elem<T1, 0>;
    using Base2 = compress_pair_elem<T2, 1>;

    template <bool Dummy = true, std::enable_if_t<dependent_type<std::is_default_constructible<T1>, Dummy>::value && dependent_type<std::is_default_constructible<T2>, Dummy>::value, int> = 0>
    constexpr compress_pair() : Base1( value_init_tag{} ), Base2( value_init_tag{} ) {}

    template <class U1, class U2>
    constexpr compress_pair( U1&& X, U2&& Y ) : Base1( std::forward<U1>( X ) ), Base2( std::forward<U2>( Y ) ) {}

    constexpr typename Base1::reference      first() { return Base1::get(); }
    constexpr typename Base1::const_reference first() const { return Base1::get(); }
    constexpr typename Base2::reference      second() { return Base2::get(); }
    constexpr typename Base2::const_reference second() const { return Base2::get(); }

    constexpr static Base1* get_first_base( compress_pair* Pair ) noexcept { return static_cast<Base1*>( Pair ); }

    constexpr static Base2* get_second_base( compress_pair* Pair ) noexcept { return static_cast<Base2*>( Pair ); }

    constexpr void swap( compress_pair& Other ) noexcept {
        using std::swap;
        swap( first(), Other.first() );
        swap( second(), Other.second() );
    }
};

template <class T1, class T2>
inline constexpr void swap( compress_pair<T1, T2>& X, compress_pair<T1, T2>& Y ) noexcept {
    X.swap( Y );
}

}  // namespace hakle

#endif  // COMPRESSPAIR_H
