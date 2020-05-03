#include "decoderNode.hpp"
#include "decoderMp3.hpp"

bool DecoderNode::createDecoder(esp_codec_type_t type)
{
    switch (type) {
    case ESP_CODEC_TYPE_MP3:
        ESP_LOGI(mTag, "Created MP3 decoder");
        mDecoder = new DecoderMp3(mVolume);
        return true;
    default: return false;
    }
}

bool DecoderNode::changeDecoder(esp_codec_type_t type)
{
    if (mDecoder) {
        delete mDecoder;
        mDecoder = nullptr;
    }
    return createDecoder(type);
}

AudioNode::StreamError DecoderNode::pullData(DataPullReq& odp, int timeout)
{
    for (;;) {
        DataPullReq idp(0);
        auto err = mPrev->pullData(idp, timeout);
        if (err) {
            if (err == kStreamFlush && mDecoder) {
                ESP_LOGW(mTag, "kStreamFlush returned by upstream node, resetting decoder");
                mDecoder->reset();
            }
            return err;
        }
        if (!mDecoder) {
            ESP_LOGI(mTag, "No decoder, getting codec info from upstream node");
            createDecoder(idp.fmt.codec); // clears any remaining data from input buffer
            if (!mDecoder) {
                return kErrNoCodec;
            }
            mFormatChangeCtr = idp.fmt.ctr;
            continue;
        }
        bool ctrChanged = idp.fmt.ctr != mFormatChangeCtr;
        bool codecChanged = idp.fmt.codec != mDecoder->type();
        if (ctrChanged || codecChanged) {
            auto ret = mDecoder->decode(nullptr, 0);
            if (ret <= 0) {
                mFormatChangeCtr = idp.fmt.ctr;
                if (codecChanged) {
                    ESP_LOGW(mTag, "Stream encoding changed");
                    changeDecoder(idp.fmt.codec);
                } else {
                    ESP_LOGW(mTag, "Stream changed, but codec not - resetting codec");
                    mDecoder->reset();
                }
                continue;
            } else {
                odp.buf = mDecoder->outputBuf();
                odp.size = ret;
                odp.fmt = mDecoder->outputFmt();
                return kNoError;
            }
        }

        int bytesNeeded = mDecoder->inputBytesNeeded();
        idp.size = bytesNeeded;
        idp.buf = nullptr;

        if (bytesNeeded > 0) {
            auto err = mPrev->pullData(idp, timeout);
            if (err) {
                return err;
            }
            myassert(idp.size > 0);
            myassert(idp.size <= bytesNeeded);
            myassert(idp.fmt.codec == mDecoder->type());
        }

        auto ret = mDecoder->decode(idp.buf, idp.size);
        mPrev->confirmRead(idp.size);
        if (ret == kNeedMoreData) {
            ESP_LOGI(mTag, "Need more data, repeating");
            continue;
        } else if (ret < 0) {
            return (StreamError)ret;
        }
        myassert(ret > 0);
        odp.buf = mDecoder->outputBuf();
        odp.size = ret;
        odp.fmt = mDecoder->outputFmt();
        return kNoError;
    }
}
uint8_t DecoderNode::getVolume() const
{
    return mVolume;
}

void DecoderNode::setVolume(uint8_t vol)
{
    mVolume = vol;
}
