//
// Created by admin on 25-12-1.
//

#ifndef CONCURRENTQUEUE_H
#define CONCURRENTQUEUE_H

#include "BlockPool.h"
#include "common/utility.h"

#include <memory>

namespace hakle {

template <class BLOCK_TYPE>
struct BlockTraits {
    constexpr static std::size_t BlockSize = BLOCK_TYPE::BlockSize;
    using ValueType                        = typename BLOCK_TYPE::ValueType;
    using BlockType                        = BLOCK_TYPE;
};

struct BlockCheckPolicy {
    virtual ~BlockCheckPolicy() = default;

    virtual bool IsEmpty() const                                      = 0;
    virtual bool SetEmpty( std::size_t Index )                        = 0;
    virtual bool SetSomeEmpty( std::size_t Index, std::size_t Count ) = 0;
    virtual void SetAllEmpty( std::size_t Index )                     = 0;
    virtual void Reset()                                              = 0;
};

template <std::size_t BLOCK_SIZE>
struct FlagsCheckPolicy : BlockCheckPolicy {
    ~FlagsCheckPolicy() override = default;

    bool IsEmpty() const override {
        for ( auto& Flag : Flags ) {
            if ( !Flag.load( std::memory_order_relaxed ) ) {
                return false;
            }
        }

        std::atomic_thread_fence( std::memory_order_acquire );
        return true;
    }

    //
    bool SetEmpty( std::size_t Index ) override {
        Flags[Index].store( 1, std::memory_order_relaxed );
        return false;
    }

    bool SetSomeEmpty( std::size_t Index, std::size_t Count ) override { return true; }
    void SetAllEmpty( std::size_t Index ) override {}
    void Reset() override {}

    std::array<std::atomic<uint8_t>, BLOCK_SIZE> Flags;
};

struct CounterCheckPolicy : BlockCheckPolicy {
    std::atomic<std::size_t> Counter;
};

enum class BlockMethod { Flags, Counter };

template <class T, std::size_t BLOCK_SIZE, class Policy>
struct HakleBlock : FreeListNode<HakleBlock<T, BLOCK_SIZE, Policy>> {
    using ValueType                        = T;
    using BlockType                        = HakleBlock;
    constexpr static std::size_t BlockSize = BLOCK_SIZE;

    alignas( HAKLE_CACHE_LINE_SIZE ) std::array<T, BLOCK_SIZE> Elements;
    HakleBlock* Next{ nullptr };
};

template <class BLOCK_TYPE, class Derived, class BLOCK_MANAGER_TYPE = HakleBlockManager<BLOCK_TYPE>>
struct QueueBase {
public:
    using BlockTraits                      = BlockTraits<BLOCK_TYPE>;
    constexpr static std::size_t BlockSize = BlockTraits::BlockSize;

    using ValueType = typename BlockTraits::ValueType;
    using BlockType = typename BlockTraits::BlockType;

    using BlockManagerType   = BLOCK_MANAGER_TYPE;
    using BlockAllocatorType = typename BlockManagerType::AllocatorType;

    virtual ~QueueBase() = default;

    template <class T>
    bool Dequeue( T& Element ) noexcept {
        return static_cast<Derived*>( this )->Dequeue( Element );
    }

    std::size_t Size() const noexcept {
        std::size_t Tail = TailIndex.load( std::memory_order_relaxed );
        std::size_t Head = HeadIndex.load( std::memory_order_relaxed );
        return CircularLessThan( Head, Tail ) ? Tail - Head : 0;
    }

    std::size_t GetTail() const noexcept { return TailIndex.load( std::memory_order_relaxed ); }

protected:
    std::atomic<std::size_t> HeadIndex{};
    std::atomic<std::size_t> TailIndex{};
    std::atomic<std::size_t> DequeueAttemptsCount{};
    std::atomic<std::size_t> DequeueFailedCount{};
    BLOCK_TYPE*              TailBlock{ nullptr };
};

// SPMC Queue
template <class BLOCK_TYPE, class BLOCK_MANAGER_TYPE = HakleBlockManager<BLOCK_TYPE>>
class ExplicitQueue : public QueueBase<BLOCK_TYPE, ExplicitQueue<BLOCK_TYPE>, BLOCK_MANAGER_TYPE> {
public:
    struct IndexEntry;
    struct IndexEntryArray;
    using Base = QueueBase<BLOCK_TYPE, ExplicitQueue, BLOCK_MANAGER_TYPE>;

    using Base::BlockAllocatorType;
    using Base::BlockManagerType;
    using Base::BlockSize;
    using Base::BlockType;
    using Base::ValueType;

    // TODO: Tratis
    using IndexEntryAllocatorType      = typename std::allocator_traits<BlockAllocatorType>::template rebind_alloc<IndexEntry>;
    using IndexEntryArrayAllocatorType = typename std::allocator_traits<BlockAllocatorType>::template rebind_alloc<IndexEntryArray>;

    explicit ExplicitQueue( std::size_t InSize, const BLOCK_MANAGER_TYPE& InBlockManager ) : BlockManager( InBlockManager ) {
        std::size_t InitialSize = CeilToPow2( InSize );
        if ( InitialSize < 2 ) {
            InitialSize = 2;
        }
        PO_IndexEntriesSize = InitialSize;

        CreateNewBlockIndexArray( 0 );
    }

    ~ExplicitQueue() override {
        if ( this->TailBlock != nullptr ) {
            // first, we find the first block that's half dequeued
            BlockType* HalfDequeuedBlock = nullptr;
            if ( this->HeadIndex.load( std::memory_order_relaxed ) & ( BlockSize - 1 ) != 0 ) {
                std::size_t i = ( PO_NextIndexEntry - PO_IndexEntriesUsed ) & ( PO_IndexEntriesSize - 1 );
                while ( CircularLessThan( this->PO_Entries[ i ].Base + BlockSize, this->HeadIndex.load( std::memory_order_relaxed ) ) ) {
                    i = ( i + 1 ) & ( PO_IndexEntriesSize - 1 );
                }
                HalfDequeuedBlock = PO_Entries[ i ].InnerBlock;
            }

            // then, we can return back all the blocks
            BlockType* Block = this->TailBlock;
            do {
                Block = Block->Next;

            } while ( Block != this->TailBlock );
        }
    }

private:
    struct IndexEntry {
        std::size_t Base{ 0 };
        BlockType*  InnerBlock{ nullptr };
    };

    struct IndexEntryArray {
        std::size_t              Size{};
        std::atomic<std::size_t> Tail{};
        IndexEntry*              Entries{ nullptr };
        IndexEntryArray*         Prev{ nullptr };

        ~IndexEntryArray() { IndexEntryAllocatorType::Deallocate( Entries, Size ); }
    };

    bool CreateNewBlockIndexArray( std::size_t FilledSlot ) noexcept {
        std::size_t SizeMask = PO_IndexEntriesSize - 1;

        PO_IndexEntriesSize <<= 1;

        IndexEntryArray* NewIndexEntryArray = nullptr;
        IndexEntry*      NewEntries         = nullptr;

        HAKLE_TRY {
            NewIndexEntryArray = IndexEntryArrayAllocatorType::Allocate();
            NewEntries         = IndexEntryAllocatorType::Allocate( PO_IndexEntriesSize );
        }
        HAKLE_CATCH( ... ) {
            if ( NewIndexEntryArray ) {
                IndexEntryArrayAllocatorType::Deallocate( NewIndexEntryArray );
                NewIndexEntryArray = nullptr;
            }
            PO_IndexEntriesSize >>= 1;
            return false;
        }

        // noexcept
        IndexEntryArrayAllocatorType::Construct( NewIndexEntryArray );

        std::size_t j = 0;
        if ( PO_IndexEntriesUsed != 0 ) {
            std::size_t i = ( PO_NextIndexEntry - PO_IndexEntriesUsed ) & SizeMask;
            do {
                NewEntries[ j++ ] = PO_Entries[ i ];
                i                 = ( i + 1 ) & SizeMask;
            } while ( j != PO_NextIndexEntry );
        }

        NewIndexEntryArray->Size    = PO_IndexEntriesSize;
        NewIndexEntryArray->Entries = NewEntries;
        NewIndexEntryArray->Tail.store( FilledSlot - 1, std::memory_order_relaxed );
        NewIndexEntryArray->Prev = CurrentIndexEntryArray.load( std::memory_order_relaxed );

        PO_NextIndexEntry = j;
        PO_Entries        = NewEntries;
        CurrentIndexEntryArray.store( NewIndexEntryArray, std::memory_order_release );
        return true;
    }

    std::atomic<IndexEntryArray*> CurrentIndexEntryArray{ nullptr };

    // Allocator
    BlockManagerType& BlockManager{};

    // used by producer only
    std::size_t PO_IndexEntriesUsed{ 0 };
    std::size_t PO_IndexEntriesSize{ 0 };
    std::size_t PO_NextIndexEntry{ 0 };
    IndexEntry* PO_Entries{ nullptr };
};

}  // namespace hakle

#endif  // CONCURRENTQUEUE_H
