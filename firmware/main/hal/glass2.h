#pragma once

#include <cstddef>
#include <cstdint>

constexpr std::size_t GLASS2_USAGE_SLOT_COUNT = 4;

struct Glass2UsageItem {
    const char* id;
    int percent;
    bool infinite = false;
};

// Initializes the SSD1309 Glass2 on CoreS3 Port A. A missing display is
// non-fatal and returns false.
bool glass2_init();

// Backward-compatible Grok-only update.
bool glass2_update_usage(int percent, std::int64_t updated_at_unix_seconds);

// Applies up to four partial service updates and redraws all retained slots.
// Unknown IDs occupy the next available slot and are ignored when full.
bool glass2_update_usage_items(const Glass2UsageItem* items, std::size_t count,
                               std::int64_t updated_at_unix_seconds);
