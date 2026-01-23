//
// Created by wwjszz on 25-11-24.
//

#ifndef BLOCKMANAGER_H
#define BLOCKMANAGER_H

#include <atomic>
#include <cstddef>

#include "block.h"
#include "common/allocator.h"
#include "common/compress_pair.h"

#include <memory>

// block_pool + free_list
namespace hakle {

enum class alloc_mode;

#ifndef HAKLE_USE_CONCEPT
template <class BLOCK_TYPE>
struct block_traits {
    static_assert( BLOCK_TYPE::block_size > 1 && ( BLOCK_TYPE::block_size & ( BLOCK_TYPE::block_size - 1 ) ) == 0, "block_size must be power of 2 and greater than 1" );

    constexpr static std::size_t block_size = BLOCK_TYPE::block_size;
    using value_type                        = typename BLOCK_TYPE::value_type;
    using block_type                        = BLOCK_TYPE;
};
#else
template <class T, template <class> class Constraint>
concept is_atomic_with = requires {
    typename T::value_type;
    requires Constraint<typename T::value_type>::value;
    requires std::same_as<std::remove_cvref_t<T>, std::atomic<typename T::value_type>>;
};

template <class T>
struct is_integer : std::bool_constant<std::integral<T>> {};

template <class T>
struct is_pointer : std::bool_constant<std::is_pointer<T>::value> {};

template <class T>
concept is_free_list_node = requires( T& t ) {
    requires is_atomic_with<std::remove_reference_t<decltype( t.free_list_refs )>, is_integer>;
    requires is_atomic_with<std::remove_reference_t<decltype( t.free_list_next )>, is_pointer>;
};

template <class T>
concept is_block_manager = requires( T& t, alloc_mode Mode, typename T::block_type* p ) {
    { t.requisition_block( Mode ) } -> std::same_as<typename T::block_type*>;
    t.return_block( p );
    t.return_blocks( p );
};

template <class BLOCK_TYPE, class T>
concept check_block_manager = is_block<BLOCK_TYPE> && std::same_as<BLOCK_TYPE, typename T::block_type>;

#endif

// TODO: position?
enum class alloc_mode {
    can_alloc,
    cannot_alloc
};

struct memory_base {
    bool has_owner{ false };
};

template <class T>
struct free_list_node : memory_base {
    std::atomic<uint32_t> free_list_refs{ 0 };
    std::atomic<T*>       free_list_next{ 0 };
};

template <HAKLE_CONCEPT( is_free_list_node ) node, class ALLOCATOR_TYPE = std::allocator<node>>
class free_list {
public:
    static_assert( std::is_base_of<free_list_node<node>, node>::value, "node must be derived from FreeListNode<node>" );

    using allocator_type   = ALLOCATOR_TYPE;
    using allocator_traits = std::allocator_traits<allocator_type>;

    constexpr explicit free_list( const allocator_type& allocator = allocator_type{} ) : allocator_pair_( nullptr, allocator ) {}

    HAKLE_CPP20_CONSTEXPR ~free_list() {
        node* current_node = head().load( std::memory_order_relaxed );
        while ( current_node != nullptr ) {
            node* Next = current_node->free_list_next.load( std::memory_order_relaxed );
            if ( !current_node->has_owner ) {
                allocator_traits::destroy( allocator(), current_node );
                allocator_traits::deallocate( allocator(), current_node );
            }
            current_node = Next;
        }
    }

    constexpr void add( node* in_node ) noexcept {
        // Set AddFlag first
        if ( in_node->free_list_refs.fetch_add( AddFlag, std::memory_order_relaxed ) == 0 ) {
            inner_add( in_node );
        }
    }

    constexpr node* try_get() noexcept {
        node* current_head = head().load( std::memory_order_relaxed );
        while ( current_head != nullptr ) {
            node*    prev_head = current_head;
            uint32_t refs     = current_head->free_list_refs.load( std::memory_order_relaxed );
            if ( ( refs & RefsMask ) == 0  // check if already taken or adding
                 || ( !current_head->free_list_refs.compare_exchange_strong( refs, refs + 1, std::memory_order_acquire,
                                                                          std::memory_order_relaxed ) ) )  // try add refs
            {
                current_head = head().load( std::memory_order_relaxed );
                continue;
            }

            // try Taken
            node* next = current_head->free_list_next.load( std::memory_order_relaxed );
            if ( head().compare_exchange_strong( current_head, next, std::memory_order_relaxed, std::memory_order_relaxed ) ) {
                // taken success, decrease refcount twice, for our and list's ref
                current_head->free_list_refs.fetch_add( -2, std::memory_order_relaxed );
                return current_head;
            }

            // taken failed, decrease refcount
            refs = prev_head->free_list_refs.fetch_add( -1, std::memory_order_relaxed );
            if ( refs == AddFlag + 1 ) {
                // no one is using it, add it back
                inner_add( prev_head );
            }
        }
        return nullptr;
    }

    // NOTE: This is intentionally not thread safe; it is up to the user to synchronize this call.
    // only useful when there is no contention (e.g. destruction)
    constexpr node* get_head() const noexcept { return head().load( std::memory_order_relaxed ); }

private:
    // add when ref count == 0
    constexpr void inner_add( node* in_node ) noexcept {
        node* current_head = head().load( std::memory_order_relaxed );
        while ( true ) {
            // first update next then refs
            in_node->free_list_next.store( current_head, std::memory_order_relaxed );
            in_node->free_list_refs.store( 1, std::memory_order_release );
            if ( !head().compare_exchange_strong( current_head, in_node, std::memory_order_relaxed, std::memory_order_relaxed ) ) {
                // check if someone already using it
                if ( in_node->free_list_refs.fetch_add( AddFlag - 1, std::memory_order_release ) == 1 ) {
                    continue;
                }
            }
            return;
        }
    }

    static constexpr uint32_t RefsMask = 0x7fffffff;
    static constexpr uint32_t AddFlag  = 0x80000000;

    constexpr allocator_type&            allocator() noexcept { return allocator_pair_.second(); }
    constexpr const allocator_type&      allocator() const noexcept { return allocator_pair_.second(); }
    constexpr std::atomic<node*>&       head() noexcept { return allocator_pair_.first(); }
    constexpr const std::atomic<node*>& head() const noexcept { return allocator_pair_.first(); }

    // compressed allocator
    compress_pair<std::atomic<node*>, allocator_type> allocator_pair_{};
};

template <HAKLE_CONCEPT( is_block ) BLOCK_TYPE, class ALLOCATOR_TYPE = std::allocator<BLOCK_TYPE>>
class block_pool {
public:
    using allocator_type   = ALLOCATOR_TYPE;
    using allocator_traits = std::allocator_traits<allocator_type>;

    constexpr explicit block_pool( std::size_t in_size, const allocator_type& in_allocator = allocator_type{} ) : allocator_pair_{ in_size, in_allocator } {
        head_ = allocator_traits::Allocate( allocator(), size() );
        for ( std::size_t i = 0; i < size(); i++ ) {
            allocator_traits::Construct( allocator(), head_ + i );
            head_[ i ].has_owner = true;
        }
    }

    HAKLE_CPP20_CONSTEXPR ~block_pool() {
        allocator_traits::destroy( allocator(), head_, size() );
        allocator_traits::deallocate( allocator(), head_, size() );
    }

    constexpr BLOCK_TYPE* get_block() noexcept {
        if ( index_.load( std::memory_order_relaxed ) >= size() )
            return nullptr;

        std::size_t current_index = index_.fetch_add( 1, std::memory_order_relaxed );
        return current_index < size() ? ( head_ + current_index ) : nullptr;
    }

private:
    constexpr allocator_type&                     allocator() noexcept { return allocator_pair_.Second(); }
    constexpr const allocator_type&               allocator() const noexcept { return allocator_pair_.Second(); }
    constexpr std::size_t&                       size() noexcept { return allocator_pair_.First(); }
    HAKLE_NODISCARD constexpr const std::size_t& size() const noexcept { return allocator_pair_.First(); }

    // compressed allocator
    compress_pair<std::size_t, allocator_type> allocator_pair_{};
    std::atomic<std::size_t>                 index_{ 0 };
    BLOCK_TYPE*                              head_{ nullptr };
};

template <HAKLE_CONCEPT( is_block ) BLOCK_TYPE, class ALLOCATOR_TYPE>
class block_manager_base : private compress_pair_elem<ALLOCATOR_TYPE, 0> {
public:
    using allocator_type        = ALLOCATOR_TYPE;
    using block_allocator_traits = std::allocator_traits<allocator_type>;
    using block_type            = BLOCK_TYPE;
#ifdef HAKLE_USE_CONCEPT
    constexpr static std::size_t block_size = block_type::block_size;
    using value_type                        = typename block_type::value_type;
#else
    using block_traits                      = BlockTraits<block_type>;
    constexpr static std::size_t block_size = block_traits::block_size;
    using value_type                        = typename block_traits::value_type;
#endif

    constexpr block_manager_base() = default;
    constexpr explicit block_manager_base( const allocator_type& allocator = allocator_type{} ) : base( allocator ) {}
    virtual ~block_manager_base() = default;

    using alloc_mode = hakle::alloc_mode;

    virtual HAKLE_CPP20_CONSTEXPR block_type* requisition_block( alloc_mode InMode ) = 0;
    virtual HAKLE_CPP20_CONSTEXPR void       return_blocks( block_type* InBlock )   = 0;
    virtual HAKLE_CPP20_CONSTEXPR void       return_block( block_type* InBlock )    = 0;

    constexpr allocator_type&       allocator() noexcept { return Base::get(); }
    constexpr const allocator_type& allocator() const noexcept { return Base::get(); }

private:
    using base = compress_pair_elem<ALLOCATOR_TYPE, 0>;
};

// We set a block pool and a free list
template <HAKLE_CONCEPT( is_block ) BLOCK_TYPE, class ALLOCATOR_TYPE = std::allocator_traits<BLOCK_TYPE>>
class hakle_block_manager : public block_manager_base<BLOCK_TYPE, ALLOCATOR_TYPE> {
public:
    using base_manager = block_manager_base<BLOCK_TYPE, ALLOCATOR_TYPE>;
    // TODO:
    // using typename base_manager::allocator_type;
    using allocator_type = typename base_manager::allocator_type;

    using typename base_manager::block_allocator_traits;
    using typename base_manager::block_type;
    using typename base_manager::value_type;

    using alloc_mode = typename base_manager::alloc_mode;

    constexpr explicit hakle_block_manager( std::size_t in_size, const allocator_type& in_allocator = allocator_type{} ) : base_manager( in_allocator ), pool_( in_size, in_allocator ), list_( in_allocator ) {}
    HAKLE_CPP20_CONSTEXPR ~hakle_block_manager() override = default;

    constexpr block_type* requisition_block( alloc_mode Mode ) override {
        block_type* block = pool_.get_block();
        if ( block != nullptr ) {
            return block;
        }

        block = list_.try_get();
        if ( block != nullptr ) {
            return block;
        }

        // TODO: constexpr
        // HAKLE_CONSTEXPR_IF( Mode == alloc_mode::cannot_alloc ) { return nullptr; }
        if ( Mode == alloc_mode::cannot_alloc ) {
            return nullptr;
        }
        else {
            // When alloc mode is can_alloc, we allocate a new block
            // If user finishes using the block, it must be returned to the free list
            block_type* new_block = block_allocator_traits::allocate( this->allocator() );
            block_allocator_traits::Construct( this->allocator(), new_block );
            return new_block;
        }
    }

    constexpr void return_block( block_type* in_block ) override { list_.add( in_block ); }
    constexpr void return_blocks( block_type* in_block ) override {
        while ( in_block != nullptr ) {
            block_type* Next = in_block->Next;
            list_.add( in_block );
            in_block = Next;
        }
    }

private:
    block_pool<block_type, allocator_type> pool_;
    free_list<block_type, allocator_type>  list_;
};

template <class T, std::size_t BLOCK_SIZE, class ALLOCATOR_TYPE = std::allocator<hakle_flags_block<T, BLOCK_SIZE>>>
using hakle_flags_block_manager = hakle_block_manager<hakle_flags_block<T, BLOCK_SIZE>>;

template <class T, std::size_t BLOCK_SIZE, class ALLOCATOR_TYPE = std::allocator<hakle_counter_block<T, BLOCK_SIZE>>>
using hakle_counter_block_manager = hakle_block_manager<hakle_counter_block<T, BLOCK_SIZE>>;

}  // namespace hakle

#endif  // BLOCKMANAGER_H
