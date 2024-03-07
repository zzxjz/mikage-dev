#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

/**
 * Interface for the frontend to set emulator-visible input state.
 *
 * Get* member functions are reserved for the emulator core.
 * Set* member functions should be called from one thread, only.
 */
class InputSource {
    struct TouchState {
        float x = 0.f;
        float y = 0.f;
        bool pressed = false;
    };

    struct CirclePadState {
        float x = 0.f;
        float y = 0.f;
    };

    std::atomic<uint16_t> buttons { 0 };

    std::mutex touch_mutex;
    TouchState touch;
    CirclePadState circle_pad;

    std::atomic<bool> home_button;

public:
    uint16_t GetButtonState() const {
        return buttons;
    }

    TouchState GetTouchState();

    CirclePadState GetCirclePadState();

    bool IsHomeButtonPressed() const { return home_button; };

    void SetPressedA(bool);
    void SetPressedB(bool);
    void SetPressedX(bool);
    void SetPressedY(bool);

    void SetPressedL(bool);
    void SetPressedR(bool);

    void SetPressedSelect(bool);
    void SetPressedStart(bool);

    void SetPressedDigiLeft(bool);
    void SetPressedDigiRight(bool);
    void SetPressedDigiUp(bool);
    void SetPressedDigiDown(bool);

    void SetPressedHome(bool);

    /**
     * Puts touch pad into "touched" state at the given coordinates
     *
     * @param x Normalized horizontal coordinate [0..1]
     * @param y Normalized vertical coordinate [0..1]
     */
    void SetTouch(float x, float y);

    /**
     * Puts touch pad into "untouched" state
     */
    void EndTouch();

    void SetCirclePad(float x, float y);
};
