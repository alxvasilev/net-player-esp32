#include "equalizer.hpp"
const std::array<EqBandConfig, 10> EqBandConfig::kPresetTenBand = {{
    {50, 1.0}, {70, 1.0}, {125, 1.0}, {250, 1.0}, {500, 1.0},
    {1000, 1.0}, {2000, 1.0}, {4000, 1.0}, {8000, 1.0}, {16000, 1.0}
}};
