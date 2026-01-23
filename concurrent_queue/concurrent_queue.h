//
// Created by admin on 25-12-1.
//

#ifndef CONCURRENTQUEUE_H
#define CONCURRENTQUEUE_H

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <functional>
#include <thread>
#include <type_traits>

#include "block_manager.h"
#include "common/allocator.h"
#include "common/common.h"
#include "common/compress_pair.h"
#include "common/utility.h"
#include "concurrent_queue.h"
#include "concurrent_queue/HashTable.h"
#include "concurrent_queue/block.h"

namespace hakle {

namespace details {
    using thread_id_t = std::thread::id;
    static const thread_id_t invalid_thread_id;
    inline thread_id_t       thread_id() noexcept { return std::this_thread::get_id(); }
    using thread_hash = std::hash<std::thread::id>;
}  // namespace details

#ifdef HAKLE_USE_CONCEPT
template <class Traits, class = void>
struct has_make_explicit_block_manager_helper : std::false_type {};

template <class Traits>
struct has_make_explicit_block_manager_helper<Traits, std::void_t<decltype( &Traits::make_explicit_block_manager )>> : std::true_type {};

// 检测 Traits 里有没有 MakeImplicit
template <class Traits, class = void>
struct has_make_implicit_block_manager_helper : std::false_type {};

template <class Traits>
struct has_make_implicit_block_manager_helper<Traits, std::void_t<decltype( &Traits::make_implicit_block_manager )>> : std::true_type {};

template <class Traits>
concept is_concurrent_queue_traits = requires( const typename Traits::explicit_allocator_type& ExplicitAllocator, const typename Traits::implicit_allocator_type& ImplicitAllocator ) {
    { Traits::block_size } -> std::convertible_to<std::size_t>;
    { Traits::initial_hash_size } -> std::convertible_to<std::size_t>;
    { Traits::initial_explicit_queue_size } -> std::convertible_to<std::size_t>;
    { Traits::initial_implicit_queue_size } -> std::convertible_to<std::size_t>;

    { Traits::make_default_explicit_block_manager( ExplicitAllocator ) } -> std::same_as<typename Traits::explicit_block_manager>;
    { Traits::make_default_implicit_block_manager( ImplicitAllocator ) } -> std::same_as<typename Traits::implicit_block_manager>;

    requires Traits::block_size > 0 && Traits::initial_hash_size > 0;
    requires is_block<typename Traits::explicit_block_type> && is_block<typename Traits::implicit_block_type>;
    requires is_block_manager<typename Traits::explicit_block_manager> && is_block_manager<typename Traits::implicit_block_manager>;
};

template <class Traits>
concept has_make_explicit_block_manager = has_make_explicit_block_manager_helper<Traits>::value;

template <class Traits>
concept has_make_implicit_block_manager = has_make_implicit_block_manager_helper<Traits>::value;

#endif

struct queue_typeless_base {
    virtual ~queue_typeless_base() = default;
};

// TODO: manager traits
template <class T, std::size_t BLOCK_SIZE, class Allocator, HAKLE_CONCEPT( is_block ) BLOCK_TYPE, HAKLE_CONCEPT( is_block_manager ) BLOCK_MANAGER_TYPE>
HAKLE_REQUIRES( check_block_size<BLOCK_SIZE, BLOCK_TYPE>&& check_block_manager<BLOCK_TYPE, BLOCK_MANAGER_TYPE> )
struct queue_base : public queue_typeless_base {
public:
    using block_manager_type                 = BLOCK_MANAGER_TYPE;
    using block_type                        = BLOCK_TYPE;
    using value_type                        = typename block_manager_type::value_type;
    constexpr static std::size_t block_size = block_manager_type::block_size;

    using value_allocator_type   = Allocator;
    using value_allocator_traits = std::allocator_traits<value_allocator_type>;

    using alloc_mode = typename block_manager_type::alloc_mode;

    constexpr explicit queue_base( const value_allocator_type& in_allocator = value_allocator_type{} ) noexcept : value_allocator_pair_( nullptr, in_allocator ) {}
    virtual HAKLE_CPP20_CONSTEXPR ~queue_base() override = default;

    HAKLE_NODISCARD constexpr std::size_t size() const noexcept {
        std::size_t tail = tail_index_.load( std::memory_order_relaxed );
        std::size_t Head = head_index_.load( std::memory_order_relaxed );
        return circular_less_than( Head, tail ) ? tail - Head : 0;
    }

    HAKLE_NODISCARD constexpr std::size_t get_tail() const noexcept { return tail_index_.load( std::memory_order_relaxed ); }

protected:
    std::atomic<std::size_t>                     head_index_{};
    std::atomic<std::size_t>                     tail_index_{};
    std::atomic<std::size_t>                     dequeue_attempts_count_{};
    std::atomic<std::size_t>                     dequeue_failed_count_{};
    compress_pair<block_type*, value_allocator_type> value_allocator_pair_{};

    constexpr value_allocator_type&       value_allocator() noexcept { return value_allocator_pair_.second(); }
    constexpr const value_allocator_type& value_allocator() const noexcept { return value_allocator_pair_.second(); }

    constexpr block_type*&                       tail_block() noexcept { return value_allocator_pair_.first(); }
    HAKLE_NODISCARD constexpr const block_type*& tail_block() const noexcept { return value_allocator_pair_.first(); }
};

// SPMC Queue
template <class T, std::size_t BLOCK_SIZE, class Allocator = std::allocator<T>, HAKLE_CONCEPT( is_block ) BLOCK_TYPE = hakle_flags_block<T, BLOCK_SIZE>,
          HAKLE_CONCEPT( is_block_manager ) BLOCK_MANAGER_TYPE = hakle_block_manager<BLOCK_TYPE>>
class fast_queue : public queue_base<T, BLOCK_SIZE, Allocator, BLOCK_TYPE, BLOCK_MANAGER_TYPE> {
public:
    using base = queue_base<T, BLOCK_SIZE, Allocator, BLOCK_TYPE, BLOCK_MANAGER_TYPE>;

    using base::block_size;
    using typename base::alloc_mode;
    using typename base::block_manager_type;
    using typename base::block_type;
    using typename base::value_allocator_traits;
    using typename base::value_allocator_type;
    using typename base::value_type;

private:
    constexpr static std::size_t block_size_log2 = bit_width( block_size ) - 1;

    struct index_entry;
    struct index_entry_array;
    using index_entry_allocator_type        = typename value_allocator_traits::template rebind_alloc<index_entry>;
    using index_entry_array_allocator_type   = typename value_allocator_traits::template rebind_alloc<index_entry_array>;
    using index_entry_allocator_traits      = typename value_allocator_traits::template rebind_traits<index_entry>;
    using index_entry_array_allocator_traits = typename value_allocator_traits::template rebind_traits<index_entry_array>;

public:
    constexpr explicit fast_queue( std::size_t InSize, block_manager_type& InBlockManager, const value_allocator_type& InAllocator = value_allocator_type{} ) noexcept
        : base( InAllocator ), block_manager_( InBlockManager ), index_entry_allocator_pair_( 0, IndexEntryAllocatorType( InAllocator ) ),
          index_entry_array_allocator_pair_( 0, index_entry_array_allocatorType( InAllocator ) ) {
        std::size_t initial_size = ceil_to_pow2( InSize ) >> 1;
        if ( initial_size < 2 ) {
            initial_size = 2;
        }
        po_index_entries_size() = initial_size;

        create_new_block_index_array( 0 );
    }

    HAKLE_CPP20_CONSTEXPR ~fast_queue() override {
        if ( this->tail_block() != nullptr ) {
            // first, we find the first block that's half dequeued
            block_type* half_dequeued_block = nullptr;
            if ( ( this->head_index_.load( std::memory_order_relaxed ) & ( block_size - 1 ) ) != 0 ) {
                std::size_t i = ( po_next_index_entry_ - po_index_entries_used() ) & ( po_index_entries_size() - 1 );
                while ( circular_less_than( this->PO_Preventries[ i ].Base + block_size, this->head_index_.load( std::memory_order_relaxed ) ) ) {
                    i = ( i + 1 ) & ( po_index_entries_size() - 1 );
                }
                half_dequeued_block = po_prev_entries_[ i ].inner_block;
            }

            // then, we can return back all the blocks
            block_type* block = this->tail_block();
            do {
                block = block->next;
                if ( block->IsEmpty() ) {
                    continue;
                }

                std::size_t i = 0;
                if ( block == half_dequeued_block ) {
                    i = this->head_index_.load( std::memory_order_relaxed ) & ( block_size - 1 );
                }
                std::size_t temp      = this->tail_index_.load( std::memory_order_relaxed ) & ( block_size - 1 );
                std::size_t last_index = temp == 0 ? block_size : temp;
                while ( i != block_size && ( block != this->tail_block() || i != last_index ) ) {
                    value_allocator_traits::destory( this->value_allocator(), ( *block )[ i++ ] );
                }
            } while ( block != this->tail_block() );
        }

        // let's return block to manager
        if ( this->tail_block() != nullptr ) {
            block_type* block = this->tail_block();
            do {
                block_type* next_block = block->next;
                block_manager_.return_block( block );
                block = next_block;
            } while ( block != this->tail_block() );
        }

        // delete index entry arrays
        index_entry_array* current = current_index_entry_array_.load( std::memory_order_relaxed );
        while ( current != nullptr ) {
            index_entry_array* prev = current->prev;
            index_entry_allocator_traits::deallocate( index_entry_allocator(), current->entries, current->size );
            index_entry_array_allocator_traits::destroy( index_entry_array_allocator(), current );
            index_entry_array_allocator_traits::deallocate( index_entry_array_allocator(), current );
            current = prev;
        }
    }

    // Enqueue, SPMC queue only supports one producer
    template <alloc_mode Mode, class... Args>
    HAKLE_REQUIRES( std::is_constructible_v<value_type, Args&&...> )
    HAKLE_CPP20_CONSTEXPR bool enqueue( Args&&... args ) {
        std::size_t current_tail_index = this->tail_index_.load( std::memory_order_relaxed );
        std::size_t new_tail_index     = current_tail_index + 1;
        std::size_t inner_index       = current_tail_index & ( block_size - 1 );
        if ( inner_index == 0 ) {
            block_type* old_tail_block = this->tail_block();
            // zero, in fact
            // we must find a new block
            if ( this->tail_block() != nullptr && this->tail_block()->next->is_empty() ) {
                // we can re-use that block
                this->tail_block() = this->tail_block()->next;
                this->tail_block()->reset();
            }
            else {
                // we need to find a new block index and get a new block from block manager
                // TODO: add MAX_SIZE check
                if ( !circular_less_than( this->head_index_.load( std::memory_order_relaxed ), current_tail_index + block_size ) ) {
                    return false;
                }

                if ( current_index_entry_array_.load( std::memory_order_relaxed ) == nullptr || po_index_entries_used() == po_index_entries_size() ) {
                    // need to create a new index entry array
                    HAKLE_CONSTEXPR_IF( Mode == alloc_mode::cannot_alloc ) { return false; }
                    else if ( !create_new_block_index_array( po_index_entries_used() ) ) {
                        return false;
                    }
                }

                block_type* new_block = block_manager_.requisition_block( Mode );
                if ( new_block == nullptr ) {
                    return false;
                }

                new_block->reset();
                if ( this->tail_block() == nullptr ) {
                    new_block->next = new_block;
                }
                else {
                    new_block->next          = this->tail_block()->next;
                    this->tail_block()->next = new_block;
                }
                this->tail_block() = new_block;
                // get a new block
                ++po_index_entries_used();
            }

            index_entry& entry = this->current_index_entry_array_.load( std::memory_order_relaxed )->entries[ po_next_index_entry_ ];
            entry.Base        = current_tail_index;
            entry.inner_block  = this->tail_block();
            this->current_index_entry_array_.load( std::memory_order_relaxed )->tail.store( po_next_index_entry_, std::memory_order_release );
            po_next_index_entry_ = ( po_next_index_entry_ + 1 ) & ( po_index_entries_size() - 1 );

            HAKLE_CONSTEXPR_IF( !std::is_nothrow_constructible<value_type, Args&&...>::value ) {
                // we need to handle exception here
                HAKLE_TRY { value_allocator_traits::construct( this->value_allocator(), ( *( this->tail_block() ) )[ inner_index ], std::forward<Args>( args )... ); }
                HAKLE_CATCH( ... ) {
                    // when old_tail_block is nullptr, we should not go back to prevent block leak
                    // rollback
                    this->tail_block()->set_all_empty()();
                    this->tail_block() = old_tail_block == nullptr ? this->tail_block() : old_tail_block;
                    po_next_index_entry_ = ( po_next_index_entry_ - 1 ) & ( po_index_entries_size() - 1 );
                    HAKLE_RETHROW;
                }
            }

            HAKLE_CONSTEXPR_IF( !std::is_nothrow_constructible<value_type, Args&&...>::value ) {
                this->tail_index_.store( new_tail_index, std::memory_order_release );
                return true;
            }
        }

        value_allocator_traits::construct( this->value_allocator(), ( *( this->tail_block() ) )[ inner_index ], std::forward<Args>( args )... );

        this->tail_index_.store( new_tail_index, std::memory_order_release );
        return true;
    }

    template <alloc_mode Mode, HAKLE_CONCEPT( std::input_iterator ) Iterator>
    HAKLE_REQUIRES( requires( Iterator Item ) { value_type( *Item ); } )
    HAKLE_CPP20_CONSTEXPR bool enqueue_bulk( Iterator item_first, std::size_t count ) {
        // set original state
        std::size_t origin_index_entries_used = po_index_entries_used();
        std::size_t origin_next_index_entry   = po_next_index_entry_;
        block_type*  start_block             = this->tail_block();
        std::size_t start_tail_index         = this->tail_index_.load( std::memory_order_relaxed );
        block_type*  first_allocated_block    = nullptr;

        // roll back
        auto roll_back = [ this, &origin_next_index_entry, &start_block ]() -> void {
            po_next_index_entry_ = origin_next_index_entry;
            this->tail_block() = start_block;
        };

        std::size_t last_tail_index = start_tail_index - 1;
        // std::size_t BlockCountNeed =
        //     ( ( ( Count + Lasttail_index_ ) & ~( block_size - 1 ) ) - ( ( Lasttail_index_ & ~( block_size - 1 ) ) ) ) >> block_size_log2;

        // Starttail_index_ - 1 must be signed before shifting
        std::size_t block_count_need   = ( ( count + start_tail_index - 1 ) >> block_size_log2 ) - ( static_cast<std::make_signed_t<std::size_t>>( start_tail_index - 1 ) >> block_size_log2 );
        std::size_t current_tail_index = last_tail_index & ~( block_size - 1 );

        if ( block_count_need > 0 ) {
            while ( block_count_need > 0 && this->tail_block() != nullptr && this->tail_block()->next->is_empty() ) {
                // we can re-use that block
                --block_count_need;
                current_tail_index += block_size;

                this->tail_block()   = this->tail_block()->next;
                first_allocated_block = first_allocated_block == nullptr ? this->tail_block() : first_allocated_block;
                this->tail_block()->Reset();

                auto& entry       = this->current_index_entry_array_.load( std::memory_order_relaxed )->entries[ po_next_index_entry_ ];
                entry.base        = current_tail_index;
                entry.inner_block  = this->tail_block();
                po_next_index_entry_ = ( po_next_index_entry_ + 1 ) & ( po_index_entries_size() - 1 );
            }

            while ( block_count_need > 0 ) {
                // we must get a new block
                --block_count_need;
                current_tail_index += block_size;

                // TODO: add MAX_SIZE check
                if ( !circular_less_than( this->head_index_.load( std::memory_order_relaxed ), current_tail_index + block_size ) ) {
                    roll_back();
                    return false;
                }

                if ( current_index_entry_array_.load( std::memory_order_relaxed ) == nullptr || po_index_entries_used() == po_index_entries_size() ) {
                    // need to create a new index entry array
                    HAKLE_CONSTEXPR_IF( Mode == alloc_mode::cannot_alloc ) {
                        roll_back();
                        return false;
                    }
                    else if ( !create_new_block_index_array( origin_index_entries_used ) ) {
                        roll_back();
                        return false;
                    }

                    origin_next_index_entry = origin_index_entries_used;
                }

                block_type* new_block = block_manager_.requisition_block( Mode );
                if ( new_block == nullptr ) {
                    roll_back();
                    return false;
                }

                new_block->reset();
                if ( this->tail_block() == nullptr ) {
                    new_block->next = new_block;
                }
                else {
                    new_block->next          = this->tail_block()->next;
                    this->tail_block()->next = new_block;
                }
                this->tail_block()   = new_block;
                first_allocated_block = first_allocated_block == nullptr ? this->tail_block() : first_allocated_block;
                // get a new block
                ++po_index_entries_used();

                auto& entry       = this->current_index_entry_array_.load( std::memory_order_relaxed )->entries[ po_next_index_entry_ ];
                entry.base        = current_tail_index;
                entry.inner_block  = this->tail_block();
                po_next_index_entry_ = ( po_next_index_entry_ + 1 ) & ( po_index_entries_size() - 1 );
            }
        }

        // we already have enough blocks, let's fill them
        std::size_t start_inner_index = start_tail_index & ( block_size - 1 );
        block_type*  current_block    = ( start_inner_index == 0 && first_allocated_block != nullptr ) ? first_allocated_block : start_block;
        while ( true ) {
            std::size_t end_inner_index = ( current_block == this->tail_block() ) ? ( start_tail_index + count - 1 ) & ( block_size - 1 ) : ( block_size - 1 );
            HAKLE_CONSTEXPR_IF( std::is_nothrow_constructible<value_type, typename std::iterator_traits<Iterator>::value_type>::value ) {
                while ( start_inner_index <= end_inner_index ) {
                    value_allocator_traits::construct( this->value_allocator(), ( *current_block )[ start_inner_index ], *item_first++ );
                    ++start_inner_index;
                }
            }
            else {
                HAKLE_TRY {
                    while ( start_inner_index <= end_inner_index ) {
                        value_allocator_traits::construct( this->value_allocator(), ( *( current_block ) )[ start_inner_index ], *item_first++ );
                        ++start_inner_index;
                    }
                }
                HAKLE_CATCH( ... ) {
                    // we need to set all allocated blocks to empty
                    if ( first_allocated_block != nullptr ) {
                        block_type* allocated_block = first_allocated_block;
                        while ( true ) {
                            allocated_block->set_all_empty();
                            // block_manager_.ReturnBlock( AllocatedBlock  );
                            if ( allocated_block == this->tail_block() ) {
                                break;
                            }
                            allocated_block = allocated_block->next;
                        }
                    }

                    roll_back();

                    // destroy all values
#if !defined( ENABLE_MEMORY_LEAK_DETECTION )
                    HAKLE_CONSTEXPR_IF( !std::is_trivially_destructible<value_type>::value ) {
#endif
                        std::size_t start_inner_index2 = start_tail_index & ( block_size - 1 );
                        block_type*  start_block2      = ( start_inner_index2 == 0 && first_allocated_block != nullptr ) ? first_allocated_block : start_block;
                        while ( true ) {
                            std::size_t end_inner_index2 = ( start_block2 == current_block ) ? start_inner_index : block_size;
                            while ( start_inner_index2 != end_inner_index2 ) {
                                value_allocator_traits::destroy( this->value_allocator(), ( *( start_block2 ) )[ start_inner_index2 ] );
                                ++start_inner_index2;
                            }
                            if ( start_block2 == current_block ) {
                                break;
                            }
                            start_block2      = start_block2->next;
                            start_inner_index2 = 0;
                        }
#if !defined( ENABLE_MEMORY_LEAK_DETECTION )
                    }
#endif

                    HAKLE_RETHROW;
                }
            }
            if ( current_block == this->tail_block() ) {
                break;
            }
            start_inner_index = 0;
            current_block    = current_block->next;
        }

        if ( first_allocated_block != nullptr ) {
            this->current_index_entry_array_.load( std::memory_order_relaxed )->tail.store( ( po_next_index_entry_ - 1 ) & ( po_index_entries_size() - 1 ), std::memory_order_release );
        }
        this->tail_index_.store( start_tail_index + count, std::memory_order_release );
        return true;
    }

    // TODO: EnqueueBulkMove

    // Dequeue
    template <class U>
    constexpr bool dequeue( U& element ) HAKLE_REQUIRES( std::assignable_from<decltype( element ), value_type&&> ) {
        std::size_t failed_count = this->dequeue_failed_count_.load( std::memory_order_relaxed );
        if ( HAKLE_LIKELY( circular_less_than( this->dequeue_attempts_count_.load( std::memory_order_relaxed ) - failed_count, this->tail_index_.load( std::memory_order_relaxed ) ) ) ) {
            // TODO: understand this
            std::atomic_thread_fence( std::memory_order_acquire );

            std::size_t attempts_count = this->dequeue_attempts_count_.fetch_add( 1, std::memory_order_relaxed );
            if ( HAKLE_LIKELY( circular_less_than( attempts_count - failed_count, this->tail_index_.load( std::memory_order_acquire ) ) ) ) {
                // NOTE: getting head_index_ must be front of getting current_index_entry_array_
                // if get current_index_entry_array_ first, there is a situation that makes FirstBlockIndexBase larger than IndexEntryTailBase
                std::size_t index      = this->head_index_.fetch_add( 1, std::memory_order_relaxed );
                std::size_t inner_index = index & ( block_size - 1 );

                // we can dequeue
                index_entry_array* local_index_entry_array = this->current_index_entry_array_.load( std::memory_order_acquire );
                std::size_t      local_index_entry_index = local_index_entry_array->tail.load( std::memory_order_acquire );

                std::size_t index_entry_tail_base  = local_index_entry_array->entries[ local_index_entry_index ].base;
                std::size_t first_block_index_base = index & ~( block_size - 1 );
                std::size_t offset              = ( first_block_index_base - index_entry_tail_base ) >> block_size_log2;
                block_type*  dequeue_block        = local_index_entry_array->entries[ ( local_index_entry_index + offset ) & ( local_index_entry_array->size - 1 ) ].inner_block;
                value_type&  value               = *( *dequeue_block )[ inner_index ];

                HAKLE_CONSTEXPR_IF( !std::is_nothrow_assignable<U&, value_type>::value ) {
                    struct guard {
                        block_type*                                    block;
                        compress_pair<std::size_t, value_allocator_type> value_allocator_pair;

                        ~guard() {
                            value_allocator_traits::destroy( value_allocator_pair.Second(), ( *block )[ value_allocator_pair.first() ] );
                            block->set_empty( value_allocator_pair.first() );
                        }
                    } guard{ .block = dequeue_block, .value_allocator_pair = { inner_index, this->value_allocator() } };

                    element = std::move( value );
                }
                else {
                    element = std::move( value );
                    value_allocator_traits::destroy( this->value_allocator(), &value );
                    dequeue_block->set_empty( inner_index );
                }
                return true;
            }

            this->dequeue_failed_count_.fetch_add( 1, std::memory_order_release );
        }
        return false;
    }

    template <HAKLE_CONCEPT( std::output_iterator<value_type&&> ) Iterator>
    std::size_t dequeue_bulk( Iterator item_first, std::size_t max_count ) {
        std::size_t failed_count  = this->dequeue_failed_count_.load( std::memory_order_relaxed );
        std::size_t desired_count = this->tail_index_.load( std::memory_order_relaxed ) - ( this->dequeue_attempts_count_.load( std::memory_order_relaxed ) - failed_count );
        if ( HAKLE_LIKELY( circular_less_than<std::size_t>( 0, desired_count ) ) ) {
            desired_count = std::min( desired_count, max_count );
            // TODO: understand this
            std::atomic_thread_fence( std::memory_order_acquire );

            std::size_t attempts_count = this->dequeue_attempts_count_.fetch_add( desired_count, std::memory_order_relaxed );
            std::size_t actual_count   = this->tail_index_.load( std::memory_order_acquire ) - ( attempts_count - failed_count );
            if ( HAKLE_LIKELY( circular_less_than<std::size_t>( 0, actual_count ) ) ) {
                actual_count = std::min( actual_count, desired_count );
                if ( actual_count < desired_count ) {
                    this->dequeue_failed_count_.fetch_add( desired_count - actual_count, std::memory_order_release );
                }

                std::size_t first_index = this->head_index_.fetch_add( actual_count, std::memory_order_relaxed );
                std::size_t inner_index = first_index & ( block_size - 1 );

                index_entry_array* local_index_entries_array = this->current_index_entry_array_.load( std::memory_order_acquire );
                std::size_t      local_index_entry_index   = local_index_entries_array->tail.load( std::memory_order_acquire );

                std::size_t index_entry_tail_base  = local_index_entries_array->entries[ local_index_entry_index ].Base;
                std::size_t first_block_index_base = first_index & ~( block_size - 1 );
                std::size_t offset              = ( first_block_index_base - index_entry_tail_base ) >> block_size_log2;
                block_type*  first_dequeue_block   = local_index_entries_array->entries[ ( local_index_entry_index + offset ) & ( local_index_entries_array->size - 1 ) ].inner_block;

                block_type*  dequeue_block = first_dequeue_block;
                std::size_t start_index   = inner_index;
                std::size_t need_count    = actual_count;
                while ( need_count != 0 ) {
                    std::size_t end_index     = ( need_count > ( block_size - start_index ) ) ? block_size : ( need_count + start_index );
                    std::size_t current_index = start_index;
                    HAKLE_CONSTEXPR_IF( std::is_nothrow_assignable<typename std::iterator_traits<Iterator>::value_type&, value_type&&>::value ) {
                        while ( current_index != end_index ) {
                            value_type& Value = *( *dequeue_block )[ current_index ];
                            *item_first       = std::move( Value );
                            ++item_first;
                            value_allocator_traits::destroy( this->value_allocator(), &Value );
                            ++current_index;
                            --need_count;
                        }
                    }
                    else {
                        HAKLE_TRY {
                            while ( current_index != end_index ) {
                                value_type& Value = *( *dequeue_block )[ current_index ];
                                *item_first++     = std::move( Value );
                                value_allocator_traits::destroy( this->value_allocator(), &Value );
                                ++current_index;
                                --need_count;
                            }
                        }
                        HAKLE_CATCH( ... ) {
                            // we need to destroy all the remaining values
                            goto enter;
                            while ( need_count != 0 ) {
                                end_index     = ( need_count > ( block_size - start_index ) ) ? block_size : ( need_count + start_index );
                                current_index = start_index;
                            enter:
                                while ( current_index != end_index ) {
                                    value_type& Value = *( *dequeue_block )[ current_index ];
                                    value_allocator_traits::destory( this->value_allocator(), &Value );
                                    --need_count;
                                    ++current_index;
                                }

                                dequeue_block->set_some_empty( start_index, end_index - start_index );
                                start_index   = 0;
                                dequeue_block = dequeue_block->next;
                            }
                            HAKLE_RETHROW;
                        }
                    }
                    block_type* temp_block = dequeue_block;
                    dequeue_block         = dequeue_block->next;
                    temp_block->set_some_empty( start_index, end_index - start_index );
                    start_index = 0;
                }
                return actual_count;
            }

            this->dequeue_failed_count_.fetch_add( desired_count, std::memory_order_release );
        }
        return 0;
    }

private:
    struct index_entry {
        std::size_t base{ 0 };
        block_type*  inner_block{ nullptr };
    };

    struct index_entry_array {
        std::size_t              size{};
        std::atomic<std::size_t> tail{};
        index_entry*              entries{ nullptr };
        index_entry_array*         prev{ nullptr };
    };

    HAKLE_CPP20_CONSTEXPR bool create_new_block_index_array( std::size_t filled_slot ) noexcept {
        std::size_t SizeMask = po_index_entries_size() - 1;

        po_index_entries_size() <<= 1;
        index_entry_array* new_index_entry_array = nullptr;
        index_entry*      new_entries         = nullptr;

        HAKLE_TRY {
            new_index_entry_array = index_entry_array_allocator_traits::allocate( index_entry_array_allocator() );
            new_entries         = index_entry_allocator_traits::allocate( index_entry_allocator(), po_index_entries_size() );
        }
        HAKLE_CATCH( ... ) {
            if ( new_index_entry_array ) {
                index_entry_array_allocator_traits::deallocate( index_entry_array_allocator(), new_index_entry_array );
                new_index_entry_array = nullptr;
            }
            po_index_entries_size() >>= 1;
            return false;
        }

        // noexcept
        index_entry_array_allocator_traits::construct( index_entry_array_allocator(), new_index_entry_array );

        std::size_t j = 0;
        if ( po_index_entries_used() != 0 ) {
            std::size_t i = ( po_next_index_entry_ - po_index_entries_used() ) & SizeMask;
            do {
                new_entries[ j++ ] = po_prev_entries_[ i ];
                i                 = ( i + 1 ) & SizeMask;
            } while ( i != po_next_index_entry_ );
        }

        new_index_entry_array->size    = po_index_entries_size();
        new_index_entry_array->entries = new_entries;
        new_index_entry_array->tail.store( filled_slot - 1, std::memory_order_relaxed );
        new_index_entry_array->prev = current_index_entry_array_.load( std::memory_order_relaxed );

        po_next_index_entry_ = j;
        po_prev_entries_    = new_entries;
        current_index_entry_array_.store( new_index_entry_array, std::memory_order_release );
        return true;
    }

    // tail index_ Entry Array
    std::atomic<index_entry_array*> current_index_entry_array_{ nullptr };

    // Block Manager
    block_manager_type& block_manager_{};

    // compressed allocator
    compress_pair<std::size_t, index_entry_allocator_type>      index_entry_allocator_pair_{};
    compress_pair<std::size_t, index_entry_array_allocator_type> index_entry_array_allocator_pair_{};

    constexpr index_entry_allocator_type&      index_entry_allocator() noexcept { return index_entry_allocator_pair_.Second(); }
    constexpr index_entry_array_allocator_type& index_entry_array_allocator() noexcept { return index_entry_array_allocator_pair_.Second(); }

    constexpr const index_entry_allocator_type&      index_entry_allocator() const noexcept { return index_entry_allocator_pair_.Second(); }
    constexpr const index_entry_array_allocator_type& index_entry_array_allocator() const noexcept { return index_entry_array_allocator_pair_.Second(); }

    // producer only fields
    constexpr std::size_t& po_index_entries_used() noexcept { return index_entry_allocator_pair_.first(); }
    constexpr std::size_t& po_index_entries_size() noexcept { return index_entry_array_allocator_pair_.first(); }

    HAKLE_NODISCARD constexpr const std::size_t& po_index_entries_used() const noexcept { return index_entry_allocator_pair_.first(); }
    HAKLE_NODISCARD constexpr const std::size_t& po_index_entries_size() const noexcept { return index_entry_array_allocator_pair_.first(); }

    std::size_t po_next_index_entry_{};
    index_entry* po_prev_entries_{ nullptr };
};

template <class T, std::size_t BLOCK_SIZE, class Allocator = std::allocator<T>, HAKLE_CONCEPT( is_block_with_meaningful_set_result ) BLOCK_TYPE = hakle_counter_block<T, BLOCK_SIZE>,
          HAKLE_CONCEPT( is_block_manager ) BLOCK_MANAGER_TYPE = hakle_block_manager<BLOCK_TYPE>>
class slow_queue : public queue_base<T, BLOCK_SIZE, Allocator, BLOCK_TYPE, BLOCK_MANAGER_TYPE> {
public:
    using base = queue_base<T, BLOCK_SIZE, Allocator, BLOCK_TYPE, BLOCK_MANAGER_TYPE>;

    using base::block_size;
    using typename base::alloc_mode;
    using typename base::block_manager_type;
    using typename base::block_type;
    using typename base::value_allocator_traits;
    using typename base::value_allocator_type;
    using typename base::value_type;

private:
    constexpr static std::size_t block_size_log2      = bit_width( block_size ) - 1;
    constexpr static std::size_t INVALID_BLOCK_BASE = 1;

    struct index_entry;
    struct index_entry_array;
    using index_entry_allocator_type        = typename value_allocator_traits::template rebind_alloc<index_entry>;
    using index_entry_array_allocator_type   = typename value_allocator_traits::template rebind_alloc<index_entry_array>;
    using index_entry_pointer_allocator_type = typename value_allocator_traits::template rebind_alloc<index_entry*>;

    using index_entry_allocator_traits        = typename value_allocator_traits::template rebind_traits<index_entry>;
    using index_entry_array_allocator_traits   = typename value_allocator_traits::template rebind_traits<index_entry_array>;
    using index_entry_pointer_allocator_traits = typename value_allocator_traits::template rebind_traits<index_entry*>;

public:
    constexpr slow_queue( std::size_t in_size, block_manager_type& in_block_manager, const value_allocator_type& in_allocator = value_allocator_type{} )
        : base( in_allocator ), index_entry_allocator_pair_( value_init_tag{}, IndexEntryAllocatorType( in_allocator ) ),
          index_entry_array_allocator_pair_( in_block_manager, index_entry_array_allocatorType( in_allocator ) ), index_entry_pointer_allocator_pair_( 0, IndexEntryPointerAllocatorType( in_allocator ) ) {
        std::size_t initial_size = ceil_to_pow2( in_size ) >> 1;
        if ( initial_size < 2 ) {
            initial_size = 2;
        }

        index_entries_size() = initial_size;
        CreateNewBlockIndexArray();
    }

    HAKLE_CPP20_CONSTEXPR ~slow_queue() override {
        std::size_t Index = this->head_index_.load( std::memory_order_relaxed );
        std::size_t tail  = this->tail_index_.load( std::memory_order_relaxed );

        // Release all block
        block_type* Block = nullptr;
        while ( Index != tail ) {
            std::size_t InnerIndex = Index & ( block_size - 1 );
            if ( InnerIndex == 0 || Block == nullptr ) {
                Block = GetBlockIndexEntryForIndex( Index )->Value.load( std::memory_order_relaxed );
            }
            value_allocator_traits::destory( this->value_allocator(), ( *Block )[ InnerIndex ] );
            if ( InnerIndex == block_size - 1 ) {
                BlockManager().ReturnBlock( Block );
            }
            ++Index;
        }

        // Delete index_entry_array
        index_entry_array* CurrentArray = current_index_entry_array().load( std::memory_order_relaxed );
        if ( CurrentArray != nullptr ) {
            for ( std::size_t i = 0; i < CurrentArray->size; ++i ) {
                index_entry_allocator_traits::destory( index_entry_allocator(), CurrentArray->Index[ i ] );
            }

            while ( CurrentArray != nullptr ) {
                index_entry_array* prev = CurrentArray->prev;
                // pass size to detect memory leaks
                index_entry_pointer_allocator_traits::deallocate( IndexEntryPointerAllocator(), CurrentArray->Index, CurrentArray->size );
                index_entry_allocator_traits::deallocate( index_entry_allocator(), CurrentArray->entries, prev == nullptr ? CurrentArray->size : ( CurrentArray->size >> 1 ) );
                index_entry_array_allocator_traits::destory( index_entry_array_allocator(), CurrentArray );
                index_entry_array_allocator_traits::deallocate( index_entry_array_allocator(), CurrentArray );
                CurrentArray = prev;
            }
        }
    }

    template <alloc_mode Mode, class... Args>
    HAKLE_REQUIRES( std::is_constructible_v<value_type, Args&&...> )
    HAKLE_CPP20_CONSTEXPR bool Enqueue( Args&&... args ) {
        std::size_t Currenttail_index_ = this->tail_index_.load( std::memory_order_relaxed );
        std::size_t Newtail_index_     = Currenttail_index_ + 1;
        std::size_t InnerIndex       = Currenttail_index_ & ( block_size - 1 );
        if ( InnerIndex == 0 ) {
            // TODO: add MAX_SIZE check
            if ( !circular_less_than( this->head_index_.load( std::memory_order_relaxed ), Currenttail_index_ + block_size ) ) {
                return false;
            }

            index_entry* NewIndexEntry = nullptr;
            if ( !InsertBlockIndexEntry<Mode>( NewIndexEntry, Currenttail_index_ ) ) {
                return false;
            }

            block_type* NewBlock = BlockManager().RequisitionBlock( Mode );
            if ( NewBlock == nullptr ) {
                RewindBlockIndexTail();
                NewIndexEntry->Value.store( nullptr, std::memory_order_relaxed );
                return false;
            }

            NewBlock->Reset();

            HAKLE_CONSTEXPR_IF( !std::is_nothrow_constructible<value_type, Args&&...>::value ) {
                HAKLE_TRY { value_allocator_traits::construct( this->value_allocator(), ( *NewBlock )[ InnerIndex ], std::forward<Args>( args )... ); }
                HAKLE_CATCH( ... ) {
                    RewindBlockIndexTail();
                    NewIndexEntry->Value.store( nullptr, std::memory_order_relaxed );
                    BlockManager().ReturnBlock( NewBlock );
                    HAKLE_RETHROW;
                }
            }

            NewIndexEntry->Value.store( NewBlock, std::memory_order_relaxed );

            this->tail_block() = NewBlock;

            HAKLE_CONSTEXPR_IF( !std::is_nothrow_constructible<value_type, Args&&...>::value ) {
                this->tail_index_.store( Newtail_index_, std::memory_order_release );
                return true;
            }
        }

        value_allocator_traits::construct( this->value_allocator(), ( *this->tail_block() )[ InnerIndex ], std::forward<Args>( args )... );
        this->tail_index_.store( Newtail_index_, std::memory_order_release );
        return true;
    }

    template <alloc_mode Mode, HAKLE_CONCEPT( std::input_iterator ) Iterator>
    HAKLE_REQUIRES( requires( Iterator Item ) { value_type( *Item ); } )
    HAKLE_CPP20_CONSTEXPR bool EnqueueBulk( Iterator ItemFirst, std::size_t Count ) {
        std::size_t Origintail_index_     = this->tail_index_.load( std::memory_order_relaxed );
        block_type*  Origintail_block     = this->tail_block();
        block_type*  FirstAllocatedBlock = nullptr;

        auto RollBack = [ this, &FirstAllocatedBlock, Origintail_index_, Origintail_block ]() {
            index_entry* IndexEntry       = nullptr;
            std::size_t Currenttail_index_ = ( Origintail_index_ - 1 ) & ~( block_size - 1 );
            for ( block_type* Block = FirstAllocatedBlock; Block; Block = Block->next ) {
                Currenttail_index_ += block_size;
                IndexEntry = GetBlockIndexEntryForIndex( Currenttail_index_ );
                IndexEntry->Value.store( nullptr, std::memory_order_relaxed );
                RewindBlockIndexTail();
            }

            BlockManager().ReturnBlocks( FirstAllocatedBlock );
            this->tail_block() = Origintail_block;
        };

        std::size_t NeedCount        = ( ( Origintail_index_ + Count - 1 ) >> block_size_log2 ) - ( static_cast<std::make_signed_t<std::size_t>>( Origintail_index_ - 1 ) >> block_size_log2 );
        std::size_t Currenttail_index_ = ( Origintail_index_ - 1 ) & ~( block_size - 1 );
        // allocate index entry and block
        while ( NeedCount > 0 ) {
            Currenttail_index_ += block_size;
            --NeedCount;

            bool        IndexInserted = false;
            block_type*  NewBlock      = nullptr;
            index_entry* IndexEntry    = nullptr;

            // TODO: add MAX_SIZE check
            bool full = !circular_less_than( this->head_index_.load( std::memory_order_relaxed ), Currenttail_index_ + block_size );
            if ( full || !( IndexInserted = InsertBlockIndexEntry<Mode>( IndexEntry, Currenttail_index_ ) ) || !( NewBlock = BlockManager().RequisitionBlock( Mode ) ) ) {
                if ( IndexInserted ) {
                    RewindBlockIndexTail();
                    IndexEntry->Value.store( nullptr, std::memory_order_relaxed );
                }
                RollBack();
                return false;
            }

            NewBlock->Reset();
            NewBlock->next = nullptr;

            IndexEntry->Value.store( NewBlock, std::memory_order_relaxed );

            if ( ( Origintail_index_ & ( block_size - 1 ) ) != 0 || FirstAllocatedBlock != nullptr ) {
                this->tail_block()->next = NewBlock;
            }
            this->tail_block() = NewBlock;
            if ( FirstAllocatedBlock == nullptr ) {
                FirstAllocatedBlock = NewBlock;
            }
        }

        // we already have enough blocks, let's fill them
        std::size_t StartInnerIndex = Origintail_index_ & ( block_size - 1 );
        block_type*  StartBlock      = ( StartInnerIndex == 0 && FirstAllocatedBlock != nullptr ) ? FirstAllocatedBlock : Origintail_block;
        block_type*  CurrentBlock    = StartBlock;
        while ( true ) {
            std::size_t EndInnerIndex = ( CurrentBlock == this->tail_block() ) ? ( Origintail_index_ + Count - 1 ) & ( block_size - 1 ) : ( block_size - 1 );
            HAKLE_CONSTEXPR_IF( std::is_nothrow_constructible<value_type, typename std::iterator_traits<Iterator>::value_type>::value ) {
                while ( StartInnerIndex <= EndInnerIndex ) {
                    value_allocator_traits::construct( this->value_allocator(), ( *CurrentBlock )[ StartInnerIndex ], *ItemFirst++ );
                    ++StartInnerIndex;
                }
            }
            else {
                HAKLE_TRY {
                    while ( StartInnerIndex <= EndInnerIndex ) {
                        value_allocator_traits::construct( this->value_allocator(), ( *( CurrentBlock ) )[ StartInnerIndex ], *ItemFirst++ );
                        ++StartInnerIndex;
                    }
                }
                HAKLE_CATCH( ... ) {
                    // first, we need to destroy all values
#if !defined( ENABLE_MEMORY_LEAK_DETECTION )
                    HAKLE_CONSTEXPR_IF( !std::is_trivially_destructible<value_type>::value ) {
#endif
                        std::size_t StartInnerIndex2 = Origintail_index_ & ( block_size - 1 );
                        block_type*  StartBlock2      = ( StartInnerIndex2 == 0 && FirstAllocatedBlock != nullptr ) ? FirstAllocatedBlock : StartBlock;
                        while ( true ) {
                            std::size_t EndInnerIndex2 = ( StartBlock2 == CurrentBlock ) ? StartInnerIndex : block_size;
                            while ( StartInnerIndex2 != EndInnerIndex2 ) {
                                value_allocator_traits::destory( this->value_allocator(), ( *( StartBlock2 ) )[ StartInnerIndex2 ] );
                                ++StartInnerIndex2;
                            }
                            if ( StartBlock2 == CurrentBlock ) {
                                break;
                            }
                            StartBlock2      = StartBlock2->next;
                            StartInnerIndex2 = 0;
                        }
#if !defined( ENABLE_MEMORY_LEAK_DETECTION )
                    }
#endif

                    RollBack();

                    HAKLE_RETHROW;
                }
            }
            if ( CurrentBlock == this->tail_block() ) {
                break;
            }
            StartInnerIndex = 0;
            CurrentBlock    = CurrentBlock->next;
        }

        this->tail_index_.store( Origintail_index_ + Count, std::memory_order_release );
        return true;
    }

    template <class U>
    constexpr bool Dequeue( U& Element ) HAKLE_REQUIRES( std::assignable_from<decltype( Element ), value_type&&> ) {
        std::size_t failed_count = this->dequeue_failed_count_.load( std::memory_order_relaxed );
        if ( HAKLE_LIKELY( circular_less_than( this->dequeue_attempts_count_.load( std::memory_order_relaxed ) - failed_count, this->tail_index_.load( std::memory_order_relaxed ) ) ) ) {
            // TODO: understand this
            std::atomic_thread_fence( std::memory_order_acquire );

            std::size_t attempts_count = this->dequeue_attempts_count_.fetch_add( 1, std::memory_order_relaxed );
            if ( HAKLE_LIKELY( circular_less_than( attempts_count - failed_count, this->tail_index_.load( std::memory_order_acquire ) ) ) ) {
                std::size_t Index      = this->head_index_.fetch_add( 1, std::memory_order_relaxed );
                std::size_t InnerIndex = Index & ( block_size - 1 );

                index_entry* Entry = GetBlockIndexEntryForIndex( Index );
                block_type*  Block = Entry->Value.load( std::memory_order_relaxed );
                value_type&  Value = *( *Block )[ InnerIndex ];

                HAKLE_CONSTEXPR_IF( !std::is_nothrow_assignable<U, value_type>::value ) {
                    struct Guard {
                        index_entry*                                   Entry;
                        block_type*                                    Block;
                        block_manager_type&                             BlockManager;
                        compress_pair<std::size_t, value_allocator_type> ValueAllocatorPair;

                        ~Guard() {
                            value_allocator_traits::destory( ValueAllocatorPair.Second(), ( *Block )[ ValueAllocatorPair.first() ] );
                            if ( Block->set_empty( ValueAllocatorPair.first() ) ) {
                                Entry->Value.store( nullptr, std::memory_order_relaxed );
                                BlockManager.ReturnBlock( Block );
                            }
                        }
                    } guard{ .Entry = Entry, .Block = Block, .BlockManager = BlockManager(), .ValueAllocatorPair = { InnerIndex, this->value_allocator() } };

                    Element = std::move( Value );
                }
                else {
                    Element = std::move( Value );
                    value_allocator_traits::destory( this->value_allocator(), &Value );
                    if ( Block->set_empty( InnerIndex ) ) {
                        Entry->Value.store( nullptr, std::memory_order_relaxed );
                        BlockManager().ReturnBlock( Block );
                    }
                }
                return true;
            }

            this->dequeue_failed_count_.fetch_add( 1, std::memory_order_release );
        }
        return false;
    }

    template <HAKLE_CONCEPT( std::output_iterator<value_type&&> ) Iterator>
    std::size_t DequeueBulk( Iterator ItemFirst, std::size_t MaxCount ) {
        std::size_t failed_count  = this->dequeue_failed_count_.load( std::memory_order_relaxed );
        std::size_t desired_count = this->tail_index_.load( std::memory_order_relaxed ) - ( this->dequeue_attempts_count_.load( std::memory_order_relaxed ) - failed_count );
        if ( HAKLE_LIKELY( circular_less_than<std::size_t>( 0, desired_count ) ) ) {
            desired_count = std::min( desired_count, MaxCount );
            // TODO: understand this
            std::atomic_thread_fence( std::memory_order_acquire );

            std::size_t attempts_count = this->dequeue_attempts_count_.fetch_add( desired_count, std::memory_order_relaxed );
            std::size_t actual_count   = this->tail_index_.load( std::memory_order_acquire ) - ( attempts_count - failed_count );
            if ( HAKLE_LIKELY( circular_less_than<std::size_t>( 0, actual_count ) ) ) {
                actual_count = std::min( actual_count, desired_count );
                if ( actual_count < desired_count ) {
                    this->dequeue_failed_count_.fetch_add( desired_count - actual_count, std::memory_order_release );
                }

                std::size_t Index      = this->head_index_.fetch_add( actual_count, std::memory_order_relaxed );
                std::size_t InnerIndex = Index & ( block_size - 1 );

                std::size_t start_index = InnerIndex;
                std::size_t NeedCount  = actual_count;

                index_entry_array* LocalIndexEntryArray;
                std::size_t      IndexEntryIndex = GetBlockIndexIndexForIndex( Index, LocalIndexEntryArray );
                while ( NeedCount != 0 ) {
                    index_entry* DequeueIndexEntry = LocalIndexEntryArray->Index[ IndexEntryIndex ];
                    block_type*  DequeueBlock      = DequeueIndexEntry->Value.load( std::memory_order_relaxed );
                    std::size_t EndIndex          = ( NeedCount > ( block_size - start_index ) ) ? block_size : ( NeedCount + start_index );
                    std::size_t CurrentIndex      = start_index;
                    HAKLE_CONSTEXPR_IF( std::is_nothrow_assignable<typename std::iterator_traits<Iterator>::value_type, value_type&&>::value ) {
                        while ( CurrentIndex != EndIndex ) {
                            value_type& Value = *( *DequeueBlock )[ CurrentIndex ];
                            *ItemFirst       = std::move( Value );
                            ++ItemFirst;
                            value_allocator_traits::destory( this->value_allocator(), &Value );
                            ++CurrentIndex;
                            --NeedCount;
                        }
                    }
                    else {
                        HAKLE_TRY {
                            while ( CurrentIndex != EndIndex ) {
                                value_type& Value = *( *DequeueBlock )[ CurrentIndex ];
                                *ItemFirst++     = std::move( Value );
                                value_allocator_traits::destory( this->value_allocator(), &Value );
                                ++CurrentIndex;
                                --NeedCount;
                            }
                        }
                        HAKLE_CATCH( ... ) {
                            // we need to destroy all the remaining values
                            goto Enter;
                            while ( NeedCount != 0 ) {
                                DequeueIndexEntry = LocalIndexEntryArray->Index[ IndexEntryIndex ];
                                DequeueBlock      = DequeueIndexEntry->Value.load( std::memory_order_relaxed );
                                EndIndex          = ( NeedCount > ( block_size - start_index ) ) ? block_size : ( NeedCount + start_index );
                                CurrentIndex      = start_index;
                            Enter:
                                while ( CurrentIndex != EndIndex ) {
                                    value_type& Value = *( *DequeueBlock )[ CurrentIndex ];
                                    value_allocator_traits::destory( this->value_allocator(), &Value );
                                    --NeedCount;
                                    ++CurrentIndex;
                                }

                                if ( DequeueBlock->set_some_empty( start_index, EndIndex - start_index ) ) {
                                    DequeueIndexEntry->Value.store( nullptr, std::memory_order_relaxed );
                                    BlockManager().ReturnBlock( DequeueBlock );
                                }
                                start_index      = 0;
                                IndexEntryIndex = ( IndexEntryIndex + 1 ) & ( LocalIndexEntryArray->size - 1 );
                            }
                            HAKLE_RETHROW;
                        }
                    }
                    if ( DequeueBlock->set_some_empty( start_index, EndIndex - start_index ) ) {
                        DequeueIndexEntry->Value.store( nullptr, std::memory_order_relaxed );
                        BlockManager().ReturnBlock( DequeueBlock );
                    }
                    start_index      = 0;
                    IndexEntryIndex = ( IndexEntryIndex + 1 ) & ( LocalIndexEntryArray->size - 1 );
                }
                return actual_count;
            }

            this->dequeue_failed_count_.fetch_add( desired_count, std::memory_order_release );
        }
        return 0;
    }

private:
    struct index_entry {
        std::atomic<std::size_t> key;
        std::atomic<block_type*>  value;
    };

    struct index_entry_array {
        std::size_t              size{};
        std::atomic<std::size_t> tail{};
        index_entry*              entries{ nullptr };
        index_entry**             index{ nullptr };
        index_entry_array*         prev{ nullptr };
    };

    template <alloc_mode Mode>
    HAKLE_CPP20_CONSTEXPR bool InsertBlockIndexEntry( index_entry*& IdxEntry, std::size_t BlockStartIndex ) noexcept {
        index_entry_array* LocalIndexentriesArray = current_index_entry_array().load( std::memory_order_relaxed );
        if ( LocalIndexentriesArray == nullptr ) {
            return false;
        }

        std::size_t NewTail = ( LocalIndexentriesArray->tail.load( std::memory_order_relaxed ) + 1 ) & ( LocalIndexentriesArray->size - 1 );
        IdxEntry            = LocalIndexentriesArray->Index[ NewTail ];
        if ( IdxEntry->Key.load( std::memory_order_relaxed ) == INVALID_BLOCK_BASE || IdxEntry->Value.load( std::memory_order_relaxed ) == nullptr ) {
            IdxEntry->Key.store( BlockStartIndex, std::memory_order_relaxed );
            LocalIndexentriesArray->tail.store( NewTail, std::memory_order_release );
            return true;
        }

        HAKLE_CONSTEXPR_IF( Mode == alloc_mode::cannot_alloc ) { return false; }
        else if ( !CreateNewBlockIndexArray() ) {
            return false;
        }
        else {
            LocalIndexentriesArray = current_index_entry_array().load( std::memory_order_relaxed );
            NewTail                = ( LocalIndexentriesArray->tail.load( std::memory_order_relaxed ) + 1 ) & ( LocalIndexentriesArray->size - 1 );
            IdxEntry               = LocalIndexentriesArray->Index[ NewTail ];
            IdxEntry->Key.store( BlockStartIndex, std::memory_order_relaxed );
            LocalIndexentriesArray->tail.store( NewTail, std::memory_order_release );
            return true;
        }
    }

    HAKLE_CPP20_CONSTEXPR void RewindBlockIndexTail() noexcept {
        index_entry_array* LocalBlockEntryArray = current_index_entry_array().load( std::memory_order_relaxed );
        LocalBlockEntryArray->tail.store( ( LocalBlockEntryArray->tail.load( std::memory_order_relaxed ) - 1 ) & ( LocalBlockEntryArray->size - 1 ), std::memory_order_relaxed );
    }

    HAKLE_CPP20_CONSTEXPR index_entry* GetBlockIndexEntryForIndex( std::size_t Index ) const noexcept {
        index_entry_array* LocalBlockIndexArray;
        std::size_t      BlockIndex = GetBlockIndexIndexForIndex( Index, LocalBlockIndexArray );
        return LocalBlockIndexArray->Index[ BlockIndex ];
    }

    HAKLE_CPP20_CONSTEXPR std::size_t GetBlockIndexIndexForIndex( std::size_t index, index_entry_array*& local_block_index_array ) const noexcept {
        local_block_index_array   = current_index_entry_array().load( std::memory_order_acquire );
        std::size_t tail       = local_block_index_array->tail.load( std::memory_order_acquire );
        std::size_t TailBase   = local_block_index_array->Index[ tail ]->Key.load( std::memory_order_relaxed );
        std::size_t Offset     = ( ( index & ~( block_size - 1 ) ) - TailBase ) >> block_size_log2;
        std::size_t BlockIndex = ( tail + Offset ) & ( local_block_index_array->size - 1 );
        return BlockIndex;
    }

    HAKLE_CPP20_CONSTEXPR bool create_new_block_index_array() noexcept {
        index_entry_array* prev       = current_index_entry_array().load( std::memory_order_relaxed );
        std::size_t      prev_size   = prev == nullptr ? 0 : prev->size;
        std::size_t      entry_count = prev == nullptr ? index_entries_size() : prev_size;

        index_entry_array* new_index_entry_array = nullptr;
        index_entry*      new_entries         = nullptr;
        index_entry**     new_index           = nullptr;

        HAKLE_TRY {
            new_index_entry_array = index_entry_array_allocator_traits::allocate( index_entry_array_allocator() );
            new_entries         = index_entry_allocator_traits::allocate( index_entry_allocator(), entry_count );
            new_index           = index_entry_pointer_allocator_traits::allocate( IndexEntryPointerAllocator(), index_entries_size() );
        }
        HAKLE_CATCH( ... ) {
            if ( new_index_entry_array ) {
                index_entry_array_allocator_traits::deallocate( index_entry_array_allocator(), new_index_entry_array );
                new_index_entry_array = nullptr;
            }

            if ( new_entries ) {
                index_entry_allocator_traits::deallocate( index_entry_allocator(), new_entries, entry_count );
                new_entries = nullptr;
            }
            return false;
        }

        // noexcept
        index_entry_array_allocator_traits::construct( index_entry_array_allocator(), new_index_entry_array );

        if ( prev != nullptr ) {
            std::size_t tail = prev->tail.load( std::memory_order_relaxed );
            std::size_t i = tail, j = 0;
            do {
                i               = ( i + 1 ) & ( prev_size - 1 );
                new_index[ j++ ] = prev->Index[ i ];
            } while ( i != tail );
        }
        for ( std::size_t i = 0; i < entry_count; ++i ) {
            index_entry_allocator_traits::construct( index_entry_allocator(), new_entries + i );
            new_entries[ i ].Key.store( INVALID_BLOCK_BASE, std::memory_order_relaxed );
            new_index[ prev_size + i ] = new_entries + i;
        }

        new_index_entry_array->entries = new_entries;
        new_index_entry_array->Index   = new_index;
        new_index_entry_array->prev    = prev;
        new_index_entry_array->tail.store( ( prev_size - 1 ) & ( index_entries_size() - 1 ), std::memory_order_relaxed );
        new_index_entry_array->size = index_entries_size();

        current_index_entry_array().store( new_index_entry_array, std::memory_order_release );

        index_entries_size() <<= 1;
        return true;
    }

    // compressed allocator
    // tail index_ Entry Array
    compress_pair<std::atomic<index_entry_array*>, index_entry_allocator_type> index_entry_allocator_pair_{};
    // Block Manager
    compress_pair<block_manager_type&, index_entry_array_allocator_type> index_entry_array_allocator_pair_;
    // next block index array capacity
    compress_pair<std::size_t, index_entry_pointer_allocator_type> index_entry_pointer_allocator_pair_{};

    constexpr index_entry_allocator_type&        index_entry_allocator() noexcept { return index_entry_allocator_pair_.second(); }
    constexpr index_entry_array_allocator_type&   index_entry_array_allocator() noexcept { return index_entry_array_allocator_pair_.second(); }
    constexpr index_entry_pointer_allocator_type& IndexEntryPointerAllocator() noexcept { return index_entry_pointer_allocator_pair_.second(); }

    constexpr const index_entry_allocator_type&        index_entry_allocator() const noexcept { return index_entry_allocator_pair_.second(); }
    constexpr const index_entry_array_allocator_type&   index_entry_array_allocator() const noexcept { return index_entry_array_allocator_pair_.second(); }
    constexpr const index_entry_pointer_allocator_type& IndexEntryPointerAllocator() const noexcept { return index_entry_pointer_allocator_pair_.second(); }

    constexpr std::atomic<index_entry_array*>& current_index_entry_array() noexcept { return index_entry_allocator_pair_.first(); }
    constexpr block_manager_type&              block_manager() noexcept { return index_entry_array_allocator_pair_.first(); }
    constexpr std::size_t&                   index_entries_size() noexcept { return index_entry_pointer_allocator_pair_.first(); }

    constexpr const std::atomic<index_entry_array*>& current_index_entry_array() const noexcept { return index_entry_allocator_pair_.first(); }
    constexpr const block_manager_type&              block_manager() const noexcept { return index_entry_array_allocator_pair_.first(); }
    HAKLE_NODISCARD constexpr const std::size_t&   index_entries_size() const noexcept { return index_entry_pointer_allocator_pair_.first(); }
};

template <class T, HAKLE_CONCEPT( IsAllocator ) Allocator>
struct ConcurrentQueueDefaultTraits {
    static constexpr std::size_t block_size                = 32;
    static constexpr std::size_t InitialBlockPoolSize     = 64;
    static constexpr std::size_t initial_hash_size          = 32;
    static constexpr std::size_t initial_explicit_queue_size = 32;
    static constexpr std::size_t initial_implicit_queue_size = 32;

    using AllocatorType = Allocator;

    using Explicitblock_type = hakle_flags_block<T, block_size>;
    using Implicitblock_type = hakle_counter_block<T, block_size>;

    using explicit_allocator_type = typename HakeAllocatorTraits<AllocatorType>::template rebind_alloc<Explicitblock_type>;
    using implicit_allocator_type = typename HakeAllocatorTraits<AllocatorType>::template rebind_alloc<Implicitblock_type>;

    using explicit_block_manager = hakle_flags_block_manager<T, block_size, explicit_allocator_type>;
    using implicit_block_manager = hakle_counter_block_manager<T, block_size, implicit_allocator_type>;

    static explicit_block_manager make_default_explicit_block_manager( const explicit_allocator_type& InAllocator ) { return explicit_block_manager( InitialBlockPoolSize, InAllocator ); }
    static implicit_block_manager make_default_implicit_block_manager( const implicit_allocator_type& InAllocator ) { return implicit_block_manager( InitialBlockPoolSize, InAllocator ); }

    static implicit_block_manager make_explicit_block_manager( const explicit_allocator_type& InAllocator, std::size_t BlockPoolSize ) { return implicit_block_manager( BlockPoolSize, InAllocator ); }
    static implicit_block_manager make_implicit_block_manager( const explicit_allocator_type& InAllocator, std::size_t BlockPoolSize ) { return implicit_block_manager( BlockPoolSize, InAllocator ); }
};

template <class T, class Allocator = HakleAllocator<T>, HAKLE_CONCEPT( is_concurrent_queue_traits ) Traits = ConcurrentQueueDefaultTraits<T, Allocator>>
class ConcurrentQueue : private Traits {
private:
    struct ProducerListNode;

public:
    struct ProducerToken;
    struct ComsumerToken;

    using Traits::block_size;
    using Traits::initial_explicit_queue_size;
    using Traits::initial_hash_size;
    using Traits::initial_implicit_queue_size;

    using typename Traits::Explicitblock_type;
    using typename Traits::Implicitblock_type;

    using typename Traits::allocator_type;
    using typename Traits::explicit_allocator_type;
    using typename Traits::implicit_allocator_type;

    using typename Traits::explicit_block_manager;
    using typename Traits::implicit_block_manager;
    using typename Traits::InitialBlockPoolSize;

    using Traits::make_default_explicit_block_manager;
    using Traits::make_default_implicit_block_manager;

    using BaseProducer = queue_typeless_base;

    using ExplicitProducer = fast_queue<T, block_size, Allocator, Explicitblock_type, explicit_block_manager>;
    using ImplicitProducer = slow_queue<T, block_size, Allocator, Implicitblock_type, implicit_block_manager>;

    using ExplicitProducerAllocatorTraits = typename HakeAllocatorTraits<allocator_type>::template rebind_traits<ExplicitProducer>;
    using ImplicitProducerAllocatorTraits = typename HakeAllocatorTraits<allocator_type>::template rebind_traits<ImplicitProducer>;
    using ProducerListNodeAllocatorTraits = typename HakeAllocatorTraits<allocator_type>::template rebind_traits<ProducerListNode>;

    using ExplicitProducerAllocatorType = typename HakeAllocatorTraits<allocator_type>::template rebind_alloc<ExplicitProducer>;
    using ImplicitProducerAllocatorType = typename HakeAllocatorTraits<allocator_type>::template rebind_alloc<ImplicitProducer>;
    using ProducerListNodeAllocatorType = typename HakeAllocatorTraits<allocator_type>::template rebind_alloc<ProducerListNode>;

    explicit constexpr ConcurrentQueue( const allocator_type& InAllocator = allocator_type{} )
        : ExplicitProducerAllocatorPair( make_default_explicit_block_manager( explicit_allocator_type( InAllocator ) ), ExplicitProducerAllocatorType( InAllocator ) ),
          ImplicitProducerAllocatorPair( make_default_implicit_block_manager( implicit_allocator_type( InAllocator ) ), ImplicitProducerAllocatorType( InAllocator ) ) {}

    template <class... Args1, class... Args2>
    HAKLE_REQUIRES( Hasmake_implicit_block_manager<Traits>&& has_make_explicit_block_manager<Traits>&& std::invocable<decltype( Traits::make_explicit_block_manager ), Args1&&...>&&
                                                                                                std::invocable<decltype( Traits::make_implicit_block_manager ), Args2&&...> )
    explicit constexpr ConcurrentQueue( std::piecewise_construct_t, std::tuple<Args1...> FirstArgs, std::tuple<Args2...> SecondArgs, const allocator_type& InAllocator )
        :
#if HAKLE_CPP_VERSION >= 17
          ExplicitProducerAllocatorPair(
              std::apply(
                  []( Args1&&... args1, const allocator_type& InAllocator ) { return Traits::make_explicit_block_manager( explicit_allocator_type( InAllocator ), std::forward<Args1>( args1 )... ); },
                  FirstArgs ),
              ExplicitProducerAllocatorType( InAllocator ) ),
          ImplicitProducerAllocatorPair(
              std::apply(
                  []( Args2&&... args2, const allocator_type& InAllocator ) { return Traits::make_implicit_block_manager( implicit_allocator_type( InAllocator ), std::forward<Args2>( args2 )... ); },
                  SecondArgs ),
              ImplicitProducerAllocatorType( InAllocator ) )
#else
          ExplicitManager( hakle::Apply(
              []( Args1&&... args1, const allocator_type& InAllocator ) { return Traits::make_explicit_block_manager( explicit_allocator_type( InAllocator ), std::forward<Args1>( args1 )... ); },
              FirstArgs ) ),
          ImplicitManager( hakle::Apply(
              []( Args2&&... args2, const allocator_type& InAllocator ) { return Traits::make_implicit_block_manager( implicit_allocator_type( InAllocator ), std::forward<Args2>( args2 )... ); },
              SecondArgs ) )
#endif
    {
    }

    explicit constexpr ConcurrentQueue( ConcurrentQueue&& Other ) noexcept
        : ProducerListsHead( std::move( Other.ProducerListsHead ) ), ProducerCount( std::move( Other.ProducerCount ) ), NextExplicitConsumerId( std::move( Other.NextExplicitConsumerId ) ),
          GlobalExplicitConsumerOffset( std::move( Other.GlobalExplicitConsumerOffset ) ), ExplicitProducerAllocatorPair( std::move( Other.ExplicitProducerAllocatorPair ) ),
          ImplicitProducerAllocatorPair( std::move( Other.ImplicitProducerAllocatorPair ) ), ImplicitMap( std::move( Other.ImplicitMap ) ) {}



    constexpr ProducerToken* GetProducerToken() noexcept {
        return new ProducerToken( GetProducerListNode( ProducerType::Explicit ) );
    }


    struct ProducerToken {
        ProducerToken( ProducerToken&& Other ) noexcept : ProducerNode( Other.ProducerNode ) {
            Other.ProducerNode = nullptr;
            if ( ProducerNode != nullptr ) {
                ProducerNode->Token = this;
            }
        }

        ~ProducerToken() {
            if ( ProducerNode != nullptr ) {
                ProducerNode->Token = nullptr;
                ProducerNode->Inactive.store( true, std::memory_order_release );
            }
        }

        ProducerToken( const ProducerToken& )            = delete;
        ProducerToken& operator=( const ProducerToken& ) = delete;

        ProducerToken& operator=( ProducerToken&& Other ) noexcept {
            swap( Other );
            return *this;
        }

        void swap( ProducerToken& Other ) noexcept {
            using std::swap;
            swap( ProducerNode, this->ProducerNode );
            if ( ProducerNode != nullptr ) {
                ProducerNode->Token = this;
            }
            if ( Other.ProducerNode != nullptr ) {
                Other.ProducerNode->Token = &Other;
            }
        }

        [[nodiscard]] bool Valid() const noexcept { return ProducerNode != nullptr; }

    protected:
        ProducerListNode* ProducerNode;
    };

    struct ConsumerToken {
        ConsumerToken( ConsumerToken&& Other ) noexcept
            : InitialOffset( Other.InitialOffset ), LastKnownGlobalOffset( Other.LastKnownGlobalOffset ), ItemsConsumed( Other.ItemsConsumed ), CurrentProducer( Other.CurrentProducer ),
              DesiredProducer( Other.DesiredProducer ) {}

        ConsumerToken& operator=( ConsumerToken&& Other ) noexcept {
            swap( Other );
            return *this;
        }

        void swap( ConsumerToken& Other ) noexcept {
            using std::swap;
            swap( InitialOffset, Other.InitialOffset );
            swap( DesiredProducer, Other.DesiredProducer );
            swap( LastKnownGlobalOffset, Other.LastKnownGlobalOffset );
            swap( CurrentProducer, Other.CurrentProducer );
            swap( DesiredProducer, Other.DesiredProducer );
        }

        ConsumerToken( const ConsumerToken& )            = delete;
        ConsumerToken& operator=( const ConsumerToken& ) = delete;

    private:
        std::uint32_t     InitialOffset;
        std::uint32_t     LastKnownGlobalOffset;
        std::uint32_t     ItemsConsumed;
        ProducerListNode* CurrentProducer;
        ProducerListNode* DesiredProducer;
    };

private:
    enum class ProducerType { Explicit, Implicit };

    struct ProducerListNode {
        ProducerListNode* Next{ nullptr };
        std::atomic<bool> Inactive{ false };
        BaseProducer*     Producer{ nullptr };
        ProducerToken*    Token{ nullptr };
        ConcurrentQueue*  Parent{ nullptr };
        ProducerType      Type;

        constexpr ProducerListNode( BaseProducer* InProducer, ProducerType InType, ConcurrentQueue* InParent ) noexcept : Producer( InProducer ), Type( InType ), Parent( InParent ) {}

        virtual HAKLE_CPP20_CONSTEXPR ~ProducerListNode() = default;
    };

    constexpr ProducerListNode* GetProducerListNode( ProducerType Type ) noexcept {
        for ( ProducerListNode* Node = ProducerListsHead.load( std::memory_order_relaxed ); Node != nullptr; Node = Node->next ) {
            if ( Node->Inactive.load( std::memory_order_relaxed ) && Node->Type == Type ) {
                bool expected = true;
                if ( Node->Inactive.compare_exchange_strong( expected, false, std::memory_order_release, std::memory_order_relaxed ) ) {
                    return Node;
                }
            }
        }

        return AddProducer( CreateProducerListNode( Type ) );
    }

    constexpr void ReclaimProducerLists() noexcept {
        for ( ProducerListNode* Node = ProducerListsHead.load( std::memory_order_relaxed ); Node != nullptr; Node = Node->next ) {
            Node->Parent = this;
        }
    }

    constexpr ProducerListNode* AddProducer( ProducerListNode* Node ) {
        if ( Node == nullptr ) {
            return nullptr;
        }

        ProducerCount.fetch_add( 1, std::memory_order_relaxed );

        ProducerListNode* Head = ProducerListsHead.load( std::memory_order_relaxed );
        do {
            Node->next = Head;
        } while ( !ProducerListsHead.compare_exchange_weak( Head, Node, std::memory_order_release, std::memory_order_relaxed ) );

        return Node;
    }

    template <typename ProducerType Type>
    constexpr ProducerListNode* CreateProducerListNode() {
        BaseProducer* producer;

        if constexpr ( Type == ProducerType::Explicit ) {
            producer = ExplicitProducerAllocatorTraits::allocate( ExplicitProducerAllocator() );
            ExplicitProducerAllocatorTraits::construct( ExplicitProducerAllocator(), producer, initial_explicit_queue_size, ExplicitManager(), value_allocator );
        }
        else {
            producer = ImplicitProducerAllocatorTraits::allocate( ImplicitProducerAllocator() );
            ImplicitProducerAllocatorTraits::construct( ImplicitProducerAllocator(), producer, initial_implicit_queue_size, ImplicitManager(), value_allocator );
        }

        ProducerListNode* node = ProducerListNodeAllocatorTraits::allocate( ProducerListNodeAllocator() );
        ProducerListNodeAllocatorTraits::construct( ProducerListNodeAllocator(), node, producer, Type, this );
        return node;
    }

    // only used in destructor
    constexpr void DeleteProducerListNode( ProducerListNode* Node ) {
        if ( Node == nullptr ) {
            return;
        }

        ProducerCount.fetch_sub( 1, std::memory_order_relaxed );

        if ( Node->Type == ProducerType::Explicit ) {
            ExplicitProducerAllocatorTraits::destory( ExplicitProducerAllocator(), Node->Producer );
            ExplicitProducerAllocatorTraits::deallocate( ExplicitProducerAllocator(), Node->Producer );
        }
        else {
            ImplicitProducerAllocatorTraits::destory( ImplicitProducerAllocator(), Node->Producer );
            ImplicitProducerAllocatorTraits::deallocate( ImplicitProducerAllocator(), Node->Producer );
        }

        ProducerListNodeAllocatorTraits::destory( ProducerListNodeAllocator(), Node );
        ProducerListNodeAllocatorTraits::deallocate( ProducerListNodeAllocator(), Node );
    }

    std::atomic<ProducerListNode*> ProducerListsHead{};
    std::atomic<uint32_t>          ProducerCount{};

    std::atomic<std::uint32_t> NextExplicitConsumerId{};
    std::atomic<std::uint32_t> GlobalExplicitConsumerOffset{};

    allocator_type                 value_allocator{};
    ProducerListNodeAllocatorType ProducerListNodeAllocator{};

    compress_pair<explicit_block_manager, ExplicitProducerAllocatorType> ExplicitProducerAllocatorPair{};
    compress_pair<implicit_block_manager, ImplicitProducerAllocatorType> ImplicitProducerAllocatorPair{};

    constexpr explicit_block_manager& ExplicitManager() noexcept { return ExplicitProducerAllocatorPair.First(); }
    constexpr implicit_block_manager& ImplicitManager() noexcept { return ImplicitProducerAllocatorPair.First(); }

    constexpr ExplicitProducerAllocatorType& ExplicitProducerAllocator() noexcept { return ExplicitProducerAllocatorPair.Second(); }
    constexpr ImplicitProducerAllocatorType& ImplicitProducerAllocator() noexcept { return ImplicitProducerAllocatorPair.Second(); }

    constexpr const explicit_block_manager& ExplicitManager() const noexcept { return ExplicitProducerAllocatorPair.First(); }
    constexpr const implicit_block_manager& ImplicitManager() const noexcept { return ImplicitProducerAllocatorPair.First(); }

    constexpr const ExplicitProducerAllocatorType& ExplicitProducerAllocator() const noexcept { return ExplicitProducerAllocatorPair.Second(); }
    constexpr const ImplicitProducerAllocatorType& ImplicitProducerAllocator() const noexcept { return ImplicitProducerAllocatorPair.Second(); }

    HashTable<details::thread_id_t, ImplicitProducer*, initial_hash_size, details::thread_hash> ImplicitMap{};
};

}  // namespace hakle

#endif  // CONCURRENTQUEUE_H
