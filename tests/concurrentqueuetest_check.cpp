#include "ConcurrentQueue/ConcurrentQueue.h"
#include <cstdio>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

// 工具：根据 prodThreads / itemsPerProd 计算理论总和
std::uint64_t CalcExpectedSum( std::size_t prodThreads, std::size_t itemsPerProd ) {
    std::uint64_t sum = 0;
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        for ( std::size_t i = 0; i < itemsPerProd; ++i ) {
            sum += static_cast<std::uint64_t>( p * itemsPerProd + i );
        }
    }
    return sum;
}

// 为了避免一次测试时间太长，这里规模用小一点的数
static constexpr std::size_t kProdThreadsSmall = 10;
static constexpr std::size_t kConsThreadsSmall = 20;
static constexpr std::size_t kItemsPerProducer = 100000;

// ---------------------------------------------------------------------
// 1. 多生产者 / 多消费者，普通 Enqueue / TryDequeue
// ---------------------------------------------------------------------
TEST( ConcurrentQueueCorrectness, MultiProducerMultiConsumer_NormalEnqDeq ) {
    hakle::ConcurrentQueue<int> queue;

    constexpr std::size_t prodThreads  = kProdThreadsSmall;
    constexpr std::size_t consThreads  = kConsThreadsSmall;
    constexpr std::size_t itemsPerProd = kItemsPerProducer;
    const std::size_t     totalItems   = prodThreads * itemsPerProd;
    const std::uint64_t   expectedSum  = CalcExpectedSum( prodThreads, itemsPerProd );

    std::atomic<std::size_t>   produced{ 0 };
    std::atomic<std::size_t>   consumed{ 0 };
    std::atomic<std::uint64_t> sum{ 0 };

    std::vector<std::thread> producers, consumers;

    // producers
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        producers.emplace_back( [ &, p ] {
            for ( std::size_t i = 0; i < itemsPerProd; ++i ) {
                int v = static_cast<int>( p * itemsPerProd + i );
                queue.Enqueue( v );
                produced.fetch_add( 1, std::memory_order_relaxed );
            }
        } );
    }

    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers.emplace_back( [ & ] {
            int value;
            while ( consumed.load( std::memory_order_relaxed ) < totalItems / 4 ) {
                if ( queue.TryDequeue( value ) ) {
                    sum.fetch_add( static_cast<std::uint64_t>( value ), std::memory_order_relaxed );
                    consumed.fetch_add( 1, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    for ( auto& t : producers )
        t.join();
    for ( auto& t : consumers )
        t.join();

    std::size_t queue_size = queue.Size();

    hakle::ConcurrentQueue<int> queue2 = std::move( queue );
    printf( "queue_size= %llu\n", queue_size );

    EXPECT_EQ( queue2.Size(), queue_size );
    EXPECT_EQ( queue.Size(), 0 );

    std::vector<std::thread> consumers2;
    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers2.emplace_back( [ & ] {
            int value;
            while ( consumed.load( std::memory_order_relaxed ) < totalItems / 2 ) {
                if ( queue2.TryDequeue( value ) ) {
                    sum.fetch_add( static_cast<std::uint64_t>( value ), std::memory_order_relaxed );
                    consumed.fetch_add( 1, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    for ( auto& t : consumers2 )
        t.join();

    std::size_t queue_size2 = queue2.Size();

    hakle::ConcurrentQueue<int> queue3;
    queue3                  = std::move( queue2 );
//    std::size_t queue_size3 = queue3.Size();
    printf( "queue_size2= %llu\n", queue_size2 );

    EXPECT_EQ( queue3.Size(), queue_size2 );

    std::vector<std::thread> consumers3;
    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers3.emplace_back( [ & ] {
            int value;
            while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                if ( queue3.TryDequeue( value ) ) {
                    sum.fetch_add( static_cast<std::uint64_t>( value ), std::memory_order_relaxed );
                    consumed.fetch_add( 1, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    for ( auto& t : consumers3 )
        t.join();

    EXPECT_EQ( queue3.Size(), 0 );

    EXPECT_EQ( produced.load(), totalItems );
    EXPECT_EQ( consumed.load(), totalItems );
    EXPECT_EQ( sum.load(), expectedSum );
}

// ---------------------------------------------------------------------
// 2. 多生产者 / 多消费者，Bulk Enqueue / TryDequeueBulk
// ---------------------------------------------------------------------
TEST( ConcurrentQueueCorrectness, MultiProducerMultiConsumer_BulkEnqDeq ) {
    constexpr std::size_t BULK = 128;

    hakle::ConcurrentQueue<int> queue;

    constexpr std::size_t prodThreads  = kProdThreadsSmall;
    constexpr std::size_t consThreads  = kConsThreadsSmall;
    constexpr std::size_t itemsPerProd = kItemsPerProducer;
    const std::size_t     totalItems   = prodThreads * itemsPerProd;
    const std::uint64_t   expectedSum  = CalcExpectedSum( prodThreads, itemsPerProd );

    std::atomic<std::size_t>   produced{ 0 };
    std::atomic<std::size_t>   consumed{ 0 };
    std::atomic<std::uint64_t> sum{ 0 };

    std::vector<std::thread> producers, consumers;

    // producers
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        producers.emplace_back( [ &, p ] {
            std::vector<int> buf( BULK );
            std::size_t      sent = 0;
            while ( sent < itemsPerProd ) {
                std::size_t n = std::min( BULK, itemsPerProd - sent );
                for ( std::size_t i = 0; i < n; ++i ) {
                    buf[ i ] = static_cast<int>( p * itemsPerProd + sent + i );
                }
                queue.EnqueueBulk( buf.data(), n );
                produced.fetch_add( n, std::memory_order_relaxed );
                sent += n;
            }
        } );
    }

    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers.emplace_back( [ & ] {
            std::vector<int> buf( BULK );
            while ( consumed.load( std::memory_order_relaxed ) < totalItems / 4 ) {
                std::size_t got = queue.TryDequeueBulk( buf.data(), BULK );
                if ( got > 0 ) {
                    std::uint64_t localSum = 0;
                    for ( std::size_t i = 0; i < got; ++i ) {
                        localSum += static_cast<std::uint64_t>( buf[ i ] );
                    }
                    sum.fetch_add( localSum, std::memory_order_relaxed );
                    consumed.fetch_add( got, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    for ( auto& t : producers )
        t.join();
    for ( auto& t : consumers )
        t.join();

    std::size_t queue_size = queue.Size();

    hakle::ConcurrentQueue<int> queue2 = std::move( queue );
    printf( "queue_size= %llu\n", queue_size );

    EXPECT_EQ( queue2.Size(), queue_size );
    EXPECT_EQ( queue.Size(), 0 );

    std::vector<std::thread> consumers2;
    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers2.emplace_back( [ & ] {
            std::vector<int> buf( BULK );
            while ( consumed.load( std::memory_order_relaxed ) < totalItems / 2 ) {
                std::size_t got = queue2.TryDequeueBulk( buf.data(), BULK );
                if ( got > 0 ) {
                    std::uint64_t localSum = 0;
                    for ( std::size_t i = 0; i < got; ++i ) {
                        localSum += static_cast<std::uint64_t>( buf[ i ] );
                    }
                    sum.fetch_add( localSum, std::memory_order_relaxed );
                    consumed.fetch_add( got, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    for ( auto& t : consumers2 )
        t.join();

    std::size_t queue_size2 = queue2.Size();

    hakle::ConcurrentQueue<int> queue3;
    queue3                  = std::move( queue2 );
//    std::size_t queue_size3 = queue3.Size();
    printf( "queue_size2= %llu\n", queue_size2 );

    EXPECT_EQ( queue3.Size(), queue_size2 );

    std::vector<std::thread> consumers3;
    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers3.emplace_back( [ & ] {
            std::vector<int> buf( BULK );
            while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                std::size_t got = queue3.TryDequeueBulk( buf.data(), BULK );
                if ( got > 0 ) {
                    std::uint64_t localSum = 0;
                    for ( std::size_t i = 0; i < got; ++i ) {
                        localSum += static_cast<std::uint64_t>( buf[ i ] );
                    }
                    sum.fetch_add( localSum, std::memory_order_relaxed );
                    consumed.fetch_add( got, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    for ( auto& t : consumers3 )
        t.join();

    EXPECT_EQ( queue3.Size(), 0 );

    EXPECT_EQ( produced.load(), totalItems );
    EXPECT_EQ( consumed.load(), totalItems );
    EXPECT_EQ( sum.load(), expectedSum );
}

// ---------------------------------------------------------------------
// 3. ProducerToken + 单元素 Enq / TryDequeueFromProducer
// ---------------------------------------------------------------------
TEST( ConcurrentQueueCorrectness, ProducerToken_Enq_TryDequeueFromProducer ) {
    hakle::ConcurrentQueue<int> queue;

    constexpr std::size_t prodThreads  = kProdThreadsSmall;
    constexpr std::size_t consThreads  = kConsThreadsSmall;
    constexpr std::size_t itemsPerProd = kItemsPerProducer;
    const std::size_t     totalItems   = prodThreads * itemsPerProd;
    const std::uint64_t   expectedSum  = CalcExpectedSum( prodThreads, itemsPerProd );

    std::vector<hakle::ConcurrentQueue<int>::ProducerToken> prodTokens;
    prodTokens.reserve( prodThreads );
    for ( std::size_t i = 0; i < prodThreads; ++i ) {
        prodTokens.emplace_back( queue.GetProducerToken() );
    }

    std::atomic<std::size_t>   produced{ 0 };
    std::atomic<std::size_t>   consumed{ 0 };
    std::atomic<std::size_t>   nextProducer{ 0 };
    std::atomic<std::uint64_t> sum{ 0 };

    std::vector<std::thread> producers, consumers;

    // producers
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        producers.emplace_back( [ &, p ] {
            auto& token = prodTokens[ p ];
            for ( std::size_t i = 0; i < itemsPerProd; ++i ) {
                int v = static_cast<int>( p * itemsPerProd + i );
                queue.EnqueueWithToken( token, v );
                produced.fetch_add( 1, std::memory_order_relaxed );
            }
        } );
    }

    // consumers：轮询不同 producer token
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers.emplace_back( [ & ] {
            int value;
            while ( consumed.load( std::memory_order_relaxed ) < totalItems / 4 ) {
                std::size_t idx = nextProducer.fetch_add( 1, std::memory_order_relaxed ) % prodThreads;
                if ( queue.TryDequeueFromProducer( prodTokens[ idx ], value ) ) {
                    sum.fetch_add( static_cast<std::uint64_t>( value ), std::memory_order_relaxed );
                    consumed.fetch_add( 1, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    for ( auto& t : producers )
        t.join();
    for ( auto& t : consumers )
        t.join();

    std::size_t queue_size = queue.Size();

    hakle::ConcurrentQueue<int> queue2 = std::move( queue );
    printf( "queue_size= %llu\n", queue_size );

    EXPECT_EQ( queue2.Size(), queue_size );
    EXPECT_EQ( queue.Size(), 0 );

    std::vector<std::thread> consumers2;
    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers2.emplace_back( [ & ] {
            int value;
            while ( consumed.load( std::memory_order_relaxed ) < totalItems / 2 ) {
                std::size_t idx = nextProducer.fetch_add( 1, std::memory_order_relaxed ) % prodThreads;
                if ( queue2.TryDequeueFromProducer( prodTokens[ idx ], value ) ) {
                    sum.fetch_add( static_cast<std::uint64_t>( value ), std::memory_order_relaxed );
                    consumed.fetch_add( 1, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }
    for ( auto& t : consumers2 )
        t.join();

    std::size_t queue_size2 = queue2.Size();

    hakle::ConcurrentQueue<int> queue3;
    queue3                  = std::move( queue2 );
//    std::size_t queue_size3 = queue3.Size();
    printf( "queue_size2= %llu\n", queue_size2 );

    EXPECT_EQ( queue3.Size(), queue_size2 );

    std::vector<std::thread> consumers3;
    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers3.emplace_back( [ & ] {
            int value;
            while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                std::size_t idx = nextProducer.fetch_add( 1, std::memory_order_relaxed ) % prodThreads;
                if ( queue3.TryDequeueFromProducer( prodTokens[ idx ], value ) ) {
                    sum.fetch_add( static_cast<std::uint64_t>( value ), std::memory_order_relaxed );
                    consumed.fetch_add( 1, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    for ( auto& t : consumers3 )
        t.join();

    EXPECT_EQ( queue3.Size(), 0 );

    EXPECT_EQ( produced.load(), totalItems );
    EXPECT_EQ( consumed.load(), totalItems );
    EXPECT_EQ( sum.load(), expectedSum );
}

// ---------------------------------------------------------------------
// 4. ProducerToken BulkEnq / ConsumerToken BulkDeq
// ---------------------------------------------------------------------
TEST( ConcurrentQueueCorrectness, ProducerTokenBulkEnq_ConsumerTokenBulkDeq ) {
    constexpr std::size_t BULK = 128;

    hakle::ConcurrentQueue<int> queue;

    constexpr std::size_t prodThreads  = kProdThreadsSmall;
    constexpr std::size_t consThreads  = kConsThreadsSmall;
    constexpr std::size_t itemsPerProd = kItemsPerProducer;
    const std::size_t     totalItems   = prodThreads * itemsPerProd;
    const std::uint64_t   expectedSum  = CalcExpectedSum( prodThreads, itemsPerProd );

    std::vector<hakle::ConcurrentQueue<int>::ProducerToken> prodTokens;
    prodTokens.reserve( prodThreads );
    for ( std::size_t i = 0; i < prodThreads; ++i ) {
        prodTokens.emplace_back( queue.GetProducerToken() );
    }

    std::atomic<std::size_t>   produced{ 0 };
    std::atomic<std::size_t>   consumed{ 0 };
    std::atomic<std::uint64_t> sum{ 0 };

    std::vector<std::thread> producers, consumers;

    // producers
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        producers.emplace_back( [ &, p ] {
            auto&            token = prodTokens[ p ];
            std::vector<int> buf( BULK );
            std::size_t      sent = 0;
            while ( sent < itemsPerProd ) {
                std::size_t n = std::min( BULK, itemsPerProd - sent );
                for ( std::size_t i = 0; i < n; ++i ) {
                    buf[ i ] = static_cast<int>( p * itemsPerProd + sent + i );
                }
                queue.EnqueueBulk( token, buf.data(), n );
                produced.fetch_add( n, std::memory_order_relaxed );
                sent += n;
            }
        } );
    }

    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers.emplace_back( [ & ] {
            typename hakle::ConcurrentQueue<int>::ConsumerToken token( queue );
            std::vector<int>                                    buf( BULK );
            while ( consumed.load( std::memory_order_relaxed ) < totalItems / 4 ) {
                std::size_t got = queue.TryDequeueBulk( token, buf.data(), BULK );
                if ( got > 0 ) {
                    std::uint64_t localSum = 0;
                    for ( std::size_t i = 0; i < got; ++i ) {
                        localSum += static_cast<std::uint64_t>( buf[ i ] );
                    }
                    sum.fetch_add( localSum, std::memory_order_relaxed );
                    consumed.fetch_add( got, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    for ( auto& t : producers )
        t.join();
    for ( auto& t : consumers )
        t.join();

    std::size_t queue_size = queue.Size();

    hakle::ConcurrentQueue<int> queue2 = std::move( queue );
    printf( "queue_size= %llu\n", queue_size );

    EXPECT_EQ( queue2.Size(), queue_size );
    EXPECT_EQ( queue.Size(), 0 );

    std::vector<std::thread> consumers2;
    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers2.emplace_back( [ & ] {
            typename hakle::ConcurrentQueue<int>::ConsumerToken token( queue );
            std::vector<int>                                    buf( BULK );
            while ( consumed.load( std::memory_order_relaxed ) < totalItems / 2 ) {
                std::size_t got = queue2.TryDequeueBulk( token, buf.data(), BULK );
                if ( got > 0 ) {
                    std::uint64_t localSum = 0;
                    for ( std::size_t i = 0; i < got; ++i ) {
                        localSum += static_cast<std::uint64_t>( buf[ i ] );
                    }
                    sum.fetch_add( localSum, std::memory_order_relaxed );
                    consumed.fetch_add( got, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }
    for ( auto& t : consumers2 )
        t.join();

    std::size_t queue_size2 = queue2.Size();

    hakle::ConcurrentQueue<int> queue3;
    queue3                  = std::move( queue2 );
//    std::size_t queue_size3 = queue3.Size();
    printf( "queue_size2= %llu\n", queue_size2 );

    EXPECT_EQ( queue3.Size(), queue_size2 );

    std::vector<std::thread> consumers3;
    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers3.emplace_back( [ & ] {
            typename hakle::ConcurrentQueue<int>::ConsumerToken token( queue );
            std::vector<int>                                    buf( BULK );
            while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                std::size_t got = queue3.TryDequeueBulk( token, buf.data(), BULK );
                if ( got > 0 ) {
                    std::uint64_t localSum = 0;
                    for ( std::size_t i = 0; i < got; ++i ) {
                        localSum += static_cast<std::uint64_t>( buf[ i ] );
                    }
                    sum.fetch_add( localSum, std::memory_order_relaxed );
                    consumed.fetch_add( got, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    for ( auto& t : consumers3 )
        t.join();

    EXPECT_EQ( queue3.Size(), 0 );

    EXPECT_EQ( produced.load(), totalItems );
    EXPECT_EQ( consumed.load(), totalItems );
    EXPECT_EQ( sum.load(), expectedSum );
}

// ---------------------------------------------------------------------
// 5. 普通 Enq + ConsumerToken Deq（单元素）
// ---------------------------------------------------------------------
TEST( ConcurrentQueueCorrectness, NormalEnq_ConsumerTokenDeq_SingleElement ) {
    hakle::ConcurrentQueue<int> queue;

    constexpr std::size_t prodThreads  = kProdThreadsSmall;
    constexpr std::size_t consThreads  = kConsThreadsSmall;
    constexpr std::size_t itemsPerProd = kItemsPerProducer;
    const std::size_t     totalItems   = prodThreads * itemsPerProd;
    const std::uint64_t   expectedSum  = CalcExpectedSum( prodThreads, itemsPerProd );

    std::atomic<std::size_t>   produced{ 0 };
    std::atomic<std::size_t>   consumed{ 0 };
    std::atomic<std::uint64_t> sum{ 0 };

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    // producers: 普通 Enqueue
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        producers.emplace_back( [ &, p ] {
            for ( std::size_t i = 0; i < itemsPerProd; ++i ) {
                int v = static_cast<int>( p * itemsPerProd + i );
                queue.Enqueue( v );
                produced.fetch_add( 1, std::memory_order_relaxed );
            }
        } );
    }

    // consumers: 使用 ConsumerToken + TryDequeue(token, value)
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers.emplace_back( [ & ] {
            typename hakle::ConcurrentQueue<int>::ConsumerToken token( queue );
            int                                                 value;
            while ( consumed.load( std::memory_order_relaxed ) < totalItems / 4 ) {
                if ( queue.TryDequeue( token, value ) ) {
                    sum.fetch_add( static_cast<std::uint64_t>( value ), std::memory_order_relaxed );
                    consumed.fetch_add( 1, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    for ( auto& t : producers )
        t.join();
    for ( auto& t : consumers )
        t.join();

    std::size_t queue_size = queue.Size();

    hakle::ConcurrentQueue<int> queue2 = std::move( queue );
    printf( "queue_size= %llu\n", queue_size );

    EXPECT_EQ( queue2.Size(), queue_size );
    EXPECT_EQ( queue.Size(), 0 );

    std::vector<std::thread> consumers2;
    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers2.emplace_back( [ & ] {
            typename hakle::ConcurrentQueue<int>::ConsumerToken token( queue );
            int                                                 value;
            while ( consumed.load( std::memory_order_relaxed ) < totalItems / 2 ) {
                if ( queue2.TryDequeue( token, value ) ) {
                    sum.fetch_add( static_cast<std::uint64_t>( value ), std::memory_order_relaxed );
                    consumed.fetch_add( 1, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }
    for ( auto& t : consumers2 )
        t.join();

    std::size_t queue_size2 = queue2.Size();

    hakle::ConcurrentQueue<int> queue3;
    queue3                  = std::move( queue2 );
//    std::size_t queue_size3 = queue3.Size();
    printf( "queue_size2= %llu\n", queue_size2 );

    EXPECT_EQ( queue3.Size(), queue_size2 );

    std::vector<std::thread> consumers3;
    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers3.emplace_back( [ & ] {
            typename hakle::ConcurrentQueue<int>::ConsumerToken token( queue );
            int                                                 value;
            while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                if ( queue3.TryDequeue( token, value ) ) {
                    sum.fetch_add( static_cast<std::uint64_t>( value ), std::memory_order_relaxed );
                    consumed.fetch_add( 1, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    for ( auto& t : consumers3 )
        t.join();

    EXPECT_EQ( queue3.Size(), 0 );

    EXPECT_EQ( produced.load(), totalItems );
    EXPECT_EQ( consumed.load(), totalItems );
    EXPECT_EQ( sum.load(), expectedSum );
}

// ---------------------------------------------------------------------
// 6. 普通 BulkEnq + ConsumerToken BulkDeq
// ---------------------------------------------------------------------
TEST( ConcurrentQueueCorrectness, NormalBulkEnq_ConsumerTokenBulkDeq ) {
    constexpr std::size_t BULK = 128;

    hakle::ConcurrentQueue<int> queue;

    constexpr std::size_t prodThreads  = kProdThreadsSmall;
    constexpr std::size_t consThreads  = kConsThreadsSmall;
    constexpr std::size_t itemsPerProd = kItemsPerProducer;
    const std::size_t     totalItems   = prodThreads * itemsPerProd;
    const std::uint64_t   expectedSum  = CalcExpectedSum( prodThreads, itemsPerProd );

    std::atomic<std::size_t>   produced{ 0 };
    std::atomic<std::size_t>   consumed{ 0 };
    std::atomic<std::uint64_t> sum{ 0 };

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    // producers: 普通 EnqueueBulk
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        producers.emplace_back( [ &, p ] {
            std::vector<int> buf( BULK );
            std::size_t      sent = 0;
            while ( sent < itemsPerProd ) {
                std::size_t n = std::min( BULK, itemsPerProd - sent );
                for ( std::size_t i = 0; i < n; ++i ) {
                    buf[ i ] = static_cast<int>( p * itemsPerProd + sent + i );
                }
                queue.EnqueueBulk( buf.data(), n );
                produced.fetch_add( n, std::memory_order_relaxed );
                sent += n;
            }
        } );
    }

    // consumers: ConsumerToken + TryDequeueBulk(token, buf, BULK)
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers.emplace_back( [ & ] {
            typename hakle::ConcurrentQueue<int>::ConsumerToken token( queue );
            std::vector<int>                                    buf( BULK );
            while ( consumed.load( std::memory_order_relaxed ) < totalItems / 4 ) {
                std::size_t got = queue.TryDequeueBulk( token, buf.data(), BULK );
                if ( got > 0 ) {
                    std::uint64_t localSum = 0;
                    for ( std::size_t i = 0; i < got; ++i ) {
                        localSum += static_cast<std::uint64_t>( buf[ i ] );
                    }
                    sum.fetch_add( localSum, std::memory_order_relaxed );
                    consumed.fetch_add( got, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    for ( auto& t : producers )
        t.join();
    for ( auto& t : consumers )
        t.join();

    std::size_t queue_size = queue.Size();

    hakle::ConcurrentQueue<int> queue2 = std::move( queue );
    printf( "queue_size= %llu\n", queue_size );

    EXPECT_EQ( queue2.Size(), queue_size );
    EXPECT_EQ( queue.Size(), 0 );

    std::vector<std::thread> consumers2;
    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers2.emplace_back( [ & ] {
            typename hakle::ConcurrentQueue<int>::ConsumerToken token( queue );
            std::vector<int>                                    buf( BULK );
            while ( consumed.load( std::memory_order_relaxed ) < totalItems / 2 ) {
                std::size_t got = queue2.TryDequeueBulk( token, buf.data(), BULK );
                if ( got > 0 ) {
                    std::uint64_t localSum = 0;
                    for ( std::size_t i = 0; i < got; ++i ) {
                        localSum += static_cast<std::uint64_t>( buf[ i ] );
                    }
                    sum.fetch_add( localSum, std::memory_order_relaxed );
                    consumed.fetch_add( got, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }
    for ( auto& t : consumers2 )
        t.join();

    std::size_t queue_size2 = queue2.Size();

    hakle::ConcurrentQueue<int> queue3;
    queue3                  = std::move( queue2 );
//    std::size_t queue_size3 = queue3.Size();
    printf( "queue_size2= %llu\n", queue_size2 );

    EXPECT_EQ( queue3.Size(), queue_size2 );

    std::vector<std::thread> consumers3;
    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers3.emplace_back( [ & ] {
            typename hakle::ConcurrentQueue<int>::ConsumerToken token( queue );
            std::vector<int>                                    buf( BULK );
            while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                std::size_t got = queue3.TryDequeueBulk( token, buf.data(), BULK );
                if ( got > 0 ) {
                    std::uint64_t localSum = 0;
                    for ( std::size_t i = 0; i < got; ++i ) {
                        localSum += static_cast<std::uint64_t>( buf[ i ] );
                    }
                    sum.fetch_add( localSum, std::memory_order_relaxed );
                    consumed.fetch_add( got, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    for ( auto& t : consumers3 )
        t.join();

    EXPECT_EQ( queue3.Size(), 0 );

    EXPECT_EQ( produced.load(), totalItems );
    EXPECT_EQ( consumed.load(), totalItems );
    EXPECT_EQ( sum.load(), expectedSum );
}

// ---------------------------------------------------------------------
// 7. ProducerToken Enq / ConsumerToken Deq（单元素）
// ---------------------------------------------------------------------
TEST( ConcurrentQueueCorrectness, ProducerTokenEnq_ConsumerTokenDeq_SingleElement ) {
    hakle::ConcurrentQueue<int> queue;

    constexpr std::size_t prodThreads  = kProdThreadsSmall;
    constexpr std::size_t consThreads  = kConsThreadsSmall;
    constexpr std::size_t itemsPerProd = kItemsPerProducer;
    const std::size_t     totalItems   = prodThreads * itemsPerProd;
    const std::uint64_t   expectedSum  = CalcExpectedSum( prodThreads, itemsPerProd );

    // 为每个生产线程创建一个 ProducerToken
    std::vector<hakle::ConcurrentQueue<int>::ProducerToken> prodTokens;
    prodTokens.reserve( prodThreads );
    for ( std::size_t i = 0; i < prodThreads; ++i ) {
        prodTokens.emplace_back( queue.GetProducerToken() );
    }

    std::atomic<std::size_t>   produced{ 0 };
    std::atomic<std::size_t>   consumed{ 0 };
    std::atomic<std::uint64_t> sum{ 0 };

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    // producers：使用 ProducerToken + EnqueueWithToken（单元素）
    for ( std::size_t p = 0; p < prodThreads; ++p ) {
        producers.emplace_back( [ &, p ] {
            auto& token = prodTokens[ p ];
            for ( std::size_t i = 0; i < itemsPerProd; ++i ) {
                int v = static_cast<int>( p * itemsPerProd + i );
                queue.EnqueueWithToken( token, v );
                produced.fetch_add( 1, std::memory_order_relaxed );
            }
        } );
    }

    // consumers：为每个线程创建一个 ConsumerToken + TryDequeue(token, value)
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers.emplace_back( [ & ] {
            typename hakle::ConcurrentQueue<int>::ConsumerToken token( queue );
            int                                                 value;
            while ( consumed.load( std::memory_order_relaxed ) < totalItems / 4 ) {
                if ( queue.TryDequeue( token, value ) ) {
                    sum.fetch_add( static_cast<std::uint64_t>( value ), std::memory_order_relaxed );
                    consumed.fetch_add( 1, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    for ( auto& t : producers )
        t.join();
    for ( auto& t : consumers )
        t.join();

    std::size_t queue_size = queue.Size();

    hakle::ConcurrentQueue<int> queue2 = std::move( queue );
    printf( "queue_size= %llu\n", queue_size );

    EXPECT_EQ( queue2.Size(), queue_size );
    EXPECT_EQ( queue.Size(), 0 );

    std::vector<std::thread> consumers2;
    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers2.emplace_back( [ & ] {
            typename hakle::ConcurrentQueue<int>::ConsumerToken token( queue );
            int                                                 value;
            while ( consumed.load( std::memory_order_relaxed ) < totalItems / 2 ) {
                if ( queue2.TryDequeue( token, value ) ) {
                    sum.fetch_add( static_cast<std::uint64_t>( value ), std::memory_order_relaxed );
                    consumed.fetch_add( 1, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }
    for ( auto& t : consumers2 )
        t.join();

    std::size_t queue_size2 = queue2.Size();

    hakle::ConcurrentQueue<int> queue3;
    queue3                  = std::move( queue2 );
//    std::size_t queue_size3 = queue3.Size();
    printf( "queue_size2= %llu\n", queue_size2 );

    EXPECT_EQ( queue3.Size(), queue_size2 );

    std::vector<std::thread> consumers3;
    // consumers
    for ( std::size_t c = 0; c < consThreads; ++c ) {
        consumers3.emplace_back( [ & ] {
            typename hakle::ConcurrentQueue<int>::ConsumerToken token( queue );
            int                                                 value;
            while ( consumed.load( std::memory_order_relaxed ) < totalItems ) {
                if ( queue3.TryDequeue( token, value ) ) {
                    sum.fetch_add( static_cast<std::uint64_t>( value ), std::memory_order_relaxed );
                    consumed.fetch_add( 1, std::memory_order_relaxed );
                }
                else {
                    std::this_thread::yield();
                }
            }
        } );
    }

    for ( auto& t : consumers3 )
        t.join();

    EXPECT_EQ( queue3.Size(), 0 );

    EXPECT_EQ( produced.load(), totalItems );
    EXPECT_EQ( consumed.load(), totalItems );
    EXPECT_EQ( sum.load(), expectedSum );
}

// 还可以继续加：
// - 普通 Enq + ConsumerToken Deq（单元素）
// - 普通 BulkEnq + ConsumerToken BulkDeq
// 等，方式和上面类似。

int main( int argc, char** argv ) {
    ::testing::InitGoogleTest( &argc, argv );

    // 打印所有测试开始和结束（可选）
    ::testing::UnitTest::GetInstance()->listeners();
    // 注意：不要删除默认的 listener，否则看不到输出

    return RUN_ALL_TESTS();
}