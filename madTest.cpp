#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <mad.h>
#include <pulse/simple.h>
#include <pulse/error.h>

// gcc -o player ./madTest.cpp -lpulse -lpulse-simple -lmad -g

pa_simple *device = NULL;
int ret = 1;
int error;
FILE* out = nullptr;

struct mad_stream mad_stream;
struct mad_frame mad_frame;
struct mad_synth mad_synth;

void output(struct mad_header const *header, struct mad_pcm *pcm);

int main(int argc, char **argv) {
    // Parse command-line arguments
    if (argc != 2) {
        fprintf(stderr, "Usage: %s [filename.mp3]", argv[0]);
        return 255;
    }

    // Set up PulseAudio 16-bit 44.1kHz stereo output
    static const pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .rate = 44100, .channels = 2 };
    if (!(device = pa_simple_new(NULL, "MP3 player", PA_STREAM_PLAYBACK, NULL, "playback", &ss, NULL, NULL, &error))) {
        printf("pa_simple_new() failed\n");
        return 255;
    }

    // Initialize MAD library
    mad_stream_init(&mad_stream);
    mad_synth_init(&mad_synth);
    mad_frame_init(&mad_frame);
    printf("sizeof(mad_stream) = %zu\n", sizeof(mad_stream.main_data));
    // Filename pointer
    char *filename = argv[1];

    // File pointer
    FILE *fp = fopen(filename, "r");
    int fd = fileno(fp);
    out = fopen("out.raw", "w");

    // Fetch file size, etc
    struct stat metadata;
    if (fstat(fd, &metadata) >= 0) {
        printf("File size %d bytes\n", (int)metadata.st_size);
    } else {
        printf("Failed to stat %s\n", filename);
        fclose(fp);
        return 254;
    }

    enum { kReadSize = 3000 };
    // Decode frame and synthesize loop
    int remaining = 0;
    uint8_t buf[kReadSize];
    for(;;) {
        if (remaining) {
            memmove(buf, mad_stream.next_frame, remaining);
        }
        auto len = fread(buf + remaining, 1, kReadSize-remaining, fp);
        if (len <= 0) {
            return 0;
        }
        mad_stream_buffer(&mad_stream, buf, len + remaining);

        for (;;) {
            auto ret = mad_frame_decode(&mad_frame, &mad_stream);
            if (ret) {
                if (MAD_RECOVERABLE(mad_stream.error)) {
                    continue;
                } else if (mad_stream.error == MAD_ERROR_BUFLEN) {
                    break;
                } else {
                    printf("mad_frame_decode returned error %d\n", mad_stream.error);
                    return 1;
                }
            }
            printf("frame size: %ld\n", mad_stream.next_frame - mad_stream.buffer);
            // Synthesize PCM data of frame
            mad_synth_frame(&mad_synth, &mad_frame);
            output(&mad_frame.header, &mad_synth.pcm);
            remaining = mad_stream.bufend - mad_stream.next_frame;
        }
//        needed = 512 - (mad_stream.next_frame - mad_stream.buffer);
    }
    // Close
    fclose(fp);
    fclose(out);
    // Free MAD structs
    mad_synth_finish(&mad_synth);
    mad_frame_finish(&mad_frame);
    mad_stream_finish(&mad_stream);

    // Close PulseAudio output
    if (device)
        pa_simple_free(device);

    return EXIT_SUCCESS;
}

// Some helper functions, to be cleaned up in the future
uint16_t scale(mad_fixed_t sample) {
    if (sample > 0xfffffff) {
        printf("high: %d\n", sample >> 28);
        double whole = (float)sample / 0xfffffff;
        printf("whole: %f\n", whole);
        printf("hex: 0x%x\n", sample);
    } else if (sample < -0xfffffff) {
        printf("low: hex=0x%x\n", sample);
    }
    auto isNeg = sample & 0x80000000;
    sample >>= (29 - 15);
    return (isNeg) ? (sample | 0x8000) : (sample & 0x7fff);
}
/*
     // round
     sample += (1L << (MAD_F_FRACBITS - 16));
     // clip
     if (sample >= MAD_F_ONE)
         sample = MAD_F_ONE - 1;
     else if (sample < -MAD_F_ONE)
         sample = -MAD_F_ONE;
     // quantize
     return sample >> (MAD_F_FRACBITS + 1 - 16);
*/

void output(struct mad_header const *header, struct mad_pcm *pcm) {
    register int nsamples = pcm->length;
//    printf("bitrate: %lu, samplerate: %d, nsamples: %d\n",
//           header->bitrate, header->samplerate, nsamples);
    mad_fixed_t const *left_ch = pcm->samples[0], *right_ch = pcm->samples[1];
    static char stream[1152*4];
    if (pcm->channels != 2) {
        printf("Mono not supported\n");
        return;
    }
    auto bufEnd = stream + sizeof(stream);
    for (char* sbytes = stream; sbytes < bufEnd;) {
        signed int sample = scale(*left_ch++);
        *(sbytes++) = ((sample >> 0) & 0xff);
        *(sbytes++) = ((sample >> 8) & 0xff);
        sample = scale(*right_ch++);
        *(sbytes++) = ((sample >> 0) & 0xff);
        *(sbytes++) = ((sample >> 8) & 0xff);
    }
    if (pa_simple_write(device, stream, (size_t)1152*4, &error) < 0) {
        fprintf(stderr, "pa_simple_write() failed: %s\n", pa_strerror(error));
        return;
    }
    fwrite(stream, 1, 1152*4, out);
}
