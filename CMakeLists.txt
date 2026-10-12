cmake_minimum_required(VERSION 3.20)
project(ledgerkv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(LEDGERKV_SANITIZE "Build with a sanitizer: address, thread, or undefined" "")
option(LEDGERKV_FUZZ "Build fuzz targets for libFuzzer (needs a clang that ships it)" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
if(LEDGERKV_SANITIZE)
  add_compile_options(-fsanitize=${LEDGERKV_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${LEDGERKV_SANITIZE})
endif()
if(LEDGERKV_FUZZ)
  # Coverage instrumentation everywhere, so libFuzzer is guided by the library too.
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()

add_library(ledgerkv
  src/arena.cc
  src/bloom.cc
  src/crc32c.cc
  src/db.cc
  src/db_iter.cc
  src/env_posix.cc
  src/file_util.cc
  src/filename.cc
  src/log.cc
  src/memtable.cc
  src/merger.cc
  src/status.cc
  src/table.cc
  src/table_builder.cc
  src/version.cc
  src/version_edit.cc
  src/wal.cc
  src/write_batch.cc
)
target_include_directories(ledgerkv PUBLIC include PRIVATE src)

# Test-only support: the fault-injecting Env.
add_library(ledgerkv_testutil src/fault_env.cc)
target_link_libraries(ledgerkv_testutil PUBLIC ledgerkv)
target_include_directories(ledgerkv_testutil PUBLIC src)

add_executable(ledgerkv_tests tests/test_main.cc tests/wal_test.cc tests/db_test.cc
  tests/table_test.cc tests/recovery_test.cc tests/compaction_test.cc
  tests/snapshot_test.cc tests/fault_test.cc)
target_link_libraries(ledgerkv_tests PRIVATE ledgerkv_testutil)
target_include_directories(ledgerkv_tests PRIVATE src tests)

add_executable(crash_test tools/crash_test.cc)
target_link_libraries(crash_test PRIVATE ledgerkv)

add_executable(powerloss_test tools/powerloss_test.cc)
target_link_libraries(powerloss_test PRIVATE ledgerkv_testutil)

add_executable(kvbench bench/kvbench.cc)
target_link_libraries(kvbench PRIVATE ledgerkv)

# Fuzz targets. With LEDGERKV_FUZZ=ON they link libFuzzer; otherwise
# fuzz/driver.cc provides a random-mutation main that works with any compiler.
add_library(ledgerkv_fuzzseeds fuzz/seeds.cc)
target_link_libraries(ledgerkv_fuzzseeds PUBLIC ledgerkv_testutil)
target_include_directories(ledgerkv_fuzzseeds PUBLIC fuzz)
foreach(target log write_batch table db)
  add_executable(fuzz_${target} fuzz/fuzz_${target}.cc)
  target_link_libraries(fuzz_${target} PRIVATE ledgerkv_fuzzseeds)
  if(LEDGERKV_FUZZ)
    target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)
  else()
    target_sources(fuzz_${target} PRIVATE fuzz/driver.cc)
  endif()
endforeach()
add_executable(fuzz_seeds fuzz/write_seeds.cc)
target_link_libraries(fuzz_seeds PRIVATE ledgerkv_fuzzseeds)

enable_testing()
add_test(NAME unit COMMAND ledgerkv_tests)
add_test(NAME crash COMMAND crash_test --iterations 50)
add_test(NAME powerloss COMMAND powerloss_test --iterations 100 --sync)
if(NOT LEDGERKV_FUZZ)
  foreach(target log write_batch table db)
    add_test(NAME fuzz_${target} COMMAND fuzz_${target} --iterations 2000)
  endforeach()
endif()
