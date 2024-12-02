#include "equalizer.hpp"
const EqBandConfig EqBandConfig::kPreset10Band[10] = {
    {31, 1000}, {62, 1000}, {125, 1000}, {250, 1000}, {500, 1000},
    {1000, 1000}, {2000, 1000}, {4000, 1000}, {8000, 1000}, {15500, 1000}
};

const EqBandConfig EqBandConfig::kPreset9Band[9] = {
    {31, 1000}, {62, 707}, {125, 707}, {250, 707}, {500, 707},
    {1500, 1500}, {4000, 707}, {8000, 707}, {15500, 707}
};

const EqBandConfig EqBandConfig::kPreset8Band[8] = {
    {45, 1000}, {250, 350}, {500, 707}, {350, 707}, {2000, 707},
    {4000, 707}, {8000, 707}, {15500, 707}
};

const EqBandConfig EqBandConfig::kPreset7Band[7] = {
    {50, 1000}, {200, 350}, {750, 350}, {1600, 350}, {4000, 707}, {8000, 707}, {15500, 707}
};

const EqBandConfig EqBandConfig::kPreset6Band[6] = {
    {50, 1000}, {200, 350}, {1000, 300}, {3000, 350}, {8000, 707}, {15500, 707}
};

const EqBandConfig EqBandConfig::kPreset5Band[5] = {
    {55, 1000}, {200, 350}, {3000, 300}, {7000, 300}, {15000, 10}
};
const EqBandConfig EqBandConfig::kPreset4Band[4] = {
    {55, 1000}, {1000, 200}, {4000, 200}, {15000, 707}
};
const EqBandConfig EqBandConfig::kPreset3Band[3] = {
    {55, 1000}, {4000, 100}, {15000, 707}
};
const EqBandConfig* EqBandConfig::kBandPresets[8] = {
    EqBandConfig::kPreset3Band, EqBandConfig::kPreset4Band, EqBandConfig::kPreset5Band,
    EqBandConfig::kPreset6Band, EqBandConfig::kPreset7Band, EqBandConfig::kPreset8Band,
    EqBandConfig::kPreset9Band, EqBandConfig::kPreset10Band
};
