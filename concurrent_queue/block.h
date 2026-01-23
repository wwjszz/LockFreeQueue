//
// Created by admin on 2025/12/12.
//

#ifndef LOCKFREESTRUCTURES_BLOCK_H
#define LOCKFREESTRUCTURES_BLOCK_H

#include <array>
#include <atomic>
#include <bit>
#if defined( ENABLE_MEMORY_LEAK_DETECTION )
#include <cstdio>
#endif

#include "common/common.h"

namespace hakle {

#ifdef HAKLE_USE_CONCEPT
template <class T>
concept is_policy = requires( T& t, const T& ct, std::size_t Index, std::size_t Count ) {
    { T::has_meaningful_set_result } -> std::convertible_to<bool>;
    { ct.is_empty() } -> std::same_as<bool>;
    { t.set_empty( Index ) } -> std::same_as<bool>;
    { t.set_some_empty( Index, Count ) } -> std::same_as<bool>;
    t.set_all_empty();
    t.reset();
};

template <class T>
concept is_block = requires {
    typename T::value_type;
    { T::has_meaningful_set_result } -> std::convertible_to<bool>;
    { T::block_size } -> std::convertible_to<std::size_t>;
} && T::block_size > 1 && std::has_single_bit( static_cast<std::size_t>( T::block_size ) ) && is_policy<T>;

template <std::size_t BLOCK_SIZE, class BLOCK_TYPE>
concept check_block_size = is_block<BLOCK_TYPE> && BLOCK_SIZE == BLOCK_TYPE::block_size;

template <class T>
concept is_block_with_meaningful_set_result = is_block<T> && ( T::has_meaningful_set_result == true );
#endif

template <class T>
struct free_list_node;

template <class T, std::size_t BLOCK_SIZE, HAKLE_CONCEPT( is_policy ) Policy>
struct hakle_block;

template <std::size_t BLOCK_SIZE>
struct flags_check_policy;

template <std::size_t BLOCK_SIZE>
struct counter_check_policy;

template <class T, std::size_t BLOCK_SIZE>
using hakle_flags_block = hakle_block<T, BLOCK_SIZE, flags_check_policy<BLOCK_SIZE>>;

template <class T, std::size_t BLOCK_SIZE>
using hakle_counter_block = hakle_block<T, BLOCK_SIZE, counter_check_policy<BLOCK_SIZE>>;

// TODO: memory_order!!!
template <std::size_t BLOCK_SIZE>
struct flags_check_policy {
    constexpr static bool has_meaningful_set_result = false;

    HAKLE_CPP20_CONSTEXPR ~flags_check_policy() = default;

    HAKLE_NODISCARD HAKLE_CPP20_CONSTEXPR bool is_empty() const {
        for ( auto& Flag : flags ) {
            if ( !Flag.load( std::memory_order_relaxed ) ) {
                return false;
            }
        }

        std::atomic_thread_fence( std::memory_order_acquire );
        return true;
    }

    HAKLE_CPP20_CONSTEXPR bool set_empty( std::size_t Index ) {
        flags[ Index ].store( 1, std::memory_order_release );
        return false;
    }

    HAKLE_CPP20_CONSTEXPR bool set_some_empty( std::size_t Index, std::size_t Count ) {
        std::atomic_thread_fence( std::memory_order_release );

        for ( std::size_t i = 0; i < Count; ++i ) {
            flags[ Index + i ].store( 1, std::memory_order_relaxed );
        }
        return false;
    }

    HAKLE_CPP20_CONSTEXPR void set_all_empty() {
        for ( std::size_t i = 0; i < BLOCK_SIZE; ++i ) {
            flags[ i ].store( 1, std::memory_order_release );
        }
    }

    HAKLE_CPP20_CONSTEXPR void reset() {
        for ( auto& Flag : flags ) {
            Flag.store( 0, std::memory_order_release );
        }
    }

#if defined( ENABLE_MEMORY_LEAK_DETECTION )
    void print_policy() {
        printf( "===PrintPolicy BLOCK_SIZE: %llu===\n", BLOCK_SIZE );
        for ( int i = 0; i < BLOCK_SIZE; ++i ) {
            printf( "Flag[%d]=%hhu\n", i, flags[ i ].load() );
        }
    }
#endif

    std::array<std::atomic<uint8_t>, BLOCK_SIZE> flags;
};

template <std::size_t BLOCK_SIZE>
struct counter_check_policy {
    constexpr static bool has_meaningful_set_result = true;

    HAKLE_CPP20_CONSTEXPR ~counter_check_policy() = default;

    HAKLE_NODISCARD HAKLE_CPP20_CONSTEXPR bool is_empty() const {
        if ( counter.load( std::memory_order_relaxed ) == BLOCK_SIZE ) {
            std::atomic_thread_fence( std::memory_order_acquire );
            return true;
        }
        return false;
    }

    // Increments the counter and returns true if the block is now empty
    HAKLE_CPP20_CONSTEXPR bool set_empty( HAKLE_MAYBE_UNUSED std::size_t Index ) {
        std::size_t OldCounter = counter.fetch_add( 1, std::memory_order_release );
        return OldCounter + 1 == BLOCK_SIZE;
    }

    HAKLE_CPP20_CONSTEXPR bool set_some_empty( HAKLE_MAYBE_UNUSED std::size_t Index, std::size_t Count ) {
        std::size_t OldCounter = counter.fetch_add( Count, std::memory_order_release );
        return OldCounter + Count == BLOCK_SIZE;
    }

    HAKLE_CPP20_CONSTEXPR void set_all_empty() { counter.store( BLOCK_SIZE, std::memory_order_release ); }

    HAKLE_CPP20_CONSTEXPR void reset() { counter.store( 0, std::memory_order_release ); }

#if defined( ENABLE_MEMORY_LEAK_DETECTION )
    void print_policy() {
        printf( "===PrintPolicy BLOCK_SIZE: %llu===\n", BLOCK_SIZE );
        printf( "counter: %llu\n", counter.load() );
    }
#endif
    std::atomic<std::size_t> counter;
};

enum class BlockMethod { Flags, Counter };

template <class T, std::size_t BLOCK_SIZE, HAKLE_CONCEPT( is_policy ) Policy>
struct hakle_block : free_list_node<hakle_block<T, BLOCK_SIZE, Policy>>, Policy {
    using value_type = T;
    using block_type = hakle_block;
    using Policy::has_meaningful_set_result;
    constexpr static std::size_t block_size = BLOCK_SIZE;

    constexpr T*       operator[]( std::size_t Index ) noexcept { return reinterpret_cast<T*>( elements.data() ) + Index; }
    constexpr const T* operator[]( std::size_t Index ) const noexcept { return reinterpret_cast<T*>( elements.data() ) + Index; }

    alignas( T ) std::array<HAKLE_BYTE, sizeof( T ) * BLOCK_SIZE> elements{};

    hakle_block* next{ nullptr };
};

}  // namespace hakle

#endif  // LOCKFREESTRUCTURES_BLOCK_H
