#include "ledgerkv/status.h"

namespace ledgerkv {

std::string Status::ToString() const {
  switch (code_) {
    case Code::kOk:
      return "OK";
    case Code::kNotFound:
      return "NotFound";
    case Code::kIOError:
      return "IOError: " + msg_;
    case Code::kCorruption:
      return "Corruption: " + msg_;
    case Code::kInvalidArgument:
      return "InvalidArgument: " + msg_;
  }
  return "Unknown";
}

}  // namespace ledgerkv
