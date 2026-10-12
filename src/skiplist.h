#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <new>

#include "arena.h"

namespace ledgerkv {

// A sorted skiplist with one writer and any number of lock-free readers.
//
// Concurrency: Insert() calls must be serialized externally. Readers need
// no synchronization at all. This works because nodes are never removed or
// modified after insertion (they live until the arena is destroyed), and a
// new node is fully initialized before a release-store links it in, which
// readers observe through acquire-loads. A reader therefore sees each node
// either not at all or completely.
//
// Comparator is a callable: int operator()(const Key& a, const Key& b),
// returning <0, 0, or >0.
template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  SkipList(Comparator cmp, Arena* arena)
      : compare_(cmp), arena_(arena), head_(NewNode(Key(), kMaxHeight)), max_height_(1) {}

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Requires that no equal key is already present.
  void Insert(const Key& key) {
    Node* prev[kMaxHeight];
    Node* x = FindGreaterOrEqual(key, prev);
    assert(x == nullptr || compare_(key, x->key) != 0);
    (void)x;

    int height = RandomHeight();
    if (height > GetMaxHeight()) {
      for (int i = GetMaxHeight(); i < height; i++) prev[i] = head_;
      // A reader that sees the new height before the new node is linked
      // finds null pointers at the new levels of head_, which is harmless.
      max_height_.store(height, std::memory_order_relaxed);
    }

    Node* node = NewNode(key, height);
    for (int i = 0; i < height; i++) {
      // The node is not yet reachable, so these stores need no ordering.
      node->NoBarrierSetNext(i, prev[i]->NoBarrierNext(i));
      // Publishes the node at level i.
      prev[i]->SetNext(i, node);
    }
  }

  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list) {}

    bool Valid() const { return node_ != nullptr; }
    const Key& key() const { return node_->key; }
    void Next() { node_ = node_->Next(0); }
    // Positions at the first key >= target.
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }
    void SeekToFirst() { node_ = list_->head_->Next(0); }

   private:
    const SkipList* list_;
    Node* node_ = nullptr;
  };

 private:
  static constexpr int kMaxHeight = 12;
  static constexpr uint32_t kBranching = 4;

  // Links are stored directly after the node, one per level.
  struct Node {
    explicit Node(const Key& k) : key(k) {}
    const Key key;

    std::atomic<Node*>* links() { return reinterpret_cast<std::atomic<Node*>*>(this + 1); }
    Node* Next(int n) { return links()[n].load(std::memory_order_acquire); }
    void SetNext(int n, Node* x) { links()[n].store(x, std::memory_order_release); }
    Node* NoBarrierNext(int n) { return links()[n].load(std::memory_order_relaxed); }
    void NoBarrierSetNext(int n, Node* x) { links()[n].store(x, std::memory_order_relaxed); }
  };
  static_assert(sizeof(Node) % alignof(std::atomic<Node*>) == 0);

  Node* NewNode(const Key& key, int height) {
    char* mem = arena_->Allocate(sizeof(Node) + sizeof(std::atomic<Node*>) * static_cast<size_t>(height));
    Node* node = new (mem) Node(key);
    for (int i = 0; i < height; i++) new (&node->links()[i]) std::atomic<Node*>(nullptr);
    return node;
  }

  int RandomHeight() {
    int height = 1;
    while (height < kMaxHeight && NextRandom() % kBranching == 0) height++;
    return height;
  }

  uint32_t NextRandom() {  // xorshift32; only the writer calls this.
    rnd_ ^= rnd_ << 13;
    rnd_ ^= rnd_ >> 17;
    rnd_ ^= rnd_ << 5;
    return rnd_;
  }

  int GetMaxHeight() const { return max_height_.load(std::memory_order_relaxed); }

  // Returns the first node >= key. If prev is non-null, fills prev[level]
  // with the last node < key at each level.
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const {
    Node* x = head_;
    int level = GetMaxHeight() - 1;
    while (true) {
      Node* next = x->Next(level);
      if (next != nullptr && compare_(next->key, key) < 0) {
        x = next;
      } else {
        if (prev != nullptr) prev[level] = x;
        if (level == 0) return next;
        level--;
      }
    }
  }

  const Comparator compare_;
  Arena* const arena_;
  Node* const head_;
  std::atomic<int> max_height_;
  uint32_t rnd_ = 0xDEADBEEF;
};

}  // namespace ledgerkv
