#ifndef SPEEDPROBE_HPP
#define SPEEDPROBE_HPP

class LinkSpeedProbe {
    ElapsedTimer mTimer;
    uint32_t mBytes = 0;
    uint32_t mAvgSpeed = 0;
public:
    uint32_t average() const { return mAvgSpeed; }
    void onTraffic(uint32_t nBytes) { mBytes += nBytes; }
    void reset() { mBytes = 0; mAvgSpeed = 0; mTimer.reset(); }
    uint32_t poll() {
        int64_t elapsed = mTimer.usElapsed();
        mTimer.reset();
        if (elapsed == 0) {
            elapsed = 1;
        }
        uint32_t speed = ((int64_t)mBytes * 1000000 + (elapsed >> 1)) / elapsed; //rounded int division
        mBytes = 0;
        mAvgSpeed = (mAvgSpeed * 3 + speed + 2) >> 2; // rounded int division by 4
        return speed;
    }
};

#endif // SPEEDPROBE_HPP
