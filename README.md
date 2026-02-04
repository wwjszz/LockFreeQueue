# LockFreeStructures

High-performance header-only lock-free concurrent queue library providing thread-safe multi-producer multi-consumer queue implementation.

## Project Overview

This project implements a high-performance lock-free concurrent queue based on Cameron Desrochers' design from the moodycamel concurrent queue project. Unlike the original implementation, this project supports C++11 to C++20 standards and provides highly customizable architecture with support for custom components. The library is completely header-only for easy integration.

## Features

- **Header-Only**: Single-header distribution for easy integration
- **Thread-Safe**: Lock-free design supporting multi-producer multi-consumer concurrent access
- **High Performance**: Avoids performance bottlenecks of traditional locks, suitable for high-concurrency scenarios
- **Cross-Standard Support**: Compatible with C++11 to C++20 standards
- **Highly Customizable**: Supports custom allocators, block managers, and other components
- **Batch Operations**: Supports bulk enqueue and dequeue operations for better throughput
- **Memory Efficient**: Optimized memory layout to reduce fragmentation

## Core Components

### ConcurrentQueue
Main queue class providing multi-producer multi-consumer concurrent queue functionality:
- Supports explicit producer tokens (ProducerToken) and implicit producers
- Supports consumer tokens (ConsumerToken) for optimized consumption
- Thread-local caching mechanism for improved performance
- Bulk operation support for efficient batch processing

### Customizable Modules
- **Allocator**: Customizable memory allocation strategies
- **Block**: Customizable block structure for data storage
- **BlockManager**: Customizable block management and memory recycling strategies

## Build Requirements

- C++11 or higher (supports C++11-C++20)
- CMake 3.22 or higher
- Compiler supporting atomic operations (GCC, Clang, MSVC)

## Integration

Since this is a header-only library, simply include the necessary headers:

```c++
#include "ConcurrentQueue/ConcurrentQueue.h"
```

No linking required!

## Usage Examples

### Basic Usage
```c++
#include "ConcurrentQueue/ConcurrentQueue.h"
#include <vector>

// Create concurrent queue
hakle::ConcurrentQueue<int> queue;

// Producer thread
auto token = queue.GetProducerToken();
queue.EnqueueWithToken(token, 42);

// Or use implicit producer
queue.Enqueue(100);

// Consumer thread
int value;
if (queue.TryDequeue(value)) {
    // Process value
}

// Use consumer token for optimized consumption
auto consumer_token = queue.GetConsumerToken();
if (queue.TryDequeue(consumer_token, value)) {
    // Process value
}
```

### Bulk Operations
```c++
#include "ConcurrentQueue/ConcurrentQueue.h"
#include <vector>

hakle::ConcurrentQueue<int> queue;

// Bulk enqueue using producer token
auto token = queue.GetProducerToken();
std::vector<int> items = {1, 2, 3, 4, 5};
bool success = queue.EnqueueBulk(token, items.begin(), items.size());

// Bulk enqueue without token (implicit producer)
std::vector<int> more_items = {10, 20, 30, 40, 50};
success = queue.EnqueueBulk(more_items.begin(), more_items.size());

// Bulk dequeue
std::vector<int> output(10);
size_t count = queue.TryDequeueBulk(output.begin(), output.size());

// Process the first 'count' elements in output vector
for (size_t i = 0; i < count; ++i) {
    // Process output[i]
}
```

### Consumer Token Bulk Operations
```c++
#include "ConcurrentQueue/ConcurrentQueue.h"
#include <vector>

hakle::ConcurrentQueue<int> queue;
auto consumer_token = queue.GetConsumerToken();

// Bulk dequeue with consumer token
std::vector<int> results(100);
size_t processed = queue.TryDequeueBulk(consumer_token, results.begin(), results.size());

// Process the first 'processed' items
for (size_t i = 0; i < processed; ++i) {
    // Process results[i]
}
```

## Design Reference

This project draws inspiration from Cameron Desrochers' moodycamel concurrent queue project at https://github.com/cameron314/concurrentqueue/tree/master, with extensions and optimizations including:

- Support for broader C++ standards (C++11-C++20)
- Header-only distribution for easier integration
- Modular architecture supporting custom components
- Enhanced batch operation support for better throughput
- Enhanced type safety and template design

## Performance Characteristics

- Lock-free design avoiding thread blocking
- Optimized memory access patterns
- Support for batch operations for better throughput
- Thread-local caching to reduce contention
- Efficient bulk operations for high-volume scenarios

## Notes

- Suitable for high-concurrency read-write scenarios
- Consider memory management strategies
- May require parameter tuning (like block size) under extreme loads
- Header-only nature means compilation times may increase with usage
- Bulk operations provide significant performance benefits for high-throughput scenarios

## License

See the [LICENSE](./LICENSE) file for licensing information.

## FreeList

由于存在ABA问题，导致如果按照简单的CAS操作，会导致操作在不应该成功的情况下成功。可以通过添加Tag避免ABA问题：

### DCAS

`每次比较时，不仅比较头指针，还需要比较tag。这样在任何在ABA之后执行的操作都会失败。这种方法的问题就是目标架构必须支持足够长度的无锁操作。当然也可以通过压缩指针等方式来使其达到要求。`
```c++
    /**
     * Head的tag是整个链表中最大的。
     * 以H1为例，当H1被get的时候，H1之后的结点的tag被增加为最大的。
     * 如果H1被重新add，那么当前的head的结点的tag一定不会被之前的小，这就保证了两次的tag不一样。
     * 当然在某种极端环境，会存在溢出，不过这种情况几乎不可能发生。
     */
    void Add( Node* InNode ) noexcept {
        HeadPtr CurrentHead = Head().load( std::memory_order_relaxed );
        HeadPtr NewHead{ InNode, 0 };

        do {
            NewHead.Tag = CurrentHead.Tag + 1;
            InNode->FreeListNext.store( CurrentHead.Ptr, std::memory_order_relaxed );
        } while ( !Head().compare_exchange_strong( CurrentHead, NewHead, std::memory_order_relaxed, std::memory_order_relaxed ) );
    }

    Node* TryGet() noexcept {
        HeadPtr CurrentHead = Head().load( std::memory_order_relaxed );
        HeadPtr NewHead;
        while ( CurrentHead.Ptr != nullptr ) {
            NewHead.Ptr = CurrentHead.Ptr->FreeListNext.load( std::memory_order_relaxed );
            NewHead.Tag = CurrentHead.Tag + 1;
            if ( Head().compare_exchange_strong( CurrentHead, NewHead, std::memory_order_relaxed, std::memory_order_relaxed ) ) {
                break;
            }
        }
        return CurrentHead.Ptr;
    }
```

### 引用计数

`引入一个计数，用来表示当前有多少对象在使用当前结点，这样add的时候，如果检测到有对象在使用当前结点，就可以将add的任务交给最后一个离开结点的结点`

```c++
    /**
     * 在链表中且没有对象在使用的结点的引用计数为1
     */
    void Add( Node* InNode ) noexcept {
        // Set AddFlag first
        if ( InNode->FreeListRefs.fetch_add( AddFlag, std::memory_order_relaxed ) == 0 ) {
            Node* CurrentHead = Head().load( std::memory_order_relaxed );
            while ( true ) {
                // first update next then refs
                InNode->FreeListNext.store( CurrentHead, std::memory_order_relaxed );
                InNode->FreeListRefs.store( 1, std::memory_order_release );
                // refs may increase
                if ( !Head().compare_exchange_strong( CurrentHead, InNode, std::memory_order_relaxed, std::memory_order_relaxed ) ) {
                    // if exchange failed, check if someone is using it
                    if ( InNode->FreeListRefs.fetch_add( AddFlag - 1, std::memory_order_release ) == 1 ) {
                        continue;
                    } // else we can let the last user add it
                }
                return;
            }
        }
    }

    Node* TryGet() noexcept {
        Node* CurrentHead = Head().load( std::memory_order_relaxed );
        while ( CurrentHead != nullptr ) {
            Node*    PrevHead = CurrentHead;
            uint32_t Refs     = CurrentHead->FreeListRefs.load( std::memory_order_relaxed );
            if ( ( Refs & RefsMask ) == 0  // check if already taken or adding
                 || ( !CurrentHead->FreeListRefs.compare_exchange_strong( Refs, Refs + 1, std::memory_order_acquire,
                                                                          std::memory_order_relaxed ) ) )  // try add refs
            {
                CurrentHead = Head().load( std::memory_order_relaxed );
                continue;
            }

            // try Taken
            Node* Next = CurrentHead->FreeListNext.load( std::memory_order_relaxed );
            if ( Head().compare_exchange_strong( CurrentHead, Next, std::memory_order_relaxed, std::memory_order_relaxed ) ) {
                // taken success, decrease refcount twice, for our and list's ref
                CurrentHead->FreeListRefs.fetch_add( -2, std::memory_order_relaxed );
                return CurrentHead;
            }

            // taken failed, decrease refcount
            Refs = PrevHead->FreeListRefs.fetch_add( -1, std::memory_order_relaxed );
            if ( Refs == AddFlag + 1 ) {
                // no one is using it, add it back
                InnerAdd( PrevHead );
            }
        }
        return nullptr;
    }
```

## LockFreeHashTable

### LockFree LinearSearch

