#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "../ConcurrentQueue/BlockManager.h"
#include "../ConcurrentQueue/HashTable.h"

#include <iostream>

#include "gtest/gtest.h"

using namespace hakle;
#include "common/allocator.h"
#include <mutex>

inline std::mutex& GetMutex() {
    static std::mutex print_mtx;
    return print_mtx;
}

template <class Tp>
class MyAllocator {
public:
    using ValueType      = Tp;
    using Pointer        = Tp*;
    using ConstPointer   = const Tp*;
    using Reference      = Tp&;
    using ConstReference = const Tp&;
    using SizeType       = size_t;
    using DifferenceType = std::ptrdiff_t;

    constexpr MyAllocator() noexcept = default;

    struct Info {
        std::atomic<int> ConstructCount{ 0 };
        std::atomic<int> AllocateCount{ 0 };
        Info() {}
        ~Info() {
            if ( AllocateCount.load() != 0 || ConstructCount.load() != 0 ) {
                std::lock_guard<std::mutex> lock( GetMutex() );
                fprintf( stderr, "\033[31m[%s] Quit with Allocate=%d, Construct=%d\033[0m\n", typeid( Tp ).name(), AllocateCount.load(), ConstructCount.load() );
                fflush( stderr );
            }
        }
    };

    static Info info;

    template <class Up>
    explicit constexpr MyAllocator( const MyAllocator<Up>& ) noexcept {}

    template <class Up>
    explicit constexpr MyAllocator( const MyAllocator<Up>&& ) noexcept {}

    template <class Up>
    constexpr MyAllocator& operator=( const MyAllocator<Up>& ) noexcept {
        return *this;
    }

    template <class Up>
    constexpr MyAllocator& operator=( const MyAllocator<Up>&& ) noexcept {
        return *this;
    }

    HAKLE_CPP14_CONSTEXPR void swap( MyAllocator& ) noexcept {}

    constexpr Pointer Allocate() {
        ++info.AllocateCount;
        return HAKLE_OPERATOR_NEW( Tp );
    }
    constexpr Pointer Allocate( SizeType n ) {
        info.AllocateCount += static_cast<int>( n );
        return HAKLE_OPERATOR_NEW_ARRAY( Tp, n );
    }

    constexpr void Deallocate( Pointer ptr ) noexcept {
        HAKLE_OPERATOR_DELETE( ptr );
        --info.AllocateCount;
    }
    constexpr void Deallocate( Pointer ptr, HAKLE_MAYBE_UNUSED SizeType n ) noexcept {
        Deallocate( ptr );
        info.AllocateCount -= static_cast<int>( n - 1 );
    }

    template <class... Args>
    constexpr void Construct( Pointer ptr, Args&&... args ) {
        HAKLE_CONSTRUCT( ptr, std::forward<Args>( args )... );
        ++info.ConstructCount;
    }

    constexpr void Destroy( Pointer ptr ) noexcept {
        HAKLE_DESTROY( ptr );
        --info.ConstructCount;
    }
    constexpr void Destroy( Pointer ptr, SizeType n ) noexcept {
        HAKLE_DESTROY_ARRAY( ptr, n );
        info.ConstructCount -= static_cast<int>( n );
    }
    constexpr void Destroy( Pointer first, Pointer last ) noexcept { Destroy( first, last - first ); }
};

template <class Tp>
typename MyAllocator<Tp>::Info MyAllocator<Tp>::info{};

template <class T, std::size_t BLOCK_SIZE>
struct MyNode : public FreeListNode<MyNode<T, BLOCK_SIZE>>, CounterCheckPolicy<BLOCK_SIZE> {
    using ValueType                        = T;
    constexpr static std::size_t BlockSize = BLOCK_SIZE;

    HAKLE_CPP14_CONSTEXPR T* operator[]( std::size_t Index ) noexcept { return reinterpret_cast<T*>( Elements.data() ) + Index; }
    constexpr const T*       operator[]( std::size_t Index ) const noexcept { return reinterpret_cast<T*>( Elements.data() ) + Index; }

    unsigned char Elements[ sizeof( T ) * BLOCK_SIZE ]{};

    MyNode* Next{ nullptr };
};

static_assert( IsBlock<MyNode<int, 32>> );

// 测试用的键值类型
using TestHashTable = HashTable<uint32_t, uint32_t, 8, core::Hash<uint32_t>, MyAllocator<Pair<std::atomic<uint32_t>, std::atomic<uint32_t>>>>;

class HashTableTest : public ::testing::Test {
protected:
    void SetUp() override {
        // FreeList<MyNode> t;
        table = new TestHashTable( UINT32_MAX );
    }

    void TearDown() override { delete table; }

    TestHashTable* table;
};

// 测试基本插入和查找
TEST_F( HashTableTest, BasicInsertAndGet ) {
    uint32_t key      = 123;
    uint32_t value    = 456;
    uint32_t outValue = 0;

    // 插入数据
    auto status = table->GetOrAdd( key, outValue, value );
    EXPECT_EQ( status, HashTableStatus::ADD_SUCCESS );
    EXPECT_EQ( outValue, value );

    // 查找数据
    bool found = table->Get( key, outValue );
    EXPECT_TRUE( found );
    EXPECT_EQ( outValue, value );
}

// 测试Set方法
TEST_F( HashTableTest, SetMethod ) {
    uint32_t key      = 123;
    uint32_t value1   = 456;
    uint32_t value2   = 789;
    uint32_t outValue = 0;

    // 先插入数据
    auto status = table->GetOrAdd( key, outValue, value1 );
    EXPECT_EQ( status, HashTableStatus::ADD_SUCCESS );

    // 使用Set更新值
    bool setResult = table->Set( key, value2 );
    EXPECT_TRUE( setResult );

    // 验证更新后的值
    bool found = table->Get( key, outValue );
    EXPECT_TRUE( found );
    EXPECT_EQ( outValue, value2 );
}

// 测试Set不存在的键
TEST_F( HashTableTest, SetNonExistentKey ) {
    uint32_t key   = 123;
    uint32_t value = 456;

    // 对不存在的键调用Set
    bool setResult = table->Set( key, value );
    EXPECT_TRUE( setResult );  // Set应该会自动插入新键

    // 验证插入的值
    uint32_t outValue = 0;
    bool     found    = table->Get( key, outValue );
    EXPECT_TRUE( found );
    EXPECT_EQ( outValue, value );
}

// 测试GetSize方法
TEST_F( HashTableTest, GetSizeMethod ) {
    EXPECT_EQ( table->GetSize(), 0 );

    // 插入一些数据
    for ( uint32_t i = 0; i < 5; ++i ) {
        uint32_t outValue = 0;
        table->GetOrAdd( i, outValue, i * 100 );
    }

    EXPECT_EQ( table->GetSize(), 5 );

    // 插入重复键不应该改变大小
    uint32_t outValue = 0;
    table->GetOrAdd( 1, outValue, 999 );
    EXPECT_EQ( table->GetSize(), 5 );  // 大小应该不变

    // 使用Set更新现有键不应该改变大小
    table->Set( 2, 888 );
    EXPECT_EQ( table->GetSize(), 5 );  // 大小应该不变

    // 使用Set插入新键应该增加大小
    table->Set( 10, 1000 );
    EXPECT_EQ( table->GetSize(), 6 );
}

// 测试查找不存在的键
TEST_F( HashTableTest, GetNonExistentKey ) {
    uint32_t outValue = 0;
    bool     found    = table->Get( 999, outValue );
    EXPECT_FALSE( found );
}

// 测试重复插入相同的键
TEST_F( HashTableTest, DuplicateKeyInsertion ) {
    uint32_t key      = 123;
    uint32_t value1   = 456;
    uint32_t value2   = 789;
    uint32_t outValue = 0;

    // 第一次插入
    auto status1 = table->GetOrAdd( key, outValue, value1 );
    EXPECT_EQ( status1, HashTableStatus::ADD_SUCCESS );
    EXPECT_EQ( outValue, value1 );

    // 第二次插入相同的键
    auto status2 = table->GetOrAdd( key, outValue, value2 );
    EXPECT_EQ( status2, HashTableStatus::GET_SUCCESS );
    EXPECT_EQ( outValue, value1 );  // 应该返回第一次插入的值
}

// 测试哈希冲突处理
TEST_F( HashTableTest, HandleHashCollisions ) {
    // 插入多个可能产生冲突的键
    std::vector<uint32_t> keys = { 1, 9, 17 };  // 这些键在初始容量8下可能冲突

    for ( uint32_t i = 0; i < keys.size(); ++i ) {
        uint32_t outValue = 0;
        auto     status   = table->GetOrAdd( keys[ i ], outValue, i * 100 );
        EXPECT_EQ( status, HashTableStatus::ADD_SUCCESS );
        EXPECT_EQ( outValue, i * 100 );
    }

    // 验证所有键都能正确找到
    for ( uint32_t i = 0; i < keys.size(); ++i ) {
        uint32_t outValue = 0;
        bool     found    = table->Get( keys[ i ], outValue );
        EXPECT_TRUE( found );
        EXPECT_EQ( outValue, i * 100 );
    }
}

// 测试哈希表扩容
TEST_F( HashTableTest, HashTableResize ) {
    // 插入足够多的数据触发扩容（初始容量8，扩容阈值约4-6）
    for ( uint32_t i = 0; i < 10; ++i ) {
        uint32_t outValue = 0;
        auto     status   = table->GetOrAdd( i, outValue, i * 1000 );
        EXPECT_EQ( status, HashTableStatus::ADD_SUCCESS );
    }

    // 验证所有数据在扩容后仍然可访问
    for ( uint32_t i = 0; i < 10; ++i ) {
        uint32_t outValue = 0;
        bool     found    = table->Get( i, outValue );
        EXPECT_TRUE( found );
        EXPECT_EQ( outValue, i * 1000 );
    }

    // 验证大小正确
    EXPECT_EQ( table->GetSize(), 10 );
}

// 高并发插入测试 - 大量线程同时插入不同的键
TEST_F( HashTableTest, HighConcurrencyInsertDifferentKeys ) {
    const int        num_threads    = 16;
    const int        num_operations = 500;
    std::atomic<int> add_success{ 0 };
    std::atomic<int> get_success{ 0 };

    std::vector<std::thread> threads;

    auto worker = [ num_operations, this, &add_success, &get_success ]( int thread_id ) {
        for ( int i = 0; i < num_operations; ++i ) {
            uint32_t key      = thread_id * num_operations + i;
            uint32_t value    = key * 10;
            uint32_t outValue = 0;

            auto status = table->GetOrAdd( key, outValue, value );
            if ( status == HashTableStatus::ADD_SUCCESS ) {
                add_success.fetch_add( 1, std::memory_order_relaxed );
            }
            else if ( status == HashTableStatus::GET_SUCCESS ) {
                get_success.fetch_add( 1, std::memory_order_relaxed );
            }

            // 验证插入的数据
            bool found = table->Get( key, outValue );
            if ( found ) {
                EXPECT_EQ( outValue, value );
            }
        }
    };

    for ( int i = 0; i < num_threads; ++i ) {
        threads.emplace_back( worker, i );
    }

    for ( auto& t : threads ) {
        t.join();
    }

    std::vector<std::thread> get_threads;

    for ( int i = 0; i < num_threads; ++i ) {
        get_threads.emplace_back( worker, i );
    }

    for ( auto& t : get_threads ) {
        t.join();
    }

    std::cout << "Add successes: " << add_success.load() << ", Get successes: " << get_success.load() << ", Total size: " << table->GetSize() << std::endl;

    // 验证所有数据都存在
    for ( int i = 0; i < num_threads; ++i ) {
        for ( int j = 0; j < num_operations; ++j ) {
            uint32_t key      = i * num_operations + j;
            uint32_t outValue = 0;
            bool     found    = table->Get( key, outValue );
            EXPECT_TRUE( found );
            EXPECT_EQ( outValue, key * 10 );
        }
    }
}

// 测试多线程同时插入相同的键
TEST_F( HashTableTest, ConcurrentDuplicateKeyInsertion ) {
    const int        num_threads   = 8;
    const uint32_t   duplicate_key = 12345;
    std::atomic<int> add_success{ 0 };
    std::atomic<int> get_success{ 0 };

    std::vector<std::thread> threads;

    auto worker = [ this, duplicate_key, &add_success, &get_success ]() {
        for ( int i = 0; i < 100; ++i ) {
            uint32_t outValue = 0;
            uint32_t value    = i * 100;  // 每个线程尝试插入不同的值

            auto status = table->GetOrAdd( duplicate_key, outValue, value );
            if ( status == HashTableStatus::ADD_SUCCESS ) {
                add_success.fetch_add( 1, std::memory_order_relaxed );
            }
            else if ( status == HashTableStatus::GET_SUCCESS ) {
                get_success.fetch_add( 1, std::memory_order_relaxed );
            }

            std::this_thread::yield();  // 增加竞争机会
        }
    };

    for ( int i = 0; i < num_threads; ++i ) {
        threads.emplace_back( worker );
    }

    for ( auto& t : threads ) {
        t.join();
    }

    // 应该只有一个线程成功插入，其他线程应该获取到已存在的值
    EXPECT_EQ( add_success.load(), 1 );
    EXPECT_EQ( get_success.load(), num_threads * 100 - 1 );

    // 验证插入的值存在
    uint32_t outValue = 0;
    bool     found    = table->Get( duplicate_key, outValue );
    EXPECT_TRUE( found );
}

// 测试多线程Set操作
TEST_F( HashTableTest, ConcurrentSetOperations ) {
    const int        num_threads   = 8;
    const uint32_t   key           = 12345;
    const uint32_t   initial_value = 100;
    std::atomic<int> set_operations{ 0 };

    // 先插入初始值
    uint32_t outValue = 0;
    table->GetOrAdd( key, outValue, initial_value );

    std::vector<std::thread> threads;

    auto worker = [ this, key, &set_operations ]( int thread_id ) {
        for ( int i = 0; i < 50; ++i ) {
            uint32_t new_value = thread_id * 100 + i;
            bool     result    = table->Set( key, new_value );
            if ( result ) {
                set_operations.fetch_add( 1, std::memory_order_relaxed );
            }
            std::this_thread::yield();
        }
    };

    for ( int i = 0; i < num_threads; ++i ) {
        threads.emplace_back( worker, i );
    }

    for ( auto& t : threads ) {
        t.join();
    }

    EXPECT_GT( set_operations.load(), 0 );

    // 验证最终值存在（可能是任意线程设置的值）
    uint32_t finalValue = 0;
    bool     found      = table->Get( key, finalValue );
    EXPECT_TRUE( found );
    // 注意：由于并发Set，最终值是不确定的
}

// 读写混合测试
TEST_F( HashTableTest, ReadWriteMixed ) {
    const int         writer_threads = 4;
    const int         reader_threads = 4;
    const int         num_operations = 1000;
    std::atomic<bool> stop_reading{ false };
    std::atomic<int>  read_success{ 0 };
    std::atomic<int>  write_success{ 0 };

    // 先插入一些初始数据
    for ( int i = 0; i < 500; ++i ) {
        uint32_t outValue = 0;
        table->GetOrAdd( i, outValue, i * 100 );
    }

    std::vector<std::thread> writers;
    std::vector<std::thread> readers;

    // 写线程 - 混合使用GetOrAdd和Set
    for ( int i = 0; i < writer_threads; ++i ) {
        writers.emplace_back(
            [ this, &write_success, num_operations ]( int thread_id ) {
                for ( int j = 0; j < num_operations; ++j ) {
                    uint32_t key      = 1000 + thread_id * num_operations + j;
                    uint32_t outValue = 0;

                    if ( j % 2 == 0 ) {
                        // 使用GetOrAdd插入新键
                        auto status = table->GetOrAdd( key, outValue, key * 10 );
                        if ( status != HashTableStatus::FAILED ) {
                            write_success.fetch_add( 1, std::memory_order_relaxed );
                        }
                    }
                    else {
                        // 使用Set更新现有键或插入新键
                        if ( table->Set( key, key * 20 ) ) {
                            write_success.fetch_add( 1, std::memory_order_relaxed );
                        }
                    }
                    std::this_thread::sleep_for( std::chrono::microseconds( 10 ) );
                }
            },
            i );
    }

    // 读线程
    for ( int i = 0; i < reader_threads; ++i ) {
        readers.emplace_back( [ this, &read_success, &stop_reading ]() {
            std::random_device              rd;
            std::mt19937                    gen( rd() );
            std::uniform_int_distribution<> dis( 0, 1500 );

            while ( !stop_reading.load( std::memory_order_acquire ) ) {
                uint32_t key      = dis( gen );
                uint32_t outValue = 0;
                if ( table->Get( key, outValue ) ) {
                    read_success.fetch_add( 1, std::memory_order_relaxed );
                }
                std::this_thread::sleep_for( std::chrono::microseconds( 5 ) );
            }
        } );
    }

    // 等待写线程完成
    for ( auto& t : writers ) {
        t.join();
    }

    // 停止读线程
    stop_reading.store( true, std::memory_order_release );
    for ( auto& t : readers ) {
        t.join();
    }

    EXPECT_GT( read_success.load(), 0 );
    EXPECT_GT( write_success.load(), 0 );

    std::cout << "Mixed test - Reads: " << read_success.load() << ", Writes: " << write_success.load() << ", Final size: " << table->GetSize() << std::endl;
}

// 压力测试 - 持续的高并发操作
TEST_F( HashTableTest, StressTest ) {
    const int            num_threads      = 12;
    const int            duration_seconds = 3;
    std::atomic<bool>    stop{ false };
    std::atomic<int64_t> total_operations{ 0 };
    std::atomic<int64_t> add_operations{ 0 };
    std::atomic<int64_t> get_operations{ 0 };
    std::atomic<int64_t> set_operations{ 0 };

    std::vector<std::thread> threads;

    auto worker = [ duration_seconds, this, &stop, &total_operations, &add_operations, &get_operations, &set_operations ]() {
        std::random_device              rd;
        std::mt19937                    gen( rd() );
        std::uniform_int_distribution<> op_dis( 0, 100 );
        std::uniform_int_distribution<> key_dis( 0, 10000 );

        auto start_time = std::chrono::steady_clock::now();

        while ( !stop.load( std::memory_order_acquire ) ) {
            int      op       = op_dis( gen );
            uint32_t key      = key_dis( gen );
            uint32_t outValue = 0;

            if ( op < 40 ) {  // 40% 插入操作
                auto status = table->GetOrAdd( key, outValue, key * 10 );
                if ( status != HashTableStatus::FAILED ) {
                    add_operations.fetch_add( 1, std::memory_order_relaxed );
                }
            }
            else if ( op < 70 ) {  // 30% 查找操作
                table->Get( key, outValue );
                get_operations.fetch_add( 1, std::memory_order_relaxed );
            }
            else {  // 30% Set操作
                if ( table->Set( key, key * 20 ) ) {
                    set_operations.fetch_add( 1, std::memory_order_relaxed );
                }
            }

            total_operations.fetch_add( 1, std::memory_order_relaxed );

            if ( total_operations.load() % 1000 == 0 ) {
                auto current_time = std::chrono::steady_clock::now();
                if ( std::chrono::duration_cast<std::chrono::seconds>( current_time - start_time ).count() >= duration_seconds ) {
                    break;
                }
            }
        }
    };

    for ( int i = 0; i < num_threads; ++i ) {
        threads.emplace_back( worker );
    }

    std::this_thread::sleep_for( std::chrono::seconds( duration_seconds ) );
    stop.store( true, std::memory_order_release );

    for ( auto& t : threads ) {
        t.join();
    }

    std::cout << "Stress test completed: " << total_operations.load() << " operations (" << add_operations.load() << " adds, " << get_operations.load() << " gets, " << set_operations.load() << " sets) in " << duration_seconds
              << " seconds. Final size: " << table->GetSize() << std::endl;
    EXPECT_GT( total_operations.load(), 0 );
}

// 测试使用分配函数的版本
TEST_F( HashTableTest, GetOrAddByFunc ) {
    const int num_threads    = 4;
    const int num_operations = 100;

    // 使用指针类型的哈希表
    using PtrHashTable = HashTable<uint32_t, uint32_t*, 8>;
    PtrHashTable ptrTable( UINT32_MAX );

    std::vector<std::thread> threads;
    std::vector<uint32_t*>   allocated_pointers;
    std::mutex               mutex;

    auto allocateFunc = []( uint32_t value ) -> uint32_t* { return new uint32_t( value ); };

    auto worker = [ num_operations, &ptrTable, &allocateFunc, &allocated_pointers, &mutex ]( int thread_id ) {
        for ( int i = 0; i < num_operations; ++i ) {
            uint32_t  key      = thread_id * num_operations + i;
            uint32_t* outValue = nullptr;

            auto status = ptrTable.GetOrAddByFunc( key, outValue, allocateFunc, key * 10 );
            EXPECT_NE( status, HashTableStatus::FAILED );
            EXPECT_NE( outValue, nullptr );
            if ( outValue != nullptr ) {
                EXPECT_EQ( *outValue, key * 10 );
                // 保存指针用于后续清理
                std::lock_guard<std::mutex> lg{ mutex };
                allocated_pointers.push_back( outValue );
            }
        }
    };

    for ( int i = 0; i < num_threads; ++i ) {
        threads.emplace_back( worker, i );
    }

    for ( auto& t : threads ) {
        t.join();
    }

    // 验证所有数据都存在
    for ( int i = 0; i < num_threads; ++i ) {
        for ( int j = 0; j < num_operations; ++j ) {
            uint32_t  key      = i * num_operations + j;
            uint32_t* outValue = nullptr;
            bool      found    = ptrTable.Get( key, outValue );
            EXPECT_TRUE( found );
            EXPECT_NE( outValue, nullptr );
            if ( outValue != nullptr ) {
                EXPECT_EQ( *outValue, key * 10 );
            }
        }
    }

    // 清理分配的内存
    for ( auto ptr : allocated_pointers ) {
        delete ptr;
    }
}

// 测试移动语义
TEST_F( HashTableTest, MoveSemantics ) {
    // 向原表插入一些数据
    for ( uint32_t i = 0; i < 10; ++i ) {
        uint32_t outValue = 0;
        table->GetOrAdd( i, outValue, i * 100 );
    }

    auto original_size = table->GetSize();

    // 移动构造
    *table = std::move( *table );

    // 验证数据被移动
    for ( uint32_t i = 0; i < 10; ++i ) {
        uint32_t outValue = 0;
        bool     found    = table->Get( i, outValue );
        EXPECT_TRUE( found );
        EXPECT_EQ( outValue, i * 100 );
    }

    EXPECT_EQ( table->GetSize(), original_size );

    // 移动构造
    TestHashTable moved_table( std::move( *table ) );

    // 验证数据被移动
    for ( uint32_t i = 0; i < 10; ++i ) {
        uint32_t outValue = 0;
        bool     found    = moved_table.Get( i, outValue );
        EXPECT_TRUE( found );
        EXPECT_EQ( outValue, i * 100 );
        bool found2 = table->Get( i, outValue );
        EXPECT_FALSE( found2 );
    }

    // 验证大小正确
    EXPECT_EQ( moved_table.GetSize(), original_size );
    EXPECT_EQ( table->GetSize(), 0 );

    // move operator=
    TestHashTable moved_table2{};
    moved_table2 = std::move( moved_table );

    // 验证数据被移动
    for ( uint32_t i = 0; i < 10; ++i ) {
        uint32_t outValue = 0;
        bool     found    = moved_table2.Get( i, outValue );
        EXPECT_TRUE( found );
        EXPECT_EQ( outValue, i * 100 );
        bool found2 = moved_table.Get( i, outValue );
        EXPECT_FALSE( found2 );
    }

    // 验证大小正确
    EXPECT_EQ( moved_table2.GetSize(), original_size );
    EXPECT_EQ( moved_table.GetSize(), 0 );

    TestHashTable swap_table{ UINT32_MAX };

    swap_table.swap( moved_table2 );

    // 验证数据被移动
    for ( uint32_t i = 0; i < 10; ++i ) {
        uint32_t outValue = 0;
        bool     found    = swap_table.Get( i, outValue );
        EXPECT_TRUE( found );
        EXPECT_EQ( outValue, i * 100 );
        bool found2 = moved_table2.Get( i, outValue );
        EXPECT_FALSE( found2 );
    }

    // 验证大小正确
    EXPECT_EQ( swap_table.GetSize(), original_size );
    EXPECT_EQ( moved_table2.GetSize(), 0 );

    TestHashTable swap_table2{ UINT32_MAX };

    using std::swap;
    swap( swap_table, swap_table2 );

    // 验证数据被移动
    for ( uint32_t i = 0; i < 10; ++i ) {
        uint32_t outValue = 0;
        bool     found    = swap_table2.Get( i, outValue );
        EXPECT_TRUE( found );
        EXPECT_EQ( outValue, i * 100 );
        bool found2 = swap_table.Get( i, outValue );
        EXPECT_FALSE( found2 );
    }

    // 验证大小正确
    EXPECT_EQ( swap_table.GetSize(), 0 );
    EXPECT_EQ( swap_table2.GetSize(), original_size );
}

// 边界值测试
TEST_F( HashTableTest, BoundaryValues ) {
    // 测试边界键值
    uint32_t min_key  = 0;
    uint32_t max_key  = UINT32_MAX - 1;  // 避免使用INVALID_KEY
    uint32_t outValue = 0;

    // 插入边界键
    auto status1 = table->GetOrAdd( min_key, outValue, 100 );
    EXPECT_EQ( status1, HashTableStatus::ADD_SUCCESS );

    auto status2 = table->GetOrAdd( max_key, outValue, 200 );
    EXPECT_EQ( status2, HashTableStatus::ADD_SUCCESS );

    // 验证边界键
    bool found1 = table->Get( min_key, outValue );
    EXPECT_TRUE( found1 );
    EXPECT_EQ( outValue, 100 );

    bool found2 = table->Get( max_key, outValue );
    EXPECT_TRUE( found2 );
    EXPECT_EQ( outValue, 200 );
}

int main( int argc, char** argv ) {
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}