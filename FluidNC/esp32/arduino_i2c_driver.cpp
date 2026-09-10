// Copyright 2025 - Mitch Bradley
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "Platform.h"
#if USE_ARDUINO_I2C_DRIVER
#    include <Wire.h>
#    include "Driver/fluidnc_i2c.h"
#    include "Logging.h"

// TwoWire buffers a whole transaction, and TwoWire::write() silently returns 0
// once txLength reaches bufferSize, which defaults to I2C_BUFFER_LENGTH (128).
// SSD1306_I2C::display() pushes an entire frame in one write --
// displayBufferSize + 1 = 1025 bytes for a 128x64 panel -- and its partial
// update path writes up to width + 1 = 129. Both exceed the default, so every
// screen update would be silently truncated. The legacy driver in
// esp32/i2c.cpp has no such limit, which is why this only bites builds that
// take the Wire path.
static constexpr size_t I2C_TX_BUFFER_SIZE = 1088;

bool i2c_master_init(objnum_t bus_number, pinnum_t sda_pin, pinnum_t scl_pin, uint32_t frequency) {
    TwoWire& i2c = bus_number ? Wire1 : Wire;

    if (i2c.setBufferSize(I2C_TX_BUFFER_SIZE) != I2C_TX_BUFFER_SIZE) {
        log_error("I2C" << int(bus_number) << " buffer allocation failed");
        return true;
    }

    // begin() returns false if the bus is already up or the pins cannot be
    // attached. Dropping that result leaves I2CBus::_error clear, so every
    // later transfer fails with a generic code indistinguishable from a device
    // NACK -- i.e. a driver fault and a wiring fault look exactly alike.
    if (!i2c.begin(int(sda_pin), int(scl_pin), frequency)) {
        log_error("I2C" << int(bus_number) << " begin failed on sda:" << int(sda_pin) << " scl:" << int(scl_pin));
        return true;
    }
    return false;
};
int i2c_write(objnum_t bus_number, uint8_t address, const uint8_t* data, size_t count) {
    TwoWire& i2c = bus_number ? Wire1 : Wire;

    i2c.beginTransmission(address);
    for (size_t i = 0; i < count; ++i) {
        i2c.write(data[i]);
    }
    auto res = i2c.endTransmission();  // i2c_err_t, see header file
    return res ? -res : count;
}
int i2c_read(objnum_t bus_number, uint8_t address, uint8_t* data, size_t count) {
    TwoWire& i2c    = bus_number ? Wire1 : Wire;
    size_t   actual = i2c.requestFrom((int)address, count);

    for (size_t i = 0; i < actual; ++i) {
        data[i] = i2c.read();
    }
    return actual;
}
#endif
