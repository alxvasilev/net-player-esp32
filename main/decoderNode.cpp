#include "decoderNode.hpp"
#include "decoderMp3.hpp"

bool DecoderNode::createDecoder(esp_codec_type_t type)
{
    switch (type) {
    case ESP_CODEC_TYPE_MP3:
        ESP_LOGI(mTag, "Created MP3 decoder");
        mDecoder = new DecoderMp3(mVolume);
        return true;
    default:
        ESP_LOGW(mTag, "No decoder for codec type %s", AudioNode::codecTypeToStr(type));
        return false;
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
    if (timeout < 0) {
        timeout = 0x7fffffff;
    }
    for (;;) {
        if (timeout < 0) {
            return kTimeout;
        }
        // get only stream format, no data, but wait for data to be available (so we know the stream format)
        DataPullReq idp(0);
        ElapsedTimer tim;
        auto err = mPrev->pullData(idp, timeout);
        if (err) {
            if (err == kStreamFlush && mDecoder) {
                ESP_LOGW(mTag, "kStreamFlush returned by upstream node, resetting decoder");
                mDecoder->reset();
            }
            return err;
        }
        timeout -= tim.msElapsed();
        if (timeout <= 0) {
            return kTimeout;
        }
        if (!mDecoder) {
            ESP_LOGI(mTag, "No decoder, creating one");
            createDecoder(idp.fmt.codec); // clears any remaining data from input buffer
            if (!mDecoder) {
                return kErrNoCodec;
            }
            mFormatChangeCtr = idp.fmt.ctr;
            continue;
        }
        // handle format/stream change
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
        // do actual stream read and decode
        int bytesNeeded = mDecoder->inputBytesNeeded();
        int ret;
        if (bytesNeeded > 0) {
            tim.reset();
            idp.reset(bytesNeeded);
            auto err = mPrev->pullData(idp, timeout);
            if (err) {
                return err;
            }
            timeout -= tim.msElapsed();
            myassert(idp.fmt.codec == mDecoder->type());
            ret = mDecoder->decode(idp.buf, idp.size);
            mPrev->confirmRead(idp.size);
        } else {
            ret = mDecoder->decode(nullptr, 0);
        }
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
