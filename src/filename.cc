#include "filename.h"

#include <cstdio>
#include <cstdlib>

namespace ledgerkv {
namespace {

std::string NumberedName(const std::string& dir, uint64_t number, const char* suffix) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s", static_cast<unsigned long long>(number), suffix);
  return dir + buf;
}

}  // namespace

std::string LogFileName(const std::string& dir, uint64_t number) {
  return NumberedName(dir, number, "log");
}

std::string TableFileName(const std::string& dir, uint64_t number) {
  return NumberedName(dir, number, "sst");
}

std::string ManifestFileName(const std::string& dir) { return dir + "/MANIFEST"; }

FileType ParseFileName(const std::string& name, uint64_t* number) {
  if (name == "MANIFEST") return FileType::kManifest;
  if (name == "LOCK") return FileType::kLock;
  size_t dot = name.find('.');
  if (dot == 0 || dot == std::string::npos) return FileType::kUnknown;
  for (size_t i = 0; i < dot; i++) {
    if (name[i] < '0' || name[i] > '9') return FileType::kUnknown;
  }
  std::string suffix = name.substr(dot + 1);
  FileType type;
  if (suffix == "log") {
    type = FileType::kLog;
  } else if (suffix == "sst") {
    type = FileType::kTable;
  } else {
    return FileType::kUnknown;
  }
  *number = std::strtoull(name.substr(0, dot).c_str(), nullptr, 10);
  return type;
}

}  // namespace ledgerkv
