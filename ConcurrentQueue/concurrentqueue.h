//
// Created by admin on 25-12-1.
//

#ifndef CONCURRENTQUEUE_H
#define CONCURRENTQUEUE_H

#include "BlockPool.h"
#include "common/utility.h"

namespace hakle {

template <class T, std::size_t BLOCK_SIZE>
struct HakleBlock : BaseBlock<T, BLOCK_SIZE>, FreeListNode<HakleBlock<T, BLOCK_SIZE>> {
    using BLOCK_TYPE = HakleBlock;

    BLOCK_TYPE* Next{ nullptr };
};

template <class BLOCK_TYPE, class Derived>
struct QueueBase {
public:
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
template <class BLOCK_TYPE, class ALLOCATOR_TYPE = HakleAllocator<BLOCK_TYPE>>
class ExplicitQueue : public QueueBase<BLOCK_TYPE, ExplicitQueue<BLOCK_TYPE>> {
public:
    struct IndexEntry;
    struct IndexEntryArray;

    using Block                        = BLOCK_TYPE;
    using BlockAllocatorType           = ALLOCATOR_TYPE;
    using IndexEntryAllocatorType      = typename std::allocator_traits<ALLOCATOR_TYPE>::template rebind_alloc<IndexEntry>;
    using IndexEntryArrayAllocatorType = typename std::allocator_traits<ALLOCATOR_TYPE>::template rebind_alloc<IndexEntryArray>;

private:
    struct IndexEntry {
        std::size_t Base;
        Block*      Next{ nullptr };
    };

    struct IndexEntryArray {
        std::size_t              Size{};
        std::atomic<std::size_t> Tail{};
        IndexEntry*              Entries{ nullptr };
        IndexEntryArray*         Prev{ nullptr };
    };

    std::atomic<IndexEntryArray*> CurrentIndexEntryArray{ nullptr };

    // used by producer only
    std::size_t PO_IndexEntriesUsed{ 0 };
    std::size_t PO_IndexEntriesSize{ 0 };
    std::size_t PO_NextIndexEntry{ 0 };
    IndexEntry* PO_Entries{ nullptr };
};

}  // namespace hakle

#endif  // CONCURRENTQUEUE_H
