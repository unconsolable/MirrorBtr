#pragma once
#include <mutex>
#include <sstream>
#include "btr/shadow/alexol/alex.h"

namespace shadow {

template <typename Key, typename NodeHandle>
class Shadow {
 public:
  Shadow() = default;

  bool IsShadowBuild() const { return shadow_build_; }

  void BuildShadow(std::pair<Key, NodeHandle> *pairs, int num_keys) {
    shadow_.bulk_load(pairs, num_keys);
    leftmost_key_ = pairs[0].first;
    leftmost_leaf_ = pairs[0].second;

    shadow_build_ = true;
  }

  NodeHandle TraverseToLeafLE(Key key) {
    assert(shadow_build_);
    NodeHandle ret = shadow_.get_payload_last_no_greater_than(key);
    if (!ret) [[unlikely]] {
      ret = leftmost_leaf_;
    }
    return ret;
  }
  
  NodeHandle TraverseToLeafL(Key key) {
    assert(shadow_build_);
    NodeHandle ret = shadow_.get_payload_last_less_than(key);
    if (!ret) [[unlikely]] {
      ret = leftmost_leaf_;
    }
    return ret;
  }

  void UpdateLeftmostKeyLeaf(Key key, NodeHandle leaf) {
    shadow_.erase(leftmost_key_);
    shadow_.insert(leftmost_key_, leaf);

    leftmost_key_ = key;
    leftmost_leaf_ = leaf;
  }

  void InsertKeyLeaf(Key key, NodeHandle leaf) {
    shadow_.insert(key, leaf);
    if (key < leftmost_key_) {
      leftmost_key_ = key;
      leftmost_leaf_ = leaf;
    }
  }

  void UpdateKeyLeaf(Key key, NodeHandle leaf) {
    shadow_.update(key, leaf);
  }

  void DeleteKey(Key key) {
    shadow_.erase(key);
  }

  void PrintStat() {
    std::stringstream ss;
    ss << "num nodes: " << shadow_.num_nodes() << '\n';
    ss << "num leaves: " << shadow_.num_leaves() << '\n';
    ss << "footprint: " << Footprint() << '\n';
    std::cerr << ss.str();
  }

  uint64_t Footprint() {
    // 64 is slot model size
    return shadow_.model_size() + shadow_.data_size() + shadow_.size() * 64;
  }

  bool IsApplicable() const { return is_applicable_; }
  void SetApplicable(bool applicable) { is_applicable_ = applicable; }

  std::mutex& BuildMutex() { return build_mu_; }

  /* [main-im] Reused as "applicability checked" flag after tree-level shadow build is disabled.
     After applicability check completes, set to true via MarkShadowBuild(). */
  void MarkShadowBuild() { shadow_build_ = true; }

 private:
  bool shadow_build_{false};
  bool is_applicable_{true};
  alexol::Alex<Key, NodeHandle> shadow_;

  Key leftmost_key_;
  NodeHandle leftmost_leaf_{0};

  std::mutex build_mu_;
};

}  // namespace shadow