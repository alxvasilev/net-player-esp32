#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <mad.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#define BQ_DEBUG
#include <equalizer.hpp>
#include <limits>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

// gcc -o player ./eqTest.cpp ./main/equalizer.cpp -I ./main -lpulse -lpulse-simple -lmad -lm -g
float gains[10] = {14, 0, 0, 12, 14};



pa_simple *device = NULL;
int ret = 1;
int error;
FILE* out = nullptr;

struct mad_stream mad_stream;
struct mad_frame mad_frame;
struct mad_synth mad_synth;

void output(struct mad_header const *header, struct mad_pcm *pcm);
Equalizer<5, int32_t> eqLeft(EqBandConfig::kPresetFiveBand);
Equalizer<5, int32_t> eqRight(EqBandConfig::kPresetFiveBand);
bool eqEnable = true;
char pollInput() {
    char ch;
    auto n = read(0, &ch, 1);
    return (n == 0) ? 0 : ch;
}
void pollKeyboard();

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
    eqLeft.init(44100, gains);
    eqRight.init(44100, gains);

    struct termios old = {0};
    if (tcgetattr(0, &old) < 0)
            perror("tcsetattr()");
    old.c_lflag &= ~ICANON;
    old.c_lflag &= ~ECHO;
    old.c_cc[VMIN] = 1;
    old.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &old) < 0) perror("tcsetattr ICANON");
    fcntl(0, F_SETFL, O_NONBLOCK);

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
            //printf("frame size: %ld\n", mad_stream.next_frame - mad_stream.buffer);
            // Synthesize PCM data of frame
            mad_synth_frame(&mad_synth, &mad_frame);
            output(&mad_frame.header, &mad_synth.pcm);
            remaining = mad_stream.bufend - mad_stream.next_frame;
        }
        pollKeyboard();
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

int16_t scale(mad_fixed_t sample) {
    return sample >> (29 - 15);
}
int32_t preampVol = 128;
void output(struct mad_header const *header, struct mad_pcm *pcm) {
//    int nsamples = pcm->length;
//    printf("bitrate: %lu, samplerate: %d, nsamples: %d\n",
//           header->bitrate, header->samplerate, nsamples);
    mad_fixed_t const *left_ch = pcm->samples[0], *right_ch = pcm->samples[1];
    static int16_t stream[1152*2];
    if (pcm->channels != 2) {
        printf("Mono not supported\n");
        return;
    }
    auto bufEnd = stream + sizeof(stream) / sizeof(int16_t);
    for (int16_t* wptr = stream; wptr < bufEnd;) {
        int16_t sample = (preampVol * scale(*left_ch++)) >> 8;
        *wptr++ = eqEnable ? eqLeft.processAndNarrow(sample) : sample;

        sample = (preampVol * scale(*right_ch++)) >> 8;
        *wptr++ = eqEnable ? eqRight.processAndNarrow(sample) : sample;
    }
    if (pa_simple_write(device, stream, (size_t)1152*4, &error) < 0) {
        fprintf(stderr, "pa_simple_write() failed: %s\n", pa_strerror(error));
        return;
    }
    fwrite(stream, 1, 1152*4, out);
}

void setEq(int band, int delta)
{
    auto oldGain = eqLeft.bandGain(band);
    auto newGain = oldGain + delta;
    eqLeft.setBandGain(band, newGain);
    eqRight.setBandGain(band, newGain);
    printf("Set band %d Hz (%d) %f --> %f\n", eqLeft.bandConfig(band).freq, band,
        oldGain, newGain);
}
void pollKeyboard()
{
    char ch = pollInput();
    if (!ch) {
        return;
    }
    int step = 2;
    switch(ch) {
    case ' ':
        eqEnable = !eqEnable;
        printf("Equalizer %s\n", eqEnable ? "ENABLED":"DISABLED");
        break;
    case 'r': {
        for (int i = 0; i < 10; i++) {
            eqLeft.setBandGain(i, gains[i]);
            eqRight.setBandGain(i, gains[i]);
        }
        printf("Reset all bands to defaults\n");
        break;
    }
    case 'R': {
        eqLeft.zeroAllGains();
        eqRight.zeroAllGains();
        printf("Reset all bands to zero gain\n");
        break;
    }
    case 'a': setEq(0, step); break;
    case 'z': setEq(0, -step); break;
    case 's': setEq(1, step); break;
    case 'x': setEq(1, -step); break;
    case 'd': setEq(2, step); break;
    case 'c': setEq(2, -step); break;
    case 'f': setEq(3, step); break;
    case 'v': setEq(3, -step); break;
    case 'g': setEq(4, step); break;
    case 'b': setEq(4, -step); break;
/*
    case 'h': setEq(5, step); break;
    case 'n': setEq(5, -step); break;
    case 'j': setEq(6, step); break;
    case 'm': setEq(6, -step); break;
    case 'k': setEq(7, step); break;
    case ',': setEq(7, -step); break;
    case 'l': setEq(8, step); break;
    case '.': setEq(8, -step); break;
    case ';': setEq(9, step); break;
    case '/': setEq(9, -step); break;
*/
    default: break;
    }
}
