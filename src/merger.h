#pragma once

#include <memory>
#include <vector>

#include "internal_iterator.h"

namespace ledgerkv {

// Merges sorted children into a single stream in internal order using a
// binary heap: O(log k) per entry for k children.
std::unique_ptr<InternalIterator> NewMergingIterator(std::vector<std::unique_ptr<InternalIterator>> children);

}  // namespace ledgerkv
