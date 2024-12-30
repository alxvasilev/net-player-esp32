#pragma once

#include <pb_encode.h>
#include <optional>
#include <string_view>
#include <vector>
#include "NanoPBHelper.h"
#include "pb_decode.h"
#include <protobuf/spirc.pb.h>
#include <memory>

namespace cspot {
struct TrackGid: public std::vector<uint8_t> {
    bool operator<(const TrackGid& other) const {
        int diff = memcmp(data(), other.data(), std::min(size(), other.size()));
        if (size() == other.size()) {
            return diff < 0;
        }
        else { // sizes differ
            return (diff == 0) ? size() < other.size() : diff < 0;
       }
    }
    TrackGid& operator=(const std::vector<uint8_t>& other) {
        std::vector<uint8_t>::operator=(other);
        return *this;
    }
};
class TrackInfo;
struct TrackReference {
    enum Type: int8_t { TRACK, EPISODE };
    TrackGid gid;
    Type type = TRACK;
    bool operator==(const TrackReference& other) const {
        return other.type == type && other.gid == gid;
    }
    bool operator<(const TrackReference& other) const {
        int tdiff = (int8_t)type - (int8_t)other.type;
        return tdiff ? tdiff < 0 : gid < other.gid;
    }
    void decodeURI(const std::string& uri);
};
}  // namespace cspot
