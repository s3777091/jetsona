#ifndef BACKLIGHT_H
#define BACKLIGHT_H

/* Minimal backlight. The HDMI panel has its own backlight; brightness control
 * is a no-op on Jetson (could later map to /sys/class/backlight). */
class Backlight {
public:
    virtual ~Backlight() = default;
    virtual void SetBrightness(int /*percent*/) {}
    virtual int GetBrightness() const { return 100; }
};

class PwmBacklight : public Backlight {
public:
    PwmBacklight(int /*gpio*/, bool /*invert*/ = false) {}
};

#endif