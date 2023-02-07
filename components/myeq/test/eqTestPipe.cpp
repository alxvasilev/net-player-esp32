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
#include <errno.h>

typedef float EqSample;
// gcc -o player ./eqTest.cpp ./main/equalizer.cpp -I ./main -lpulse -lpulse-simple -lmad -lm -g
float bandGains[10] = {10, 0, 0, 0, 0, 0, 0, 0, 8, 10};
Equalizer<10, EqSample> eqLeft(EqBandConfig::kPreset10Band);
Equalizer<10, EqSample> eqRight(EqBandConfig::kPreset10Band);
bool eqEnable = true;
int ctrlPipe = -1;
char pollInput() {
    if (ctrlPipe < 0) {
        return 0;
    }
    char ch;
    auto n = read(ctrlPipe, &ch, 1);
    if (n <= 0) {
        return 0;
    }
    fprintf(stderr, "read char '%c'\n", ch);
    return ch;
}
void pollKeyboard();
int32_t preampVol = 128;

template <typename S>
void process(char* buf, int len)
{
    auto bufEnd = (S*)(buf + len);
    for (auto* sptr = (S*)buf; sptr < bufEnd;) {
        auto sample = *sptr;
        sample = (sample * preampVol) >> 8;
        *sptr++ = eqEnable ? eqLeft.processAndNarrow(sample) : sample;
        sample = *sptr;
        sample = (sample * preampVol) >> 8;
        *sptr++ = eqEnable ? eqRight.processAndNarrow(sample) : sample;
    }
}
void ctrlSetNonblock(bool enable)
{
    if (ctrlPipe < 0) {
        return;
    }
    struct termios old = {0};
    if (tcgetattr(ctrlPipe, &old) < 0) {
        perror("tcsetattr()");
        exit(255);
    }
    if (enable) {
        old.c_lflag &= ~ICANON;
        old.c_lflag &= ~ECHO;
        fprintf(stderr, "old VMIN: %d, old VTIME: %d\n", old.c_cc[VMIN], old.c_cc[VTIME]);
        old.c_cc[VMIN] = 1;
        old.c_cc[VTIME] = 0;
    } else {
        old.c_lflag |= ICANON;
        old.c_lflag |= ECHO;
        old.c_cc[VMIN] = 1;
        old.c_cc[VTIME] = 255;
    }
    if (tcsetattr(ctrlPipe, TCSANOW, &old) < 0) perror("tcsetattr ICANON");
    auto flags = fcntl(ctrlPipe, F_GETFL, 0);
    flags = enable ? (flags | O_NONBLOCK) : (flags &~ O_NONBLOCK);
    fcntl(ctrlPipe, F_SETFL, flags);
}

void terminate(int code)
{
    exit(code);
}
int main(int argc, char **argv)
{
    // Parse command-line arguments
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <bps> <samplerate>\n", argv[0]);
        return 255;
    }
    int bps = atoi(argv[1]);
    if (bps != 16 && bps != 32) {
        fprintf(stderr, "Unsuported bps %d\n", bps);
        return 255;
    }
    int sampleSize = 2 * bps / 8;
    int sampleRate = atoi(argv[2]);
    if (sampleRate < 1000 || sampleRate > 200000) {
        fprintf(stderr, "Unsupported sample rate %d\n", sampleRate);
        return 255;
    }
    if (argc > 3) {
        ctrlPipe = open(argv[3], O_RDONLY|O_NONBLOCK);
        if (ctrlPipe >= 0) {
            fprintf(stderr, "Listening for keyboard on control pipe %s\n", argv[3]);
        } else {
            fprintf(stderr, "Error %s opening control pipe %s\n", strerror(errno), argv[3]);
        }
    }
    eqLeft.init(sampleRate, bandGains);
    eqRight.init(sampleRate, bandGains);

    void(*processFunc)(char* buf, int len) = (bps == 16) ? process<int16_t> : process<int32_t>;
    enum { kReadSize = 1024 };
    char buf[kReadSize];
    int dataLen = 0;
    for(;;) {
        auto len = read(0, buf + dataLen, sizeof(buf) - dataLen);
        if (len <= 0) {
            if (len < 0) {
                perror("Error reading stdin");
            }
            terminate(1);
        }
        dataLen += len;
        int toRead = (dataLen / sampleSize) * sampleSize;
        pollKeyboard();
        processFunc(buf, toRead);
        write(1, buf, toRead);
        dataLen -= toRead;
        if (dataLen) {
            memmove(buf, buf + toRead, dataLen);
        }
    }
    return 0;
}

void setEq(int band, int delta)
{
    auto oldGain = bandGains[band];
    auto newGain = oldGain + delta;
    eqLeft.setBandGain(band, newGain);
    eqRight.setBandGain(band, newGain);
    bandGains[band] = newGain;
    fprintf(stderr, "Set band %d Hz (%d) %f --> %f\n", eqLeft.bandConfig(band).freq, band,
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
        fprintf(stderr, "Equalizer %s\n", eqEnable ? "ENABLED":"DISABLED");
        break;
    case 'r': {
        eqLeft.setAllGains(bandGains);
        eqRight.setAllGains(bandGains);
        fprintf(stderr, "Reset all bands to defaults\n");
        break;
    }
    case 'R': {
        eqLeft.zeroAllGains();
        eqRight.zeroAllGains();
        memset(bandGains, 0, sizeof(bandGains));
        fprintf(stderr, "Reset all bands to zero gain\n");
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
