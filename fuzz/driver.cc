// A main for fuzz targets when libFuzzer is not available (Apple clang does
// not ship it). Not coverage-guided: it mutates the seed corpus at random
// (bit flips, byte overwrites, insertions, deletions, truncations, splices)
// and feeds each mutant to the target. Any crash, sanitizer report, or
// FUZZ_CHECK failure aborts with the input saved for reproduction.
//
//   fuzz_x [--iterations N] [--seed S] [file...]
//
// With files, runs each file once instead (to replay a saved crash).

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "fuzz.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace {

std::string current;

void SaveOnCrash(int sig) {
  std::ofstream("fuzz-crash.bin", std::ios::binary).write(current.data(), static_cast<std::streamsize>(current.size()));
  std::fprintf(stderr, "\ncrashing input (%zu bytes) saved to fuzz-crash.bin\n", current.size());
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

std::string Mutate(const std::vector<std::string>& corpus, std::mt19937_64& rng) {
  std::string s = corpus[rng() % corpus.size()];
  int rounds = 1 + static_cast<int>(rng() % 6);
  for (int r = 0; r < rounds; r++) {
    switch (rng() % 7) {
      case 0:  // Flip a bit.
        if (!s.empty()) s[rng() % s.size()] ^= static_cast<char>(1 << (rng() % 8));
        break;
      case 1:  // Overwrite a byte, favoring boundary values.
        if (!s.empty()) {
          static const char kInteresting[] = {0, 1, 0x7f, static_cast<char>(0x80), static_cast<char>(0xff)};
          s[rng() % s.size()] = rng() % 2 ? kInteresting[rng() % 5] : static_cast<char>(rng());
        }
        break;
      case 2:  // Insert random bytes.
        s.insert(rng() % (s.size() + 1), std::string(1 + rng() % 8, static_cast<char>(rng())));
        break;
      case 3:  // Delete a range.
        if (!s.empty()) {
          size_t pos = rng() % s.size();
          s.erase(pos, 1 + rng() % std::min<size_t>(16, s.size() - pos));
        }
        break;
      case 4:  // Truncate.
        if (!s.empty()) s.resize(rng() % s.size());
        break;
      case 5: {  // Splice with another corpus entry.
        const std::string& other = corpus[rng() % corpus.size()];
        if (!other.empty()) s = s.substr(0, rng() % (s.size() + 1)) + other.substr(rng() % other.size());
        break;
      }
      case 6:  // Duplicate a range.
        if (!s.empty()) {
          size_t pos = rng() % s.size(), len = 1 + rng() % std::min<size_t>(32, s.size() - pos);
          s.insert(rng() % (s.size() + 1), s.substr(pos, len));
        }
        break;
    }
  }
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t iterations = 20000, seed = std::random_device{}();
  std::vector<std::string> files;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--iterations" && i + 1 < argc) iterations = std::strtoull(argv[++i], nullptr, 10);
    else if (arg == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
    else files.push_back(arg);
  }
  std::signal(SIGABRT, SaveOnCrash);
  std::signal(SIGSEGV, SaveOnCrash);

  if (!files.empty()) {
    for (const auto& f : files) {
      std::ifstream in(f, std::ios::binary);
      std::stringstream buf;
      buf << in.rdbuf();
      current = buf.str();
      LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(current.data()), current.size());
    }
    std::printf("ran %zu inputs\n", files.size());
    return 0;
  }

  std::vector<std::string> corpus = FuzzSeeds();
  for (const auto& s : corpus) {
    current = s;
    LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(current.data()), current.size());
  }
  std::mt19937_64 rng(seed);
  for (uint64_t i = 0; i < iterations; i++) {
    current = Mutate(corpus, rng);
    LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(current.data()), current.size());
    // Keep some mutants as new parents so mutations can compound.
    if (rng() % 64 == 0 && corpus.size() < 256) corpus.push_back(current);
  }
  std::printf("%llu mutated inputs (seed %llu), no failures\n", static_cast<unsigned long long>(iterations),
              static_cast<unsigned long long>(seed));
  return 0;
}
