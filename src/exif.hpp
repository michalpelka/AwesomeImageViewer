#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct MetaEntry {
    std::string group;  // "Camera", "Exif", "GPS", ...
    std::string name;
    std::string value;
};

// Parses EXIF from an in-memory file (JPEG, TIFF/DNG/most TIFF-based RAWs, PNG eXIf, WebP).
// Returns an empty vector when no EXIF block is found. Never throws on malformed data.
std::vector<MetaEntry> parseExif(const std::vector<uint8_t>& file);

// Short human readable container name from magic bytes ("JPEG", "PNG", ...).
std::string detectFormat(const std::vector<uint8_t>& file);
