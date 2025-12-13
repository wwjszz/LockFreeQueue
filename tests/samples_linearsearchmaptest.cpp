//
// Created by wwjszz on 25-11-19.
//

#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

#include "../Hash/LinearSearchMap.h"
#include "gtest/gtest.h"

namespace hakle {

class LinearSearchMapTest : public ::testing::Test {
protected:
    static constexpr std::size_t       MAP_SIZE = 10000;
    samples::LinearSearchMap<MAP_SIZE> map;
};

// Basic functionality tests
TEST_F( LinearSearchMapTest, BasicSetAndGet ) {
    map.SetItem( 1, 100 );
    EXPECT_EQ( map.GetItem( 1 ), 100 );

    map.SetItem( 2, 200 );
    EXPECT_EQ( map.GetItem( 2 ), 200 );

    // First key-value pair should still exist
    EXPECT_EQ( map.GetItem( 1 ), 100 );
}

TEST_F( LinearSearchMapTest, GetNonExistentKey ) { EXPECT_EQ( map.GetItem( 999 ), 0 ); }

TEST_F( LinearSearchMapTest, UpdateExistingKey ) {
    map.SetItem( 1, 100 );
    EXPECT_EQ( map.GetItem( 1 ), 100 );

    map.SetItem( 1, 200 );
    EXPECT_EQ( map.GetItem( 1 ), 200 );
}

TEST_F( LinearSearchMapTest, MultipleItems ) {
    for ( int i = 1; i <= 10; ++i ) {
        map.SetItem( i, i * 100 );
    }

    for ( int i = 1; i <= 10; ++i ) {
        EXPECT_EQ( map.GetItem( i ), i * 100 );
    }
}

// Concurrent tests
TEST_F( LinearSearchMapTest, ConcurrentSetDifferentKeys ) {
    constexpr int NUM_THREADS      = 4;
    constexpr int ITEMS_PER_THREAD = 20;

    std::vector<std::thread> threads;

    for ( int t = 0; t < NUM_THREADS; ++t ) {
        threads.emplace_back( [ ITEMS_PER_THREAD, this, t ]() {
            int base = t * ITEMS_PER_THREAD + 1;
            for ( int i = 0; i < ITEMS_PER_THREAD; ++i ) {
                int key   = base + i;
                int value = key * 100;
                map.SetItem( key, value );
            }
        } );
    }

    for ( auto& thread : threads ) {
        thread.join();
    }

    // Verify all items
    for ( int t = 0; t < NUM_THREADS; ++t ) {
        int base = t * ITEMS_PER_THREAD + 1;
        for ( int i = 0; i < ITEMS_PER_THREAD; ++i ) {
            int key   = base + i;
            int value = key * 100;
            EXPECT_EQ( map.GetItem( key ), value );
        }
    }
}

TEST_F( LinearSearchMapTest, ConcurrentSetSameKey ) {
    constexpr int    NUM_THREADS = 8;
    constexpr int    KEY         = 42;
    std::atomic<int> successCount{ 0 };

    std::vector<std::thread> threads;

    for ( int t = 0; t < NUM_THREADS; ++t ) {
        threads.emplace_back( [ KEY, this, t, &successCount ]() {
            int value = ( t + 1 ) * 100;
            map.SetItem( KEY, value );
            successCount++;
        } );
    }

    for ( auto& thread : threads ) {
        thread.join();
    }

    EXPECT_EQ( successCount, NUM_THREADS );

    // Key should exist, value should be set by one of the threads
    int result = map.GetItem( KEY );
    EXPECT_NE( result, 0 );

    // Verify the value is valid (one of the values set by threads)
    bool validValue = false;
    for ( int t = 0; t < NUM_THREADS; ++t ) {
        if ( result == ( t + 1 ) * 100 ) {
            validValue = true;
            break;
        }
    }
    EXPECT_TRUE( validValue );
}

TEST_F( LinearSearchMapTest, ConcurrentSetAndGet ) {
    constexpr int NUM_WRITER_THREADS = 4;
    constexpr int NUM_READER_THREADS = 4;
    constexpr int ITEMS_PER_WRITER   = 10;

    std::atomic<bool>        startFlag{ false };
    std::vector<std::thread> threads;

    // Writer threads
    for ( int t = 0; t < NUM_WRITER_THREADS; ++t ) {
        threads.emplace_back( [ ITEMS_PER_WRITER, this, t, &startFlag ]() {
            while ( !startFlag.load() ) {
                std::this_thread::yield();
            }

            int base = t * ITEMS_PER_WRITER + 1;
            for ( int i = 0; i < ITEMS_PER_WRITER; ++i ) {
                int key   = base + i;
                int value = key * 100;
                map.SetItem( key, value );
            }
        } );
    }

    // Reader threads
    for ( int t = 0; t < NUM_READER_THREADS; ++t ) {
        threads.emplace_back( [ ITEMS_PER_WRITER, NUM_WRITER_THREADS, this, &startFlag ]() {
            while ( !startFlag.load() ) {
                std::this_thread::yield();
            }

            for ( int i = 0; i < 1000; ++i ) {
                int key   = ( i % ( NUM_WRITER_THREADS * ITEMS_PER_WRITER ) ) + 1;
                int value = map.GetItem( key );

                // If value is found, verify it's correct
                if ( value != 0 ) {
                    EXPECT_EQ( value, key * 100 );
                }
            }
        } );
    }

    startFlag.store( true );

    for ( auto& thread : threads ) {
        thread.join();
    }

    // Final verification of all written data
    for ( int t = 0; t < NUM_WRITER_THREADS; ++t ) {
        int base = t * ITEMS_PER_WRITER + 1;
        for ( int i = 0; i < ITEMS_PER_WRITER; ++i ) {
            int key = base + i;
            EXPECT_EQ( map.GetItem( key ), key * 100 );
        }
    }
}

TEST_F( LinearSearchMapTest, StressTest_ThreadSafe ) {
    constexpr int NUM_THREADS           = 8;
    constexpr int OPERATIONS_PER_THREAD = 100;
    constexpr int KEY_RANGE             = 50;

    // 记录每个 key 所有可能被写入的合法值（线程安全）
    std::vector<std::set<int>> legalValues( KEY_RANGE + 1 );  // index 1~50
    std::mutex                 legalMutex;

    std::vector<std::thread> threads;
    std::atomic<int>         errorCount{ 0 };

    for ( int t = 0; t < NUM_THREADS; ++t ) {
        threads.emplace_back( [ OPERATIONS_PER_THREAD, KEY_RANGE, this, t, &errorCount, &legalValues, &legalMutex ]() {
            std::mt19937                       rng( t );
            std::uniform_int_distribution<int> keyDist( 1, KEY_RANGE );

            for ( int i = 0; i < OPERATIONS_PER_THREAD; ++i ) {
                int key   = keyDist( rng );
                int value = key * 1000 + t;

                // 先记录这是一个合法值
                {
                    std::lock_guard<std::mutex> lock( legalMutex );
                    legalValues[ key ].insert( value );
                }

                // 写入 map（你的 map 是线程安全的）
                map.SetItem( key, value );

                // 读回来
                int retrieved = map.GetItem( key );

                // 验证：retrieved 必须是 0（未初始化？）或某个合法值
                if ( retrieved != 0 ) {
                    std::lock_guard<std::mutex> lock( legalMutex );
                    if ( legalValues[ key ].find( retrieved ) == legalValues[ key ].end() ) {
                        errorCount++;
                    }
                }
            }
        } );
    }

    for ( auto& th : threads )
        th.join();

    EXPECT_EQ( errorCount.load(), 0 );
}

// Boundary tests
TEST_F( LinearSearchMapTest, FillToCapacity ) {
    for ( std::size_t i = 1; i <= MAP_SIZE; ++i ) {
        map.SetItem( static_cast<int>( i ), static_cast<int>( i * 10 ) );
    }

    for ( std::size_t i = 1; i <= MAP_SIZE; ++i ) {
        EXPECT_EQ( map.GetItem( static_cast<int>( i ) ), static_cast<int>( i * 10 ) );
    }
}

// Performance benchmark test (optional)
TEST_F( LinearSearchMapTest, PerformanceBenchmark ) {
    constexpr int NUM_THREADS      = 4;
    constexpr int ITEMS_PER_THREAD = 2000;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for ( int t = 0; t < NUM_THREADS; ++t ) {
        threads.emplace_back( [ ITEMS_PER_THREAD, this, t ]() {
            int base = t * ITEMS_PER_THREAD + 1;

            // Write
            for ( int i = 0; i < ITEMS_PER_THREAD; ++i ) {
                int key = base + i;
                map.SetItem( key, key * 100 );
            }

            // Read and verify
            for ( int i = 0; i < ITEMS_PER_THREAD; ++i ) {
                int key   = base + i;
                int value = map.GetItem( key );
                if ( value != key * 100 ) {
                    std::cerr << value << " " << key * 100 << " " << "Verification failed!" << std::endl;
                }
            }
        } );
    }

    for ( auto& thread : threads ) {
        thread.join();
    }

    auto end      = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>( end - start );

    std::cout << "Performance test completed in " << duration.count() << "ms" << std::endl;
}

}  // namespace hakle