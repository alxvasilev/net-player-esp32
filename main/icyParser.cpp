#include "icyParser.hpp"
#include <assert.h>
#include <avLog.hpp>

static const char *TAG = "icy-parser";
bool IcyParser::parseHeader(const char* key, const char* value)
{
    if (strcasecmp(key, "icy-metaint") == 0) {
        mIcyInterval = atoi(value);
        mIcyCtr = 0;
        AV_LOGI(TAG, "Response contains ICY metadata with interval %ld", mIcyInterval);
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
bool IcyParser::processRecvData(char* recvData, int& totalDataLen)
{
    auto& icyMeta = mIcyMetaBuf;
    bool hadTitle = false;
    char* buf = recvData;
    int bufLen = totalDataLen;
    bool metaIsNotNull = true; // initialize it just to prevent warning
    for(;;) {
        int metaOffset, metaLen;
        char* metaStart;
        if (mIcyRemaining) { // we are receiving non-empty metadata
            metaOffset = 0;
            metaStart = buf;
            metaLen = std::min(bufLen, (int)mIcyRemaining);
            icyMeta.appendStr(buf, metaLen, true);
            mIcyRemaining -= metaLen;
        }
        else { // not receiving metadata
            if (mIcyCtr + bufLen <= mIcyInterval) { // no metadata in buffer
                mIcyCtr += bufLen;
                return hadTitle;
            }
            // metadata starts somewhere in our buffer
            metaOffset = mIcyInterval - mIcyCtr;
            metaStart = buf + metaOffset;
            int metaTotalSize = (*metaStart << 4) + 1; // includes the length byte
            metaLen = std::min(bufLen - metaOffset, metaTotalSize);
            mIcyRemaining = metaTotalSize - metaLen;

            metaIsNotNull = metaTotalSize > 1;
            if (metaIsNotNull) { // non-null metadata
                icyMeta.clear();
                icyMeta.ensureFreeSpace(metaTotalSize); // includes terminating null
                if (metaLen > 1) { // first byte is length
                    icyMeta.appendStr(metaStart+1, metaLen-1, true);
                }
            }
        }
        totalDataLen -= metaLen;
        if (mIcyRemaining > 0) { // metadata continues in next chunk
            mIcyCtr = 0;
            return hadTitle;
        }
        // meta starts and ends in our buffer
        assert(mIcyRemaining == 0);
        if (metaIsNotNull) {
            parseIcyData();
            hadTitle = true;
        }
        int remain = bufLen - metaOffset - metaLen;
        if (remain > 0) { // we have stream data after the metadata
            memmove(metaStart, metaStart + metaLen, remain);
            if (remain <= mIcyInterval) {
                mIcyCtr = remain;
                return hadTitle; // no more metadata in packet
            }
            else {
                mIcyCtr = mIcyInterval;
                buf = metaStart + mIcyInterval;
                bufLen = remain - mIcyInterval;
                continue;
            }
        }
        else {
            assert(remain == 0);
            mIcyCtr = 0; // no stream data after metadata
            return hadTitle;
        }
    }
}
void IcyParser::parseIcyData() // we extract only StreamTitle
{
    const char kStreamTitlePrefix[] = "StreamTitle='";
    auto start = strstr(mIcyMetaBuf.buf(), kStreamTitlePrefix);
    if (!start) {
        AV_LOGW(TAG, "ICY parse error: 'StreamTitle=' string not found");
        return;
    }
    start += sizeof(kStreamTitlePrefix)-1; //sizeof(kStreamTitlePreix) incudes the terminating NULL
    auto end = strchr(start, ';');
    int titleSize;
    if (!end) {
        AV_LOGW(TAG, "ICY parse error: Closing ';' of StreamTitle not found");
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

