#include "merger.h"

#include <algorithm>

namespace ledgerkv {
namespace {

class MergingIterator final : public InternalIterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<InternalIterator>> children)
      : children_(std::move(children)) {}

  void SeekToFirst() override {
    for (auto& c : children_) c->SeekToFirst();
    RebuildHeap();
  }

  void Seek(std::string_view target) override {
    for (auto& c : children_) c->Seek(target);
    RebuildHeap();
  }

  void Next() override {
    std::pop_heap(heap_.begin(), heap_.end(), Later);
    InternalIterator* top = heap_.back();
    top->Next();
    if (top->Valid()) {
      std::push_heap(heap_.begin(), heap_.end(), Later);
    } else {
      heap_.pop_back();
    }
  }

  bool Valid() const override { return !heap_.empty(); }
  std::string_view key() const override { return heap_.front()->key(); }
  uint64_t seq() const override { return heap_.front()->seq(); }
  RecordType type() const override { return heap_.front()->type(); }
  std::string_view value() const override { return heap_.front()->value(); }

  Status status() const override {
    for (const auto& c : children_) {
      Status s = c->status();
      if (!s.ok()) return s;
    }
    return Status::OK();
  }

 private:
  // Heap order: std:: heaps keep the "largest" element on top, so a child
  // that sorts later in internal order counts as smaller.
  static bool Later(const InternalIterator* a, const InternalIterator* b) {
    return CompareInternal(a->key(), a->seq(), b->key(), b->seq()) > 0;
  }

  void RebuildHeap() {
    heap_.clear();
    for (auto& c : children_) {
      if (c->Valid()) heap_.push_back(c.get());
    }
    std::make_heap(heap_.begin(), heap_.end(), Later);
  }

  std::vector<std::unique_ptr<InternalIterator>> children_;
  std::vector<InternalIterator*> heap_;
};

}  // namespace

std::unique_ptr<InternalIterator> NewMergingIterator(std::vector<std::unique_ptr<InternalIterator>> children) {
  return std::make_unique<MergingIterator>(std::move(children));
}

}  // namespace ledgerkv
