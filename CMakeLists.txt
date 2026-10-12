cmake_minimum_required(VERSION 3.20)
project(ledgerkv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(LEDGERKV_SANITIZE "Build with a sanitizer: address, thread, or undefined" "")

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
if(LEDGERKV_SANITIZE)
  add_compile_options(-fsanitize=${LEDGERKV_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${LEDGERKV_SANITIZE})
endif()

add_library(ledgerkv
  src/bloom.cc
  src/crc32c.cc
  src/db.cc
  src/file_util.cc
  src/filename.cc
  src/log.cc
  src/memtable.cc
  src/status.cc
  src/table.cc
  src/table_builder.cc
  src/version_edit.cc
  src/wal.cc
)
target_include_directories(ledgerkv PUBLIC include PRIVATE src)

add_executable(ledgerkv_tests tests/test_main.cc tests/wal_test.cc tests/db_test.cc
  tests/table_test.cc tests/recovery_test.cc tests/compaction_test.cc)
target_link_libraries(ledgerkv_tests PRIVATE ledgerkv)
target_include_directories(ledgerkv_tests PRIVATE src tests)

add_executable(crash_test tools/crash_test.cc)
target_link_libraries(crash_test PRIVATE ledgerkv)

add_executable(kvbench bench/kvbench.cc)
target_link_libraries(kvbench PRIVATE ledgerkv)

enable_testing()
add_test(NAME unit COMMAND ledgerkv_tests)
add_test(NAME crash COMMAND crash_test --iterations 50)
