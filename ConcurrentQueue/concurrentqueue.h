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

}  // namespace hakle

#endif  // CONCURRENTQUEUE_H
