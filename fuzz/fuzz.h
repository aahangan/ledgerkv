#pragma once

// Shared pieces for the fuzz targets. Each target defines
// LLVMFuzzerTestOneInput and FuzzSeeds(). Built with -DLEDGERKV_FUZZ=ON they
// run under libFuzzer (coverage-guided). Otherwise driver.cc supplies a main
// that mutates the seeds at random, which works with any compiler.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// Representative valid inputs for the target, used as the starting corpus.
std::vector<std::string> FuzzSeeds();

#define FUZZ_CHECK(cond)                                                          \
  do {                                                                            \
    if (!(cond)) {                                                                \
      std::fprintf(stderr, "%s:%d: FUZZ_CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      std::abort();                                                               \
    }                                                                             \
  } while (0)

namespace ledgerkv::fuzz {

// Seed builders, shared with the tool that writes the libFuzzer corpus.
std::vector<std::string> LogSeeds();
std::vector<std::string> WriteBatchSeeds();
std::vector<std::string> TableSeeds();
std::vector<std::string> DBSeeds();

// A scratch path unique to this process.
std::string ScratchPath(const std::string& name);

}  // namespace ledgerkv::fuzz
