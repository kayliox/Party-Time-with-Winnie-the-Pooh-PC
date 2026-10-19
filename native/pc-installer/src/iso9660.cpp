// ISO 9660 / Joliet reader for the installer (see iso9660.h). Directory records are parsed only inside the sectors of
// their directory extent; extents outside the image, multi-extent files and oversized directories are rejected.
#include "iso9660.h"

#include <algorithm>
#include <cwctype>

namespace {

const uint32_t kMaxDirBytes = 4u << 20;  // a disc directory larger than 4 MB is not a real disc

uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

bool SameName(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (towlower(a[i]) != towlower(b[i])) return false;
    return true;
}

std::vector<std::wstring> SplitPath(const std::wstring& path) {
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : path) {
        if (c == L'/' || c == L'\\') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

}  // namespace

IsoImage::~IsoImage() {
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}

bool IsoImage::ReadSectors(uint32_t lba, uint32_t count, uint8_t* out) {
    for (uint32_t i = 0; i < count; i++) {
        uint64_t off = uint64_t(lba + i) * sectorSize_ + dataOffset_;
        if (off + 2048 > fileSize_) return false;
        LARGE_INTEGER li;
        li.QuadPart = LONGLONG(off);
        DWORD got = 0;
        if (!SetFilePointerEx(file_, li, nullptr, FILE_BEGIN) || !ReadFile(file_, out + size_t(i) * 2048, 2048, &got, nullptr) || got != 2048)
            return false;
    }
    return true;
}

bool IsoImage::Open(const std::wstring& path, std::wstring* error) {
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        *error = L"The file cannot be opened.";
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file_, &size) || size.QuadPart < 17 * 2048) {
        *error = L"The file is too small to be a disc image.";
        return false;
    }
    fileSize_ = uint64_t(size.QuadPart);
    // a raw image (.bin) has a 12-byte sync pattern at the start of every 2352-byte sector
    static const uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    uint8_t raw[16];
    LARGE_INTEGER li;
    li.QuadPart = 16LL * 2352;
    DWORD got = 0;
    if (fileSize_ >= 17 * 2352 && SetFilePointerEx(file_, li, nullptr, FILE_BEGIN) && ReadFile(file_, raw, 16, &got, nullptr) && got == 16 &&
        memcmp(raw, sync, 12) == 0) {
        sectorSize_ = 2352;
        dataOffset_ = raw[15] == 2 ? 24 : 16;  // Mode 2 Form 1 has an 8-byte subheader
    }
    uint8_t vd[2048];
    bool havePvd = false;
    uint8_t rootRec[34] = {};
    for (uint32_t lba = 16; lba < 32; lba++) {
        if (!ReadSectors(lba, 1, vd) || memcmp(vd + 1, "CD001", 5) != 0) break;
        if (vd[0] == 255) break;  // terminator
        bool jolietSvd = vd[0] == 2 && vd[88] == '%' && vd[89] == '/' && (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E');
        if ((vd[0] == 1 && !havePvd && !joliet_) || jolietSvd) {
            memcpy(rootRec, vd + 156, 34);
            havePvd = true;
            joliet_ = jolietSvd;
        }
    }
    if (!havePvd) {
        *error = L"This is not an ISO 9660 disc image (no volume descriptor). Images in other formats (MDF, NRG, CCD) can be mounted or converted to ISO first.";
        return false;
    }
    root_.dir = true;
    root_.lba = Le32(rootRec + 2);
    root_.size = Le32(rootRec + 10);
    if (root_.size == 0 || root_.size > kMaxDirBytes || uint64_t(root_.lba) * sectorSize_ >= fileSize_) {
        *error = L"The disc image's root directory is damaged.";
        return false;
    }
    return true;
}

bool IsoImage::List(const IsoEntry& dir, std::vector<IsoEntry>* out) {
    out->clear();
    if (!dir.dir || dir.size == 0 || dir.size > kMaxDirBytes) return false;
    uint32_t sectors = (dir.size + 2047) / 2048;
    std::vector<uint8_t> buf(size_t(sectors) * 2048);
    if (!ReadSectors(dir.lba, sectors, buf.data())) return false;
    size_t pos = 0, end = dir.size;
    while (pos < end) {
        uint8_t len = buf[pos];
        if (len == 0) {  // records never cross a sector: the rest of this sector is padding
            pos = (pos / 2048 + 1) * 2048;
            continue;
        }
        if (len < 34 || pos + len > end) return false;
        const uint8_t* r = buf.data() + pos;
        uint8_t nameLen = r[32];
        if (33u + nameLen > len) return false;
        uint8_t flags = r[25];
        if (!(nameLen == 1 && (r[33] == 0 || r[33] == 1))) {  // skip "." and ".."
            if (flags & 0x80) return false;                     // multi-extent files never occur on these discs
            IsoEntry e;
            e.dir = (flags & 2) != 0;
            e.lba = Le32(r + 2);
            e.size = Le32(r + 10);
            if (joliet_) {
                for (uint8_t i = 0; i + 1 < nameLen; i += 2) e.name += wchar_t(r[33 + i] << 8 | r[34 + i]);
            } else {
                for (uint8_t i = 0; i < nameLen; i++) e.name += wchar_t(r[33 + i]);
            }
            size_t semi = e.name.find(L';');
            if (semi != std::wstring::npos) e.name.resize(semi);
            while (!e.name.empty() && e.name.back() == L'.') e.name.pop_back();  // "NL." -> "NL", as the CD driver shows it
            if (e.name.empty() || e.name.find_first_of(L"/\\:*?\"<>|") != std::wstring::npos || e.name == L"..") return false;
            if (uint64_t(e.lba) * sectorSize_ + (e.dir ? 0 : uint64_t(e.size) / 2048 * sectorSize_) > fileSize_ + sectorSize_) return false;
            out->push_back(e);
        }
        pos += len;
    }
    return true;
}

bool IsoImage::Find(const std::wstring& path, IsoEntry* out) {
    IsoEntry cur = root_;
    std::vector<IsoEntry> items;
    for (const std::wstring& part : SplitPath(path)) {
        if (!cur.dir || !List(cur, &items)) return false;
        auto it = std::find_if(items.begin(), items.end(), [&](const IsoEntry& e) { return SameName(e.name, part); });
        if (it == items.end()) return false;
        cur = *it;
    }
    *out = cur;
    return true;
}

bool IsoImage::Read(const IsoEntry& file, const std::function<bool(const uint8_t*, size_t)>& sink) {
    if (file.dir) return false;
    const uint32_t chunk = 256;  // sectors per read (512 KB)
    std::vector<uint8_t> buf(size_t(chunk) * 2048);
    uint32_t left = file.size, lba = file.lba;
    while (left > 0) {
        uint32_t sectors = std::min(chunk, (left + 2047) / 2048);
        if (sectorSize_ == 2048) {  // contiguous: one read
            uint64_t off = uint64_t(lba) * 2048;
            if (off + uint64_t(sectors) * 2048 > fileSize_ + 2047) return false;
            LARGE_INTEGER li;
            li.QuadPart = LONGLONG(off);
            DWORD want = std::min<uint32_t>(sectors * 2048, left), got = 0;
            if (!SetFilePointerEx(file_, li, nullptr, FILE_BEGIN) || !ReadFile(file_, buf.data(), want, &got, nullptr) || got != want) return false;
            if (!sink(buf.data(), want)) return false;
            left -= want;
        } else {
            if (!ReadSectors(lba, sectors, buf.data())) return false;
            uint32_t n = std::min<uint32_t>(sectors * 2048, left);
            if (!sink(buf.data(), n)) return false;
            left -= n;
        }
        lba += sectors;
    }
    return true;
}
