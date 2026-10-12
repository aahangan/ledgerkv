#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>

#include "testing.h"

namespace ledgerkv::testing {

std::string TempDir() {
  std::string tmpl = (std::filesystem::temp_directory_path() / "ledgerkv-test-XXXXXX").string();
  char* dir = ::mkdtemp(tmpl.data());
  CHECK(dir != nullptr);
  return dir;
}

}  // namespace ledgerkv::testing

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);  // Show progress even when output is piped.
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  for (const auto& t : ledgerkv::testing::Registry()) {
    if (filter && std::strstr(t.name, filter) == nullptr) continue;
    std::printf("[ RUN  ] %s\n", t.name);
    t.fn();
    std::printf("[  OK  ] %s\n", t.name);
    ran++;
  }
  std::printf("%d tests passed\n", ran);
  return 0;
}
