
#include <AP_HAL/AP_HAL.h>
#include "GPIO.h"

#if CONFIG_HAL_BOARD == HAL_BOARD_LINUX

#include "RCOutput_PCA9685.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

#define PCA9685_RA_MODE1           0x00
#define PCA9685_RA_MODE2           0x01
#define PCA9685_RA_LED0_ON_L       0x06
#define PCA9685_RA_LED0_ON_H       0x07
#define PCA9685_RA_LED0_OFF_L      0x08
#define PCA9685_RA_LED0_OFF_H      0x09
#define PCA9685_RA_ALL_LED_ON_L    0xFA
#define PCA9685_RA_ALL_LED_ON_H    0xFB
#define PCA9685_RA_ALL_LED_OFF_L   0xFC
#define PCA9685_RA_ALL_LED_OFF_H   0xFD
#define PCA9685_RA_PRE_SCALE       0xFE

#define PCA9685_MODE1_RESTART_BIT  (1 << 7)
#define PCA9685_MODE1_EXTCLK_BIT   (1 << 6)
#define PCA9685_MODE1_AI_BIT       (1 << 5)
#define PCA9685_MODE1_SLEEP_BIT    (1 << 4)
#define PCA9685_MODE1_SUB1_BIT     (1 << 3)
#define PCA9685_MODE1_SUB2_BIT     (1 << 2)
#define PCA9685_MODE1_SUB3_BIT     (1 << 1)
#define PCA9685_MODE1_ALLCALL_BIT  (1 << 0)
#define PCA9685_ALL_LED_OFF_H_SHUT (1 << 4)
#define PCA9685_MODE2_INVRT_BIT    (1 << 4)
#define PCA9685_MODE2_OCH_BIT      (1 << 3)
#define PCA9685_MODE2_OUTDRV_BIT   (1 << 2)
#define PCA9685_MODE2_OUTNE1_BIT   (1 << 1)
#define PCA9685_MODE2_OUTNE0_BIT   (1 << 0)

/*
 * Drift for internal oscillator
 * see: https://github.com/diydrones/ardupilot/commit/50459bdca0b5a1adf95
 * and https://github.com/adafruit/Adafruit-PWM-Servo-Driver-Library/issues/11
 */
#define PCA9685_INTERNAL_CLOCK (1.04f * 25000000.f)
#define PCA9685_EXTERNAL_CLOCK 24576000.f

using namespace Linux;

#define PWM_CHAN_COUNT 16

static const AP_HAL::HAL& hal = AP_HAL_BOARD_DRIVER;

LinuxRCOutput_PCA9685::LinuxRCOutput_PCA9685(uint8_t addr,
                                             bool external_clock,
                                             uint8_t channel_offset,
                                             int16_t oe_pin_number) :
    _i2c_sem(NULL),
    _enable_pin(NULL),
    _frequency(50),
    _pulses_buffer(new uint16_t[PWM_CHAN_COUNT - channel_offset]),
    _addr(addr),
    _external_clock(external_clock),
    _channel_offset(channel_offset),
    _oe_pin_number(oe_pin_number)
{
    if (_external_clock)
        _osc_clock = PCA9685_EXTERNAL_CLOCK;
    else
        _osc_clock = PCA9685_INTERNAL_CLOCK;
}

LinuxRCOutput_PCA9685::~LinuxRCOutput_PCA9685()
{
    delete [] _pulses_buffer;
}

void LinuxRCOutput_PCA9685::init(void* machtnicht)
{
    _i2c_sem = hal.i2c->get_semaphore();
    if (_i2c_sem == NULL) {
        hal.scheduler->panic(PSTR("PANIC: RCOutput_PCA9685 did not get "
                                  "valid I2C semaphore!"));
        return; /* never reached */
    }

    reset_all_channels();

    /* Set the initial frequency */
    set_freq(0, 50);

    /* Enable PCA9685 PWM */
    if (_oe_pin_number != -1) {
        _enable_pin = hal.gpio->channel(_oe_pin_number);
        _enable_pin->mode(HAL_GPIO_OUTPUT);
        _enable_pin->write(0);
    }
}

void LinuxRCOutput_PCA9685::reset_all_channels()
{
    if (!_i2c_sem->take(10)) {
        return;
    }

    uint8_t data[4] = {0x00, 0x00, 0x00, 0x00};
    hal.i2c->writeRegisters(_addr, PCA9685_RA_ALL_LED_ON_L, 4, data);

    /* Wait for the last pulse to end */
    hal.scheduler->delay(2);

    _i2c_sem->give();
}

void LinuxRCOutput_PCA9685::set_freq(uint32_t chmask, uint16_t freq_hz)
{

    /* Correctly finish last pulses */
    for (int i = 0; i < (PWM_CHAN_COUNT - _channel_offset); i++) {
        write(i, _pulses_buffer[i]);
    }

    if (!_i2c_sem->take(10)) {
        return;
    }

    /* Shutdown before sleeping.
     * see p.14 of PCA9685 product datasheet
     */
    hal.i2c->writeRegister(_addr, PCA9685_RA_ALL_LED_OFF_H, PCA9685_ALL_LED_OFF_H_SHUT);

    /* Put PCA9685 to sleep (required to write prescaler) */
    hal.i2c->writeRegister(_addr, PCA9685_RA_MODE1, PCA9685_MODE1_SLEEP_BIT);

    /* Calculate prescale and save frequency using this value: it may be
     * different from @freq_hz due to rounding/ceiling. We use ceil() rather
     * than round() so the resulting frequency is never greater than @freq_hz
     */
    uint8_t prescale = ceil(_osc_clock / (4096 * freq_hz)) - 1;
    _frequency = _osc_clock / (4096 * (prescale + 1));

    /* Write prescale value to match frequency */
    hal.i2c->writeRegister(_addr, PCA9685_RA_PRE_SCALE, prescale);

    if (_external_clock) {
        /* Enable external clocking */
        hal.i2c->writeRegister(_addr, PCA9685_RA_MODE1,
                               PCA9685_MODE1_SLEEP_BIT | PCA9685_MODE1_EXTCLK_BIT);
    }

    /* Restart the device to apply new settings and enable auto-incremented write */
    hal.i2c->writeRegister(_addr, PCA9685_RA_MODE1,
                            PCA9685_MODE1_RESTART_BIT | PCA9685_MODE1_AI_BIT);

    _i2c_sem->give();
}

uint16_t LinuxRCOutput_PCA9685::get_freq(uint8_t ch)
{
    return _frequency;
}

void LinuxRCOutput_PCA9685::enable_ch(uint8_t ch)
{

}

void LinuxRCOutput_PCA9685::disable_ch(uint8_t ch)
{
    write(ch, 0);
}

void LinuxRCOutput_PCA9685::write(uint8_t ch, uint16_t period_us)
{
    if (ch >= (PWM_CHAN_COUNT - _channel_offset)) {
        return;
    }

    if (!_i2c_sem->take_nonblocking()) {
        return;
    }

    uint16_t length;

    if (period_us == 0)
        length = 0;
    else
        length = round((period_us * 4096) / (1000000.f / _frequency)) - 1;

    uint8_t data[2] = {length & 0xFF, length >> 8};
    uint8_t status = hal.i2c->writeRegisters(_addr,
                                             PCA9685_RA_LED0_OFF_L + 4 * (ch + _channel_offset),
                                             2,
                                             data);

    _pulses_buffer[ch] = period_us;

    _i2c_sem->give();
}

uint16_t LinuxRCOutput_PCA9685::read(uint8_t ch)
{
    return _pulses_buffer[ch];
}

void LinuxRCOutput_PCA9685::read(uint16_t* period_us, uint8_t len)
{
    for (int i = 0; i < len; i++)
        period_us[i] = read(0 + i);
}

#endif
