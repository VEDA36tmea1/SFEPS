#ifndef SAMPLE_UTILS_H
#define SAMPLE_UTILS_H

#include <cstddef>
#include <cstdint>

inline bool should_sample(std::uint64_t counter, std::size_t interval) {
    if (counter == 1) return true;
    if (interval == 0) return false;
    return (counter % interval) == 0;
}

#endif
