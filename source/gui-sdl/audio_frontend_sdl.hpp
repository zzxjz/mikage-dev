#pragma once

#include <ui/audio_frontend.hpp>

class SDLAudioFrontend : public AudioFrontend {
public:
    void OutputSamples(std::array<int16_t, 2> samples) override {}
    SDLAudioFrontend() = default;
};
