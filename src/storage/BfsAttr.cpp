#include "BfsAttr.h"

#include <Node.h>
#include <Volume.h>
#include <Entry.h>
#include <fs_attr.h>
#include <fs_index.h>

#include <cstring>

namespace daw {

bool WriteAttrFloat(const char* path, const char* name, float v) {
    BNode node(path);
    if (node.InitCheck() != B_OK) return false;
    return node.WriteAttr(name, B_FLOAT_TYPE, 0, &v, sizeof(v)) == (ssize_t)sizeof(v);
}

bool ReadAttrFloat(const char* path, const char* name, float* out) {
    BNode node(path);
    if (node.InitCheck() != B_OK) return false;
    float v = 0.0f;
    if (node.ReadAttr(name, B_FLOAT_TYPE, 0, &v, sizeof(v)) != (ssize_t)sizeof(v))
        return false;
    if (out) *out = v;
    return true;
}

bool WriteAttrString(const char* path, const char* name, const std::string& s) {
    BNode node(path);
    if (node.InitCheck() != B_OK) return false;
    // Include the terminating NUL so it round-trips as a B_STRING_TYPE.
    const size_t n = s.size() + 1;
    return node.WriteAttr(name, B_STRING_TYPE, 0, s.c_str(), n) == (ssize_t)n;
}

bool ReadAttrString(const char* path, const char* name, std::string* out) {
    BNode node(path);
    if (node.InitCheck() != B_OK) return false;
    attr_info info;
    if (node.GetAttrInfo(name, &info) != B_OK || info.size <= 0) return false;
    std::string buf;
    buf.resize((size_t)info.size);
    if (node.ReadAttr(name, B_STRING_TYPE, 0, &buf[0], buf.size())
        != (ssize_t)buf.size())
        return false;
    if (!buf.empty() && buf.back() == '\0') buf.pop_back();  // drop NUL
    if (out) *out = buf;
    return true;
}

void EnsureDawIndexes(const char* pathOnVolume) {
    BEntry entry(pathOnVolume);
    BVolume vol;
    if (entry.InitCheck() != B_OK || entry.GetVolume(&vol) != B_OK) return;
    const dev_t dev = vol.Device();
    // Create each index; B_FILE_EXISTS just means it's already there.
    fs_create_index(dev, kAttrBpm,      B_FLOAT_TYPE,  0);
    fs_create_index(dev, kAttrDuration, B_FLOAT_TYPE,  0);
    fs_create_index(dev, kAttrKey,      B_STRING_TYPE, 0);
}

} // namespace daw
