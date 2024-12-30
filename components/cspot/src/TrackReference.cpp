#include "TrackReference.h"

#include "NanoPBExtensions.h"
#include "Utils.h"
#include "protobuf/spirc.pb.h"
#include "TrackQueue.h"

using namespace cspot;

static constexpr auto base62Alphabet =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

void TrackReference::decodeURI(const std::string& uri)
{
    if (gid.empty()) {
        // Episode GID is being fetched via base62 encoded URI
        auto idString = uri.substr(uri.find_last_of(":") + 1, uri.size());
        gid = {0};

        std::string_view alphabet(base62Alphabet);
        for (int x = 0; x < idString.size(); x++) {
            size_t d = alphabet.find(idString[x]);
            gid = bigNumMultiply(gid, 62);
            gid = bigNumAdd(gid, d);
        }

        if (uri.find("episode:") != std::string::npos) {
            type = Type::EPISODE;
        }
    }
}

