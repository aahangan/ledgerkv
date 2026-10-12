#include "db_iter.h"

#include <cassert>
#include <string>

namespace ledgerkv {
namespace {

class DBIter final : public Iterator {
 public:
  DBIter(std::unique_ptr<InternalIterator> iter, uint64_t seq, std::vector<std::shared_ptr<const void>> pins)
      : pins_(std::move(pins)), iter_(std::move(iter)), seq_(seq) {}

  bool Valid() const override { return valid_; }

  void SeekToFirst() override {
    iter_->SeekToFirst();
    FindNextVisible(nullptr);
  }

  void Seek(std::string_view target) override {
    iter_->Seek(target);
    FindNextVisible(nullptr);
  }

  void Next() override {
    assert(valid_);
    saved_key_.assign(iter_->key());
    iter_->Next();
    FindNextVisible(&saved_key_);  // Skip older versions of the key just returned.
  }

  std::string_view key() const override { return iter_->key(); }
  std::string_view value() const override { return iter_->value(); }
  Status status() const override { return iter_->status(); }

 private:
  // Advances to the next entry a reader at seq_ should see. Entries arrive
  // newest version first per key, so the first visible version of a key is
  // the one to return, unless it is a tombstone, in which case the whole key
  // is skipped.
  void FindNextVisible(const std::string* skip) {
    bool skipping = skip != nullptr;
    if (skipping) skip_key_ = *skip;
    while (iter_->Valid()) {
      if (iter_->seq() > seq_) {
        iter_->Next();  // Written after our snapshot.
      } else if (skipping && iter_->key() == skip_key_) {
        iter_->Next();  // Older version of a key already decided.
      } else if (iter_->type() == RecordType::kDelete) {
        skip_key_.assign(iter_->key());
        skipping = true;
        iter_->Next();
      } else {
        valid_ = true;
        return;
      }
    }
    valid_ = false;
  }

  std::vector<std::shared_ptr<const void>> pins_;  // Declared first: destroyed last.
  std::unique_ptr<InternalIterator> iter_;
  const uint64_t seq_;
  bool valid_ = false;
  std::string saved_key_;
  std::string skip_key_;
};

}  // namespace

std::unique_ptr<Iterator> NewDBIterator(std::unique_ptr<InternalIterator> internal, uint64_t seq,
                                        std::vector<std::shared_ptr<const void>> pins) {
  return std::make_unique<DBIter>(std::move(internal), seq, std::move(pins));
}

}  // namespace ledgerkv
