#ifndef VORBIS_DEC_HPP
#define VORBIS_DEC_HPP
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ivorbiscodec.h>
#include "codec_internal.h"
#include <assert.h>
#ifndef vorb_err
    #define vorb_err(fmt,...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#endif
#ifdef VORB_DEBUG
  #ifndef vorb_dbg
    #define vorb_dbg vorb_err
  #endif
#else
    #define vorb_dbg(...)
#endif

class VorbisDecoder {
protected:
    enum {kBufSize = 4096};
    ogg_sync_state   mOy; /* sync and verify incoming physical bitstream */
    ogg_stream_state mOs; /* take physical pages, weld into a logical stream of packets */
    ogg_page         mOg; /* one Ogg bitstream page. Vorbis packets are inside */
    ogg_packet       mOp; /* one raw packet of data for decode */
    vorbis_info      mVi; /* struct that stores all the static vorbis bitstream settings */
    vorbis_comment   mVc; /* struct that stores all the bitstream user comments */
    vorbis_dsp_state mVd; /* central working state for the packet->PCM decoder */
    vorbis_block     mVb; /* local working space for packet->PCM decode */
    bool mNeedMoreData = false;
    bool mReachedEos = false;
    int setNeedMoreData() { mNeedMoreData = true; return 0; }
    template<typename T, int Bps>
    T convertSample(ogg_int32_t);
public:
    char* writePtr(int len) {
        return ogg_sync_buffer(&mOy, len);
    }
    void notifyWrote(int len)
    {
        assert(len <= mOy.storage - mOy.fill);
        ogg_sync_wrote(&mOy, len);
        mNeedMoreData = false;
    }
    int pendingBytes() const { return mOy.fill - mOy.returned; }
    bool eos() const { return ogg_page_eos(&mOg); }
    int streamSerialNo() const { return ogg_page_serialno(&mOg); }
    int64_t lastSamplePos() const { return ogg_page_granulepos(&mOg); }
    VorbisDecoder()
    : mOy{}, mOs{}, mOg{}, mOp{}, mVi{}, mVc{}, mVd{}, mVb{} // zero-initialize all
    {
        ogg_sync_init(&mOy); /* Now we can read pages */
    }
    ~VorbisDecoder()
    {
        reset();
    }
    const vorbis_info& streamInfo() const { return mVi; }
    const vorbis_comment& streamComments() const { return mVc; }
    const int64_t& granulePos() const { return mOs.granulepos; }
    int init()
    {
        enum { kNumSyncRetries = 10 };
        mReachedEos = false;
        /* We want the first page (which is guaranteed to be small and only contain the Vorbis
        stream initial header) We need the first page to get the stream serialno. */
        int ret;
        int i = 0;
        // sync: read pages till beginning-of-stream page found
        for (; i < kNumSyncRetries; i++) { // finite attempts to sync
            ret = ogg_sync_pageout(&mOy, &mOg);
            if (ret == 0) {
                return setNeedMoreData();
            }
            else if (ret == 1) {
                if (ogg_page_bos(&mOg)) { // found beginning-of-stream page, bail out
                    break;
                }
                vorb_dbg("init: Skipping non-BOS page, with EOS=%d", ogg_page_eos(&mOg));
            }
            if (ret == -1) {
                vorb_dbg("init: No sync, retrying");
            }
        }
        if (i == kNumSyncRetries) {
            vorb_err("init: Invalid stream or couldn't find a BOS page");
            return -1;
        }
        if (ret == 0) {
            return setNeedMoreData();
        }
        /* Get the serial number and set up the rest of decode. */
        /* serialno first; use it to set up a logical stream */
        ret = ogg_stream_init(&mOs, ogg_page_serialno(&mOg));
        if (ret) {
            vorb_err("ogg_stream_init() failed");
            return -9;
        }
        /* extract the initial header from the first page and verify that the
        Ogg bitstream is in fact Vorbis data */
        /* I handle the initial header first instead of just having the code
        read all three Vorbis headers at once because reading the initial
        header is an easy way to identify a Vorbis bitstream and it's
        useful to see that functionality seperated out. */

        vorbis_info_init(&mVi);
        vorbis_comment_init(&mVc);
        if ((ret = ogg_stream_pagein(&mOs, &mOg)) < 0) {
            /* error; stream version mismatch perhaps */
            vorb_err("init: ogg_stream_pagein() returned error %d", ret);
            return -2;
        }
        if ((ret = ogg_stream_packetout(&mOs, &mOp)) != 1) {
            /* no page? must not be vorbis */
            vorb_err("init: ogg_stream_packetout() returned error %d", ret);
            return -3;
        }
        if(vorbis_synthesis_headerin(&mVi, &mVc, &mOp) < 0) {
            /* error case; not a vorbis header */
            vorb_err("init: vorbis_synthesis_headerin() returned error - no audio data in stream");
            return -4;
        }
        /* At this point, we're sure we're Vorbis. We've set up the logical
        (Ogg) bitstream decoder. Get the comment and codebook headers and
        set up the Vorbis decoder */

        /* The next two packets in order are the comment and codebook headers.
        They're likely large and may span multiple pages. Thus we read
        and submit data until we get our two packets, watching that no
        pages are missing. If a page is missing, error out; losing a
        header page is the only place where missing data is fatal. */
        i = 0;
        while(i < 2) {
            int result = ogg_sync_pageout(&mOy, &mOg);
            if(result == 0) {
                return setNeedMoreData(); // Need more data
            }
            else if (result < 0) { // not synced yet
                // Don't complain about missing or corrupt data yet. We'll catch it at the packet output phase
                continue;
            }
            // result == 1
            ogg_stream_pagein(&mOs, &mOg); // we can ignore any errors here as they'll also become apparent at packetout
            while(i < 2) {
                result = ogg_stream_packetout(&mOs, &mOp);
                if(result == 0) {
                    return setNeedMoreData();
                }
                else if(result < 0) {
                    /* Uh oh; data at some point was corrupted or missing!
                    We can't tolerate that in a header.  Die. */
                    vorb_err("init: Can't read packet for synthesis header");
                    return -5;
                }
                result = vorbis_synthesis_headerin(&mVi, &mVc, &mOp);
                if(result < 0) {
                    vorb_err("init: vorbis_synthesis_headerin() failed");
                    return -6;
                }
                i++;
            }
        }
        /* OK, got and parsed all three headers. Initialize the Vorbis packet->PCM decoder. */
        if (vorbis_synthesis_init(&mVd, &mVi) != 0) { /* central decode state */
            vorb_err("init: vorbis_synthesis_init() failed");
            return -8;
        }
        vorbis_block_init(&mVd, &mVb);  /* local state for most of the decode so multiple block decodes can
                                        proceed in parallel. We could init multiple vorbis_block structures for vd here */
        return 1;
    }
    void write(const char* data, int len)
    {
        auto buf = ogg_sync_buffer(&mOy, len);
        memcpy(buf, data, len);
        notifyWrote(len);
    }
    int numOutputSamples() { return vorbis_synthesis_pcmout(&mVd, nullptr) * mVi.channels; }
    int decode()
    {
        for (;;) {
          //consume packets already in buffer, if any
          for (;;) {
            int result = ogg_stream_packetout(&mOs, &mOp);
            if (result == 0) {
                break;
            }
            else if(result < 0) { /* missing or corrupt data at this page position */
                vorb_err("ogg_stream_packetout: error %d, continuing...", result);
                continue;
            }
            result = vorbis_synthesis(&mVb, &mOp);
            if (result) {
                vorb_err("vorbis_synthesis: error %d", result);
                return result;
            }
            result = vorbis_synthesis_blockin(&mVd, &mVb);
            if (result) {
                vorb_err("vorbis_synthesis_blockin: error %d", result);
            }
            return result ? result : 1;
          }
          // feed a new page into buffer
          for(;;) {
              if (mReachedEos) {
                  return OV_EOF;
              }
              // get more data
              int result = ogg_sync_pageout(&mOy, &mOg);
              if (result == 0) {
                  return setNeedMoreData(); /* need more data */
              }
              else if(result < 0) { /* missing or corrupt data at this page position */
                  vorb_err("ogg_sync_pageout: error %d, continuing...", result);
                  continue;
              }
              mReachedEos = ogg_page_eos(&mOg);
              if (mReachedEos) {
                  vorb_dbg("ogg_sync_pageout: EOS page detected");
              }
              result = ogg_stream_pagein(&mOs, &mOg);
              if (result) {
                  vorb_err("ogg_stream_pagein: error %d", result);
              }
          }
       }
    }
    template <typename T, int Bps=16>
    int getSamples(T* samples, int num)
    {
        ogg_int32_t** pcm = nullptr;
        int nAvail = vorbis_synthesis_pcmout(&mVd, &pcm);
        if (nAvail <= 0) {
            return 0;
        }
        int nSamples;
        int nChans = mVi.channels;
        if (nChans == 2) {
            if (num & 1) {
                vorb_err("getSamples: Stereo output, but sample capacity of output buffer is not even");
                return 0;
            }
            nSamples = std::min(num >> 1, nAvail);
            auto wptr = samples;
            auto left = pcm[0];
            auto right = pcm[1];
            for (int i = 0; i < nSamples; i++) {
                (*wptr++) = convertSample<T, Bps>(left[i]);
                (*wptr++) = convertSample<T, Bps>(right[i]);
            }
            vorbis_synthesis_read(&mVd, nSamples); /* tell libvorbis how many samples we actually consumed */
            return nSamples << 1;
        }
        else if (nChans == 1) {
            nSamples = std::min(num, nAvail);
            auto rptr = pcm[0];
            auto wend = samples + nSamples;
            for (auto wptr = samples; wptr < wend;) {
                *(wptr++) = convertSample<T, Bps>(*(rptr++));
            }
            vorbis_synthesis_read(&mVd, nSamples); /* tell libvorbis how many samples we actually consumed */
            return nSamples;
        }
        else {
            vorb_err("getSamples: unsupported number of channels %d", nChans);
            return 0;
        }
    }
    void reset(bool clearSync = true)
    {
        vorbis_block_clear(&mVb);
        vorbis_dsp_clear(&mVd);
        /* ogg_page and ogg_packet structs always point to storage in
         libvorbis.  They're never freed or manipulated directly */
        ogg_stream_clear(&mOs);
        vorbis_comment_clear(&mVc);
        vorbis_info_clear(&mVi);  /* must be called last */
        if (clearSync) {
            ogg_sync_clear(&mOy);
        }
    }
};
template <>
inline int16_t VorbisDecoder::convertSample<int16_t, 16>(ogg_int32_t x)
{
    x >>= 10;
    if (x > 32767) {
        return 32767;
    }
    else if (x < -32768) {
        return 32768;
    }
    return x;
}
template <>
inline int32_t VorbisDecoder::convertSample<int32_t, 24>(ogg_int32_t x)
{
    return (x >> 1);
}

#endif
