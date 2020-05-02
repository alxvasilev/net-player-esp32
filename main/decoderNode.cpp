#include "decoderNode.hpp"
#include "decoderMp3.hpp"

bool DecoderNode::createDecoder(esp_codec_type_t type)
{
    switch (type) {
    case ESP_CODEC_TYPE_MP3:
        ESP_LOGI(mTag, "Created MP3 decoder");
        mDecoder = new DecoderMp3;
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
        ESP_LOGD(mTag, "About to get upstream codec type");
        DataPullReq idp(0);
        auto err = mPrev->pullData(idp, timeout);
        if (err) {
            return err;
        }
        ESP_LOGD(mTag, "Obtained upstream codec type: %d", idp.fmt.codec);
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
                    changeDecoder(idp.fmt.codec);
                } else {
                    mDecoder->reset();
                }
                continue;
            } else {
                ESP_LOGI(mTag, "Codec type changed in upstream node, successfully decoded and returning leftover data");
                odp.buf = mDecoder->outputBuf();
                odp.size = ret;
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
