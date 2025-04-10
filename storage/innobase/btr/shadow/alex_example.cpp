#include <cstdint>
#include "alexol/alex.h"

using KeyT = uint64_t;
constexpr KeyT kMaxKeyCnt = 180;
std::pair<KeyT, KeyT> keys[kMaxKeyCnt];

int main() {
  for (KeyT j = 0; j < kMaxKeyCnt; ++j) {
    keys[j] = {j, j};
  }
  for (int i = 0; i < 1; ++i) {
    alexol::Alex<KeyT, KeyT> tree;
    std::cout << "Iter " << i << std::endl;
    tree.bulk_load(keys, kMaxKeyCnt);

    for (KeyT j = kMaxKeyCnt; j < 2 * kMaxKeyCnt; ++j) {
      tree.insert(j, j);
      
      if (j == 304 || j == 305) {
        auto iter = tree.find(kMaxKeyCnt);
        if (iter.is_end() || iter.payload() != kMaxKeyCnt) {
          std::cout << "Error, after insert " << j << std::endl;
          return 0;
        }
      }
    }
    std::cout << tree.size() << std::endl;

    for (KeyT j = kMaxKeyCnt; j < 2 * kMaxKeyCnt; ++j) {
      auto iter = tree.find(j);
      if (iter.is_end() || iter.payload() != j) {
        std::cout << "Error, key " << j << " not found" << std::endl;
      }
    }
  }
  return 0;
}