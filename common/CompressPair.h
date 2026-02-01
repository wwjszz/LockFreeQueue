//
// Created by admin on 25-12-5.
//

#ifndef COMPRESSPAIR_H
#define COMPRESSPAIR_H

#include <concepts>
#include <type_traits>
#include <utility>

#include "common.h"

namespace hakle {

struct DefaultInitTag {};
struct ValueInitTag {};

template <class T, bool>
struct DependentType : T {};

template <class T, int Index,
          bool CanBeEmptyBase = std::is_empty<T>::value
#if HAKLE_CPP_VERSION >= 14
                                && !std::is_final<T>::value
#endif
          >
struct CompressPairElem {

#if HAKLE_CPP_VERSION >= 20
    friend HAKLE_CPP14_CONSTEXPR bool operator==( const CompressPairElem& lhs, const CompressPairElem& rhs ) HAKLE_REQUIRES( std::equality_comparable<T> ) { return lhs.value == rhs.value; }
#endif

    using Reference      = T&;
    using ConstReference = const T&;

    CompressPairElem() = default;
    constexpr explicit CompressPairElem( DefaultInitTag ) {}
    constexpr explicit CompressPairElem( ValueInitTag ) : value() {}

    template <class U, typename std::enable_if<!std::is_same<CompressPairElem, typename std::decay<U>::type>::value, int>::type = 0>
    constexpr explicit CompressPairElem( U&& u ) : value( std::forward<U>( u ) ) {}

    HAKLE_CPP14_CONSTEXPR                   CompressPairElem( const CompressPairElem& Other ) = default;
    HAKLE_CPP14_CONSTEXPR CompressPairElem& operator=( const CompressPairElem& Other )        = default;

    HAKLE_CPP14_CONSTEXPR                   CompressPairElem( CompressPairElem&& Other ) noexcept = default;
    HAKLE_CPP14_CONSTEXPR CompressPairElem& operator=( CompressPairElem&& Other ) noexcept        = default;

    HAKLE_CPP14_CONSTEXPR ~CompressPairElem() = default;

#if HAKLE_CPP_VERSION >= 20
    HAKLE_CPP14_CONSTEXPR void swap( CompressPairElem& Other ) noexcept HAKLE_REQUIRES( std::swappable<T> ) {
        using std::swap;
        swap( value, Other.value );
    }
#endif

    HAKLE_CPP14_CONSTEXPR Reference Get() { return value; }
    constexpr ConstReference        Get() const { return value; }

private:
    T value;
};

template <class T, int Index>
struct CompressPairElem<T, Index, true> : private T {

#if HAKLE_CPP_VERSION >= 20
    friend HAKLE_CPP14_CONSTEXPR bool operator==( const CompressPairElem& lhs, const CompressPairElem& rhs ) HAKLE_REQUIRES( std::equality_comparable<T> ) {
        return static_cast<const T&>( lhs ) == static_cast<const T&>( rhs );
    }
#endif

    using Reference      = T&;
    using ConstReference = const T&;
    using ValueType      = T;

    CompressPairElem() = default;
    constexpr explicit CompressPairElem( DefaultInitTag ) {}
    constexpr explicit CompressPairElem( ValueInitTag ) : ValueType() {}

    template <class U, typename std::enable_if<!std::is_same<CompressPairElem, typename std::decay<U>::type>::value, int>::type = 0>
    constexpr explicit CompressPairElem( U&& u ) : ValueType( std::forward<U>( u ) ) {}

    HAKLE_CPP14_CONSTEXPR                   CompressPairElem( const CompressPairElem& Other ) = default;
    HAKLE_CPP14_CONSTEXPR CompressPairElem& operator=( const CompressPairElem& Other )        = default;

    HAKLE_CPP14_CONSTEXPR                   CompressPairElem( CompressPairElem&& Other ) noexcept = default;
    HAKLE_CPP14_CONSTEXPR CompressPairElem& operator=( CompressPairElem&& Other ) noexcept        = default;

    HAKLE_CPP14_CONSTEXPR ~CompressPairElem() = default;

#if HAKLE_CPP_VERSION >= 20
    HAKLE_CPP14_CONSTEXPR void swap( CompressPairElem& Other ) noexcept HAKLE_REQUIRES( std::is_swappable_v<T> ) {
        using std::swap;
        swap( static_cast<T&>( *this ), static_cast<T&>( Other ) );
    }
#endif

    Reference      Get() { return *this; }
    ConstReference Get() const { return *this; }
};

// TODO: use [[no_unique_address] to replace
template <class T1, class T2>
HAKLE_REQUIRES( ( !std::same_as<T1, T2> ))
class CompressPair : private CompressPairElem<T1, 0>, private CompressPairElem<T2, 1> {
#ifndef HAKLE_USE_CONCEPT
    static_assert( !std::is_same<T1, T2>::value, "T1 and T2 are the same" );
#endif

public:
    using Base1 = CompressPairElem<T1, 0>;
    using Base2 = CompressPairElem<T2, 1>;

    template <bool Dummy                                                                                                                                                           = true,
              typename std::enable_if<DependentType<std::is_default_constructible<T1>, Dummy>::value && DependentType<std::is_default_constructible<T2>, Dummy>::value, int>::type = 0>
    constexpr CompressPair() : Base1( ValueInitTag{} ), Base2( ValueInitTag{} ) {}

    template <class U1, class U2>
    constexpr CompressPair( U1&& X, U2&& Y ) : Base1( std::forward<U1>( X ) ), Base2( std::forward<U2>( Y ) ) {}

    HAKLE_CPP14_CONSTEXPR ~CompressPair() = default;

    HAKLE_CPP14_CONSTEXPR               CompressPair( const CompressPair& Other ) = default;
    HAKLE_CPP14_CONSTEXPR CompressPair& operator=( const CompressPair& Other )    = default;

    HAKLE_CPP14_CONSTEXPR               CompressPair( CompressPair&& Other ) noexcept = default;
    HAKLE_CPP14_CONSTEXPR CompressPair& operator=( CompressPair&& Other ) noexcept    = default;

    HAKLE_CPP14_CONSTEXPR typename Base1::Reference First() { return Base1::Get(); }
    constexpr typename Base1::ConstReference        First() const { return Base1::Get(); }
    HAKLE_CPP14_CONSTEXPR typename Base2::Reference Second() { return Base2::Get(); }
    constexpr typename Base2::ConstReference        Second() const { return Base2::Get(); }

    constexpr static Base1* GetFirstBase( CompressPair* Pair ) noexcept { return static_cast<Base1*>( Pair ); }

    constexpr static Base2* GetSecondBase( CompressPair* Pair ) noexcept { return static_cast<Base2*>( Pair ); }

#if HAKLE_CPP_VERSION >= 20
    constexpr void swap( CompressPair& Other ) noexcept HAKLE_REQUIRES( std::swappable<T1>&& std::swappable<T2> ) {
        using std::swap;
        swap( First(), Other.First() );
        swap( Second(), Other.Second() );
    }
#endif
};

#if HAKLE_CPP_VERSION >= 20
template <class T1, class T2>
HAKLE_REQUIRES( std::equality_comparable<T1>&& std::equality_comparable<T2> )
bool operator==( const CompressPair<T1, T2>& lhs, const CompressPair<T1, T2>& rhs ) {
    return lhs.First() == rhs.First() && lhs.Second() == rhs.Second();
}

template <class T, int Index, bool CanBeEmptyBase>
HAKLE_REQUIRES( std::is_swappable_v<T> )
inline HAKLE_CPP14_CONSTEXPR void swap( CompressPairElem<T, Index, CanBeEmptyBase>& X, CompressPairElem<T, Index, CanBeEmptyBase>& Y ) noexcept {
    X.swap( Y );
}

template <class T1, class T2>
HAKLE_REQUIRES( std::is_swappable_v<T1>&& std::is_swappable_v<T2> )
inline HAKLE_CPP14_CONSTEXPR void swap( CompressPair<T1, T2>& X, CompressPair<T1, T2>& Y ) noexcept {
    X.swap( Y );
}

#endif

}  // namespace hakle

#endif  // COMPRESSPAIR_H
