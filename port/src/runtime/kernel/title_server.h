#pragma once
// Emulation of TTT2's Xbox Live title-storage file (default on, TTT2_TITLE_SERVER=0 disables); see
// title_server.cpp. While enabled, user 0 reports a Live sign-in.
#include <cstdint>
#include <vector>

bool TitleServerEnabled();
// The title_storage.bin image served for `level` (0..3), zero-padded to `bufferBytes`.
std::vector<uint8_t> TitleStorageImage(uint32_t level, uint32_t bufferBytes);
