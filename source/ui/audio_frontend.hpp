#pragma once

#include <array>
#include <cstdint>

struct AudioFrontend {
    virtual void OutputSamples(std::array<int16_t, 2> samples) = 0;

};
