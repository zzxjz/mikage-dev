#pragma once

// TODO: Instead of allowing any interrupt to be reported here, just allow triggering a single one
class InterruptListener {
public:
    virtual void NotifyInterrupt(uint32_t index) = 0;
};
