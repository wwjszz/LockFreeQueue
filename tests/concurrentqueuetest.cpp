//
// Created by admin on 2026/1/22.
//

#include "ConcurrentQueue/ConcurrentQueue.h"

#include <iostream>

int main() {
    hakle::ConcurrentQueue<int> queue;

    queue.Enqueue( 1 );
    queue.Enqueue( 2 );
    queue.Enqueue( 3 );

    int value;
    queue.TryDequeue( value );
    std::cout << value << std::endl;
    queue.TryDequeue( value );
    std::cout << value << std::endl;
    queue.TryDequeue( value );
    std::cout << value << std::endl;

    return 0;
}