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

```cpp
#include "ConcurrentQueue/ConcurrentQueue.h"
```

No linking required!

## Usage Examples

### Basic Usage
```cpp
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
```cpp
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
```cpp
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