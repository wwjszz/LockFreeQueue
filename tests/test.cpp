#include "ConcurrentQueue/ConcurrentQueue.h"


using namespace hakle;


int main(  ) {
    hakle::ConcurrentQueue<int> q;
    q.Enqueue( 1 );

    return 0;
}