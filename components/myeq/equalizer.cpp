#include "equalizer.hpp"
const EqBandConfig EqBandConfig::kPreset10Band[10] = {
    {150, 1200}, {150, 1200}, {340, 707}, {500, 707}, {1000, 707},
    {2000, 707}, {3000, 707}, {4000, 707}, {8000, 707}, {15500, 707}
};

const EqBandConfig EqBandConfig::kPreset9Band[9] = {
    {150, 1200}, {150, 1200}, {340, 707}, {500, 707}, {1000, 707},
    {2000, 707}, {4000, 707}, {8000, 707}, {15500, 707}
};

const EqBandConfig EqBandConfig::kPreset8Band[8] = {
    {150, 1200}, {150, 1200}, {340, 707}, {1000, 707},
    {2000, 707}, {4000, 707}, {8000, 707}, {15500, 707}
};

const EqBandConfig EqBandConfig::kPreset7Band[7] = {
    {150, 1200}, {150, 1200}, {340, 707}, {1500, 600},
    {4000, 707}, {8000, 707}, {15500, 707}
};

const EqBandConfig EqBandConfig::kPreset6Band[6] = {
    {150, 1200}, {150, 1200}, {340, 707}, {3000, 600}, {6000, 707}, {15500, 707}
};

const EqBandConfig EqBandConfig::kPreset5Band[5] = {
    {150, 1200}, {150, 1200}, {300, 707}, {4000, 200}, {15000, 500}
};
const EqBandConfig EqBandConfig::kPreset4Band[4] = {
    {150, 1200}, {150, 1200}, {340, 707}, {10000, 300}
};
const EqBandConfig EqBandConfig::kPreset3Band[3] = {
    {150, 1200}, {500, 500}, {10000, 300}
};
const EqBandConfig* EqBandConfig::kBandPresets[8] = {
    EqBandConfig::kPreset3Band, EqBandConfig::kPreset4Band, EqBandConfig::kPreset5Band,
    EqBandConfig::kPreset6Band, EqBandConfig::kPreset7Band, EqBandConfig::kPreset8Band,
    EqBandConfig::kPreset9Band, EqBandConfig::kPreset10Band
};
