//
// Created by wwjszz on 25-11-19.
//

#ifndef LINEARSEARCHMAP_H
#define LINEARSEARCHMAP_H

#include <array>

#include "../ReaderWriterQueue/readerwriterqueue.h"

namespace hakle {

template <std::size_t N>
class LinearSearchMap {
public:
    void SetItem( int InKey, int InValue ) {
        for (LinearSearchMapEntry& Entry : Data) {
            if (int CurrentKey = Entry.Key.Load(); CurrentKey != InKey) {
                if (CurrentKey != 0)
                    continue;

                if (!Entry.Key.CompareExchangeStrong( CurrentKey, InKey ) && CurrentKey != 0 && CurrentKey != InKey)
                    continue;
            }
            Entry.Value.Store( InValue );
            return;
        }
    }

    [[nodiscard]] int GetItem( int InKey ) const {
        for (const LinearSearchMapEntry& Entry : Data) {
            int CurrentKey = Entry.Key.Load();
            if (CurrentKey == InKey)
                return Entry.Value.Load();
            if (CurrentKey == 0)
                break;
        }
        return 0;
    }

private:
    struct LinearSearchMapEntry {
        WeakAtomic<int> Key;
        WeakAtomic<int> Value;
    };

    std::array<LinearSearchMapEntry, N> Data;
};

}


#endif //LINEARSEARCHMAP_H