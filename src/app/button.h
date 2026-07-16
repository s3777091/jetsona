#ifndef BUTTON_H
#define BUTTON_H

#include <functional>

/* Minimal button stub. Phase 1 has no physical button wired; the UI is driven
 * by touch. OnClick callbacks are stored but never triggered here. A later
 * phase can wire this to a GPIO header pin or a USB keyboard key via evdev. */
class Button {
public:
    Button(int /*gpio*/) {}
    void OnClick(std::function<void()> /*cb*/) {}
};

#endif