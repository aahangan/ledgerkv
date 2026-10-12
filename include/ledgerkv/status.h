#pragma once

#include <string>

namespace ledgerkv {

class Status {
 public:
  Status() = default;

  static Status OK() { return Status(); }
  static Status NotFound() { return Status(Code::kNotFound, ""); }
  static Status IOError(std::string msg) { return Status(Code::kIOError, std::move(msg)); }
  static Status Corruption(std::string msg) { return Status(Code::kCorruption, std::move(msg)); }
  static Status InvalidArgument(std::string msg) {
    return Status(Code::kInvalidArgument, std::move(msg));
  }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsIOError() const { return code_ == Code::kIOError; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }

  std::string ToString() const;

 private:
  enum class Code { kOk, kNotFound, kIOError, kCorruption, kInvalidArgument };

  Status(Code code, std::string msg) : code_(code), msg_(std::move(msg)) {}

  Code code_ = Code::kOk;
  std::string msg_;
};

}  // namespace ledgerkv
