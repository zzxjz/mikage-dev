#include "input.hpp"

#include <framework/bit_field_new.hpp>

struct ButtonState {
    uint16_t storage;

    using Fields = v2::BitField::Fields<uint16_t>;

    auto A() const         { return Fields::MakeOn< 0, 1>(this); }
    auto B() const         { return Fields::MakeOn< 1, 1>(this); }
    auto X() const         { return Fields::MakeOn<10, 1>(this); }
    auto Y() const         { return Fields::MakeOn<11, 1>(this); }
    auto L() const         { return Fields::MakeOn< 9, 1>(this); }
    auto R() const         { return Fields::MakeOn< 8, 1>(this); }
    auto Select() const    { return Fields::MakeOn< 2, 1>(this); }
    auto Start() const     { return Fields::MakeOn< 3, 1>(this); }
    auto DigiLeft() const  { return Fields::MakeOn< 5, 1>(this); }
    auto DigiRight() const { return Fields::MakeOn< 4, 1>(this); }
    auto DigiUp() const    { return Fields::MakeOn< 6, 1>(this); }
    auto DigiDown() const  { return Fields::MakeOn< 7, 1>(this); }
};

void InputSource::SetPressedA(bool pressed) {
    buttons = ButtonState { buttons }.A()(pressed).storage;
}

void InputSource::SetPressedB(bool pressed) {
    buttons = ButtonState { buttons }.B()(pressed).storage;
}

void InputSource::SetPressedX(bool pressed) {
    buttons = ButtonState { buttons }.X()(pressed).storage;
}

void InputSource::SetPressedY(bool pressed) {
    buttons = ButtonState { buttons }.Y()(pressed).storage;
}

void InputSource::SetPressedL(bool pressed) {
    buttons = ButtonState { buttons }.L()(pressed).storage;
}

void InputSource::SetPressedR(bool pressed) {
    buttons = ButtonState { buttons }.R()(pressed).storage;
}

void InputSource::SetPressedSelect(bool pressed) {
    buttons = ButtonState { buttons }.Select()(pressed).storage;
}

void InputSource::SetPressedStart(bool pressed) {
    buttons = ButtonState { buttons }.Start()(pressed).storage;
}

void InputSource::SetPressedDigiLeft(bool pressed) {
    buttons = ButtonState { buttons }.DigiLeft()(pressed).storage;
}

void InputSource::SetPressedDigiRight(bool pressed) {
    buttons = ButtonState { buttons }.DigiRight()(pressed).storage;
}

void InputSource::SetPressedDigiUp(bool pressed) {
    buttons = ButtonState { buttons }.DigiUp()(pressed).storage;
}

void InputSource::SetPressedDigiDown(bool pressed) {
    buttons = ButtonState { buttons }.DigiDown()(pressed).storage;
}

void InputSource::SetPressedHome(bool pressed) {
    home_button = pressed;
}

auto InputSource::GetTouchState() -> TouchState {
    std::lock_guard lock(touch_mutex);

    return touch;
}

void InputSource::SetTouch(float x, float y) {
    std::lock_guard lock(touch_mutex);

    touch.x = x;
    touch.y = y;
    touch.pressed = true;
}

void InputSource::EndTouch() {
    std::lock_guard lock(touch_mutex);

    touch.pressed = false;
}

auto InputSource::GetCirclePadState() -> CirclePadState {
    std::lock_guard lock(touch_mutex);

    return circle_pad;
}

void InputSource::SetCirclePad(float x, float y) {
    std::lock_guard lock(touch_mutex);

    circle_pad.x = x;
    circle_pad.y = y;
}
