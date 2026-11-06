// PC water: data of the user's own Far Cry (PC, 2004) install, read at run time and never shipped or stored (ini key
// fc1_path; default "asset_fc1\Far Cry" in the folder that holds the game's assets\, searched upwards from the exe).
// The .pak files are zip archives: a reader of our own (central directory + inflate, RFC 1951).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gpu::fc1 {

// the install's FCData folder, or "" when there is none (logged once)
const std::string& DataFolder();

// a file of a .pak in FCData (e.g. "Textures.pak", "Textures/Waterbump.dds"; names compare without case); false when
// the pak or the file is missing or broken
bool ReadPakFile(const char* pak, const char* name, std::vector<uint8_t>& out);

// an uncompressed 24 or 32 bit DDS: level 0 as one 8 bit channel (red), false for anything else
bool DdsGray(const std::vector<uint8_t>& dds, uint32_t& width, uint32_t& height, std::vector<uint8_t>& gray);

}  // namespace gpu::fc1
