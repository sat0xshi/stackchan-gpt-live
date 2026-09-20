#pragma once

#include <cstdint>

// Initializes the SSD1309 Glass2 on CoreS3 Port A. A missing display is
// non-fatal and returns false.
bool glass2_init();

// Usage data contract for the future HTTP/JSON producer. The percentage must
// be in [0, 100]; updated_at_unix_seconds is the source update time.
// This is the only function that updates the Glass2 usage display.
bool glass2_update_usage(int percent, std::int64_t updated_at_unix_seconds);
