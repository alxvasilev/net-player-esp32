#include "icyParser.hpp"
#include "esp_log.h"
#include <assert.h>

static const char *TAG = "icy-parser";
bool IcyParser::parseHeader(const char* key, const char* value)
{
    if (strcasecmp(key, "icy-metaint") == 0) {
        mIcyInterval = atoi(value);
        mIcyCtr = 0;
        ESP_LOGI(TAG, "Response contains ICY metadata with interval %ld", mIcyInterval);
    } else if (strcasecmp(key, "icy-name") == 0) {
        MutexLocker locker(mInfoMutex);
        mStaName.reset(strdup(value));
    } else if (strcasecmp(key, "icy-description") == 0) {
        MutexLocker locker(mInfoMutex);
        mStaDesc.reset(strdup(value));
    } else if (strcasecmp(key, "icy-genre") == 0) {
        MutexLocker locker(mInfoMutex);
        mStaGenre.reset(strdup(value));
    } else if (strcasecmp(key, "icy-url") == 0) {
        MutexLocker locker(mInfoMutex);
        mStaUrl.reset(strdup(value));
    } else {
        return false;
    }
    return true;
}
bool IcyParser::processRecvData(char* buf, int& rlen)
{
    auto& icyMeta = mIcyMetaBuf;
    if (mIcyRemaining) { // we are receiving non-empty metadata
        assert(icyMeta.buf());
        auto metaLen = std::min(rlen, (int)mIcyRemaining);
        icyMeta.append(buf, metaLen);
        mIcyRemaining -= metaLen;
        if (mIcyRemaining <= 0) { // meta data complete
            assert(mIcyRemaining == 0);
            icyMeta.appendChar(0);
            mIcyCtr = rlen - metaLen;
            if (mIcyCtr > 0) {
                // there is stream data after end of metadata
                memmove(buf, buf + metaLen, mIcyCtr);
            } else { // no stream data after meta
                assert(mIcyCtr == 0);
            }
            parseIcyData();
            rlen = mIcyCtr;
            return true;
        } else { // metadata continues in next chunk
            rlen = 0; // all this chunk was metadata
            return false;
        }
    } else { // not receiving metadata
        if (mIcyCtr + rlen <= mIcyInterval) {
            mIcyCtr += rlen;
            return false; // no metadata in buffer
        }
        // metadata starts somewhere in our buffer
        int metaOffset = mIcyInterval - mIcyCtr;
        auto metaStart = buf + metaOffset;
        int metaTotalSize = (*metaStart << 4) + 1; // includes the length byte
        int metaChunkSize = std::min(rlen - metaOffset, metaTotalSize);
        mIcyRemaining = metaTotalSize - metaChunkSize;

        if (metaTotalSize > 1) {
            icyMeta.clear();
            icyMeta.ensureFreeSpace(metaTotalSize); // includes terminating null
            if (metaChunkSize > 1) { // first byte is length
                icyMeta.append(metaStart+1, metaChunkSize-1);
            }
        }
        if (mIcyRemaining > 0) { // metadata continues in next chunk
            mIcyCtr = 0;
            rlen = metaOffset;
            return false;
        }
        // meta starts and ends in our buffer
        assert(mIcyRemaining == 0);
        int remLen = rlen - metaOffset - metaTotalSize;
        mIcyCtr = remLen;
        if (metaTotalSize > 1) {
            icyMeta.appendChar(0);
            parseIcyData();
        }
        // else don't clear current title

        if (remLen > 0) { // we have stream data after the metadata
            memmove(metaStart, metaStart + metaTotalSize, remLen);
            rlen = metaOffset + remLen;
        } else {
            rlen = metaOffset; // no stream data after metadata
        }
        return metaTotalSize > 1;
    }
}
void IcyParser::parseIcyData() // we extract only StreamTitle
{
    const char kStreamTitlePrefix[] = "StreamTitle='";
    auto start = strstr(mIcyMetaBuf.buf(), kStreamTitlePrefix);
    if (!start) {
        ESP_LOGW(TAG, "ICY parse error: StreamTitle= string not found");
        return;
    }
    start += sizeof(kStreamTitlePrefix)-1; //sizeof(kStreamTitlePreix) incudes the terminating NULL
    auto end = strchr(start, ';');
    int titleSize;
    if (!end) {
        ESP_LOGW(TAG, "ICY parse error: Closing quote of StreamTitle not found");
        titleSize = mIcyMetaBuf.dataSize() - sizeof(kStreamTitlePrefix) - 1;
    } else {
        end--; // move to closing quote
        titleSize = end - start;
    }
    memmove(mIcyMetaBuf.buf(), start, titleSize);
    mIcyMetaBuf[titleSize] = 0;
    mIcyMetaBuf.setDataSize(titleSize + 1);
    {
        MutexLocker locker(mInfoMutex);
        mTrackName.reset(mIcyMetaBuf.release());
    }
}

void IcyParser::reset()
{
    mIcyInterval = mIcyCtr = mIcyRemaining = 0;
    mIcyMetaBuf.clear();
    clearIcyInfo();
}

void IcyInfo::clearIcyInfo()
{
    MutexLocker locker(mInfoMutex);
    mTrackName.reset();
    mStaName.reset();
    mStaDesc.reset();
    mStaGenre.reset();
    mStaUrl.reset();
}

