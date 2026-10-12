#include "version.h"

#include <algorithm>

namespace ledgerkv {

bool Overlaps(const TableMeta& m, std::string_view lo, std::string_view hi) {
  return !(std::string_view(m.largest) < lo || std::string_view(m.smallest) > hi);
}

std::shared_ptr<Version> ApplyEdit(const Version& base, const VersionEdit& edit,
                                   const std::map<uint64_t, std::shared_ptr<Table>>& tables) {
  auto v = std::make_shared<Version>(base);
  std::set<uint64_t> deleted(edit.deleted_tables.begin(), edit.deleted_tables.end());
  for (auto& level : v->levels) {
    std::erase_if(level, [&](const FileEntry& f) { return deleted.count(f.meta.number) > 0; });
  }
  for (const TableMeta& m : edit.added_tables) v->levels[m.level].push_back({m, tables.at(m.number)});
  std::sort(v->levels[0].begin(), v->levels[0].end(),
            [](const FileEntry& a, const FileEntry& b) { return a.meta.number > b.meta.number; });
  for (int level = 1; level < kNumLevels; level++) {
    std::sort(v->levels[level].begin(), v->levels[level].end(),
              [](const FileEntry& a, const FileEntry& b) { return a.meta.smallest < b.meta.smallest; });
  }
  return v;
}

std::set<uint64_t> LiveTables(const Version& v) {
  std::set<uint64_t> live;
  for (const auto& level : v.levels) {
    for (const FileEntry& f : level) live.insert(f.meta.number);
  }
  return live;
}

namespace {

class LevelIterator final : public InternalIterator {
 public:
  explicit LevelIterator(std::vector<FileEntry> files) : files_(std::move(files)) {}

  void SeekToFirst() override {
    status_ = Status::OK();
    Open(0);
    if (inner_) inner_->SeekToFirst();
    SkipExhausted();
  }

  void Seek(std::string_view target) override {
    status_ = Status::OK();
    auto it = std::lower_bound(files_.begin(), files_.end(), target, [](const FileEntry& f, std::string_view k) {
      return std::string_view(f.meta.largest) < k;
    });
    Open(static_cast<size_t>(it - files_.begin()));
    if (inner_) inner_->Seek(target);
    SkipExhausted();
  }

  void Next() override {
    inner_->Next();
    SkipExhausted();
  }

  bool Valid() const override { return inner_ && inner_->Valid(); }
  std::string_view key() const override { return inner_->key(); }
  uint64_t seq() const override { return inner_->seq(); }
  RecordType type() const override { return inner_->type(); }
  std::string_view value() const override { return inner_->value(); }
  Status status() const override { return status_; }

 private:
  void Open(size_t index) {
    index_ = index;
    inner_.reset();
    if (index < files_.size()) inner_ = std::make_unique<Table::Iterator>(files_[index].table.get());
  }

  // Moves on to the next table while the current one is used up.
  void SkipExhausted() {
    while (inner_ && !inner_->Valid()) {
      if (!inner_->status().ok()) {
        status_ = inner_->status();
        inner_.reset();
        return;
      }
      Open(index_ + 1);
      if (inner_) inner_->SeekToFirst();
    }
  }

  std::vector<FileEntry> files_;
  size_t index_ = 0;
  std::unique_ptr<Table::Iterator> inner_;
  Status status_;
};

}  // namespace

std::unique_ptr<InternalIterator> NewLevelIterator(std::vector<FileEntry> files) {
  return std::make_unique<LevelIterator>(std::move(files));
}

}  // namespace ledgerkv
