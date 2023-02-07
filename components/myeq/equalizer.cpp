#include "equalizer.hpp"
const std::array<EqBandConfig, 10> EqBandConfig::kPreset10Band = {{
    {50, .33}, {70, 0.33}, {125, .33}, {250, .33}, {500, .33},
    {1000, .33}, {2000, .33}, {4000, .33}, {8000, .33}, {16000, .33}
}};

const std::array<EqBandConfig, 8> EqBandConfig::kPreset8Band = {{
    {50, 1.0}, {250, 1.0}, {500, 1.0}, {1000, 1.0}, {2000, 1.0},
    {4000, 1.0}, {8000, 1.0}, {16000, 1.0}
}};

const std::array<EqBandConfig, 6> EqBandConfig::kPreset6Band = {{
    {50, 1.0}, {100, 0.33}, {1000, 1.0}, {3000, 1.0}, {8000, 1.0}, {15000, 1.0}
}};

const std::array<EqBandConfig, 5> EqBandConfig::kPreset5Band = {{
    {50, 1.0}, {100, 2.0}, {4000, 4.0}, {8000, 2.0}, {16000, 1.0}
}};
