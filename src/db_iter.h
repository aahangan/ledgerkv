#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "internal_iterator.h"
#include "ledgerkv/iterator.h"

namespace ledgerkv {

// Turns an internal iterator (every version, every tombstone) into the public
// view at sequence seq: for each user key, only the newest version with
// sequence <= seq, and nothing for keys whose newest such version is a
// tombstone. pins keeps the memtables and Version being read alive.
std::unique_ptr<Iterator> NewDBIterator(std::unique_ptr<InternalIterator> internal, uint64_t seq,
                                        std::vector<std::shared_ptr<const void>> pins);

}  // namespace ledgerkv
