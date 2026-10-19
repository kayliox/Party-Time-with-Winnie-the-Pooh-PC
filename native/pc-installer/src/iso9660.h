// Reads files from an ISO 9660 disc image: .iso (2048-byte sectors) or raw .bin (2352-byte sectors, Mode 1 or Mode 2
// Form 1), preferring the Joliet tree. The image is untrusted input: every size and offset is bounds-checked.
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct IsoEntry {
    std::wstring name;  // as the CD driver shows it: Joliet name, or ISO name without ";1" and a trailing dot
    bool dir = false;
    uint32_t lba = 0;
    uint32_t size = 0;
};

class IsoImage {
public:
    ~IsoImage();
    bool Open(const std::wstring& path, std::wstring* error);
    // path components separated by '/' or '\', compared case-insensitively; L"" = the root
    bool Find(const std::wstring& path, IsoEntry* out);
    bool List(const IsoEntry& dir, std::vector<IsoEntry>* out);
    // streams the file's bytes to sink in chunks; sink returns false to stop (then Read returns false)
    bool Read(const IsoEntry& file, const std::function<bool(const uint8_t*, size_t)>& sink);
    const IsoEntry& Root() const { return root_; }

private:
    bool ReadSectors(uint32_t lba, uint32_t count, uint8_t* out);
    HANDLE file_ = INVALID_HANDLE_VALUE;
    uint64_t fileSize_ = 0;
    uint32_t sectorSize_ = 2048, dataOffset_ = 0;
    bool joliet_ = false;
    IsoEntry root_;
};
