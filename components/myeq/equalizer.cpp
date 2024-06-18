#include "equalizer.hpp"
const EqBandConfig EqBandConfig::kPreset10Band[10] = {
    {31, -15}, {62, 15}, {125, 15}, {250, 15}, {500, 15},
    {1000, 15}, {2000, 15}, {4000, 15}, {8000, 15}, {15500, -15}
};

const EqBandConfig EqBandConfig::kPreset9Band[9] = {
    {31, 3}, {62, 3}, {125, 3}, {250, 3}, {500, 3},
    {1500, 3}, {4000, 3}, {8000, 3}, {15500, -1}
};

const EqBandConfig EqBandConfig::kPreset8Band[8] = {
    {50, -1}, {250, 10}, {500, 10}, {1000, 10}, {2000, 10},
    {4000, 10}, {8000, 10}, {15500, -1}
};

const EqBandConfig EqBandConfig::kPreset7Band[7] = {
    {50, -1}, {250, 10}, {750, 10}, {2000, 10}, {4000, 10}, {8000, 10}, {15500, 3}
};

const EqBandConfig EqBandConfig::kPreset6Band[6] = {
    {50, -1}, {500, 10}, {1000, 10}, {4000, 10}, {8000, 10}, {15500, -1}
};

const EqBandConfig EqBandConfig::kPreset5Band[5] = {
    {55, -1}, {100, 3}, {4000, 40}, {8000, 20}, {15000, 10}
};
const EqBandConfig EqBandConfig::kPreset4Band[4] = {
    {65, 3}, {1000, 20}, {4000, 40}, {15000, 10}
};
const EqBandConfig EqBandConfig::kPreset3Band[3] = {
    {65, 3}, {4000, 30}, {15000, 10}
};
const EqBandConfig* EqBandConfig::kBandPresets[8] = {
    EqBandConfig::kPreset3Band, EqBandConfig::kPreset4Band, EqBandConfig::kPreset5Band,
    EqBandConfig::kPreset6Band, EqBandConfig::kPreset7Band, EqBandConfig::kPreset8Band,
    EqBandConfig::kPreset9Band, EqBandConfig::kPreset10Band
};
