#include "soft_i2c.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

#define SCL_HALF_US 5

static soft_i2c_t *g_bus = NULL;

static void sda_out(void)
{
    gpio_set_direction(g_bus->sda, GPIO_MODE_OUTPUT_OD);
}

static void sda_in(void)
{
    gpio_set_direction(g_bus->sda, GPIO_MODE_INPUT);
}

static void scl_high(void) { gpio_set_level(g_bus->scl, 1); }
static void scl_low(void)  { gpio_set_level(g_bus->scl, 0); }
static void sda_high(void) { gpio_set_level(g_bus->sda, 1); }
static void sda_low(void)  { gpio_set_level(g_bus->sda, 0); }
static int  sda_read(void) { return gpio_get_level(g_bus->sda); }

static void i2c_delay(void)
{
    esp_rom_delay_us(SCL_HALF_US);
}

static void i2c_start(void)
{
    sda_out();
    sda_high();
    scl_high();
    i2c_delay();
    sda_low();
    i2c_delay();
    scl_low();
}

static void i2c_stop(void)
{
    sda_out();
    sda_low();
    scl_high();
    i2c_delay();
    sda_high();
    i2c_delay();
}

static bool i2c_write_byte(uint8_t b)
{
    sda_out();
    for (int i = 7; i >= 0; i--) {
        if (b & (1 << i)) sda_high(); else sda_low();
        i2c_delay();
        scl_high();
        i2c_delay();
        scl_low();
    }
    /* ACK */
    sda_in();
    scl_high();
    i2c_delay();
    bool ack = (sda_read() == 0);
    scl_low();
    sda_out();
    return ack;
}

static uint8_t i2c_read_byte(bool ack)
{
    uint8_t b = 0;
    sda_in();
    for (int i = 7; i >= 0; i--) {
        scl_high();
        i2c_delay();
        if (sda_read()) b |= (1 << i);
        scl_low();
        i2c_delay();
    }
    sda_out();
    if (ack) sda_low(); else sda_high();
    scl_high();
    i2c_delay();
    scl_low();
    sda_high();
    return b;
}

void soft_i2c_init(soft_i2c_t *bus, int sda, int scl)
{
    bus->sda = sda;
    bus->scl = scl;
    g_bus = bus;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode         = GPIO_MODE_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    sda_high();
    scl_high();
}

bool soft_i2c_write(uint8_t addr, const uint8_t *data, int len)
{
    i2c_start();
    if (!i2c_write_byte((addr << 1) | 0)) {
        i2c_stop();
        return false;
    }
    for (int i = 0; i < len; i++) {
        if (!i2c_write_byte(data[i])) {
            i2c_stop();
            return false;
        }
    }
    i2c_stop();
    return true;
}

bool soft_i2c_read(uint8_t addr, uint8_t *buf, int len)
{
    i2c_start();
    if (!i2c_write_byte((addr << 1) | 1)) {
        i2c_stop();
        return false;
    }
    for (int i = 0; i < len; i++) {
        buf[i] = i2c_read_byte(i < len - 1);
    }
    i2c_stop();
    return true;
}
