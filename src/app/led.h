#ifndef LED_H
#define LED_H

class Led {
public:
    virtual ~Led() = default;
    virtual void On() {}
    virtual void Off() {}
    virtual void BlinkOnce() {}
};

class NoLed : public Led {};
class SingleLed : public Led {
public:
    SingleLed(int /*gpio*/) {}
};
#endif