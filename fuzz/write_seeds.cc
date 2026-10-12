// Writes the seed corpus for each fuzz target into DIR/<target>/, for use
// with libFuzzer:  fuzz_seeds DIR && fuzz_log DIR/log
#include <filesystem>
#include <fstream>

#include "fuzz.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: fuzz_seeds DIR\n");
    return 2;
  }
  using ledgerkv::fuzz::DBSeeds;
  using ledgerkv::fuzz::LogSeeds;
  using ledgerkv::fuzz::TableSeeds;
  using ledgerkv::fuzz::WriteBatchSeeds;
  const std::pair<const char*, std::vector<std::string>> targets[] = {
      {"log", LogSeeds()}, {"write_batch", WriteBatchSeeds()}, {"table", TableSeeds()}, {"db", DBSeeds()}};
  for (const auto& [name, seeds] : targets) {
    std::filesystem::path dir = std::filesystem::path(argv[1]) / name;
    std::filesystem::create_directories(dir);
    for (size_t i = 0; i < seeds.size(); i++) {
      std::ofstream(dir / ("seed" + std::to_string(i)), std::ios::binary)
          .write(seeds[i].data(), static_cast<std::streamsize>(seeds[i].size()));
    }
  }
  return 0;
}
