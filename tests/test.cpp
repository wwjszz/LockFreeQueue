#include "ConcurrentQueue/ConcurrentQueue.h"
#include "common/CompressPair.h"
#include <type_traits>
#include <vector>

class A {
public:
    A() {}
    A( A&& ) = delete;
};

// using namespace hakle;

int main() {
    hakle::ConcurrentQueue<int> q;
    q.Enqueue( 1 );

    hakle::CompressPair<A, int> a{};

    std::vector<int> t;
    t.swap( t );

    static_assert( !std::is_swappable_v<A> );
    static_assert( std::is_default_constructible_v<A> );

    return 0;
}