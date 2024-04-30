#include "dlna-parse.hpp"
#include <tinyxml2.h>
#include <sstream>

const char* kZeroHmsTime = "0:00:00.000";
using namespace tinyxml2;

const XMLElement* xmlFindPath(const XMLNode* node, const char* path)
{
    if (!path || !node) {
        return nullptr;
    }
    char buf[32];
    for(;;) {
        while (*path == '/') {
            path++;
        }
        const char* end = strchr(path, '/');
        int len = end ? (end - path) : strlen(path);
        if (!len) {
            return node->ToElement();
        }
        if (len > sizeof(buf)-1) {
            printf("Tag name exceeded max internal buf size\n");
            return nullptr;
        }
        memcpy(buf, path, len);
        buf[len] = 0;
        node = node->FirstChildElement(buf);
        if (!node) {
            return nullptr;
        }
        if (!end) {
            return node->ToElement();
        }
        path = end + 1;
    }
}


std::string msToHmsString(uint32_t ms)
{
    std::ostringstream oss;
    oss << ms / 3600000 << ':';
    ms %= 3600000;
    oss << ms / 60000 << ':';
    ms %= 60000;
    oss << ms / 1000 << '.';
    oss << (ms % 1000);
    return oss.str();
}

uint32_t parseHmsTime(const char* hms)
{
    if (!hms) {
        return -1;
    }
    const char* pos = hms;
    uint32_t result = 0;
    for (; *pos; pos++) {
        if (*pos == '.') {
            result = atoi(pos+1);
            if (result > 999) {
                return 0xffffffff;
            }
            break;
        }
    }
    char buf[8];
    for (int mult = 1000; mult <= 3600000; mult *= 60) {
        auto end = pos;
        for (pos--; pos >= hms; pos--) {
            if (*pos == ':') {
                break;
            }
        }
        pos++;
        auto lNum = end - pos;
        if ((lNum < 1) || (mult < 3600000 && lNum > 2) || (lNum > sizeof(buf)-1)) {
            return 0xffffffff;
        }
        memcpy(buf, pos, lNum);
        buf[lNum] = 0;
        result += atoi(buf) * mult;
        if (pos == hms) {
            return result;
        }
        pos--;
    }
    return result;
}
