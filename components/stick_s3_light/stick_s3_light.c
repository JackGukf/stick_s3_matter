/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/* The StickS3 has no LED, so the Matter light is rendered on the on-board
 * ST7789P3 LCD: the whole panel is filled with the current color and the
 * backlight PWM carries the brightness level.
 *
 * Pin map and the power-up sequence follow the M5Stack StickS3 PinMap and
 * M5GFX's board_M5StickS3 init: the LCD rail (L3B) is not wired to the SoC, it
 * is gated by GPIO2 of the M5PM1 PMIC on I2C, and must be up ~100 ms before the
 * panel is initialized.
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_st7789.h>
#include <esp_log.h>
#include <string.h>

#include <color_format.h>
#include <stick_s3_light.h>

static const char *TAG = "stick_s3_light";

/* I2C (shared bus: PMIC, IMU, audio codec) */
#define I2C_SDA_GPIO 47
#define I2C_SCL_GPIO 48
#define I2C_FREQ_HZ 100000

/* M5PM1 PMIC */
#define M5PM1_ADDR 0x6E
#define M5PM1_REG_DEVICE_ID 0x00
#define M5PM1_DEVICE_ID 0x50
#define M5PM1_REG_I2C_CFG 0x09
#define M5PM1_REG_GPIO_MODE 0x10 /* 1 = output */
#define M5PM1_REG_GPIO_OUT 0x11
#define M5PM1_REG_GPIO_DRV 0x13  /* 1 = open-drain, 0 = push-pull */
#define M5PM1_REG_GPIO_FUNC0 0x16 /* 2 bits per pin, 00 = plain GPIO */
#define M5PM1_LCD_POWER_PIN 2     /* PYG2 gates the L3B rail (LCD/MIC/SPK) */

/* ST7789P3 panel */
#define LCD_MOSI_GPIO 39
#define LCD_SCLK_GPIO 40
#define LCD_DC_GPIO 45
#define LCD_CS_GPIO 41
#define LCD_RST_GPIO 21
#define LCD_BL_GPIO 38
#define LCD_HOST SPI2_HOST
#define LCD_PCLK_HZ (40 * 1000 * 1000)
#define LCD_WIDTH 135
#define LCD_HEIGHT 240
#define LCD_X_GAP 52
#define LCD_Y_GAP 40
#define LCD_FILL_LINES 20 /* lines per draw_bitmap chunk */

/* Backlight PWM */
#define BL_LEDC_TIMER LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BL_LEDC_RES LEDC_TIMER_10_BIT
#define BL_LEDC_FREQ_HZ 5000
#define BL_DUTY_MAX ((1 << 10) - 1)
/* Matter level 1 must still be visible, so the backlight never goes below this
 * duty while the light is on. */
#define BL_DUTY_MIN 30

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_line_buf; /* LCD_FILL_LINES * LCD_WIDTH pixels, DMA capable */

/* Current light state. Brightness/saturation are 0-100, hue 0-360, as handed
 * over by app_driver after remapping from the Matter ranges. */
static bool s_power;
static uint8_t s_brightness = 64;
static HS_color_t s_hs = {0, 0};
static RGB_color_t s_rgb = {0, 0, 0};

static esp_err_t m5pm1_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, 100);
}

static esp_err_t m5pm1_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), 100);
}

static esp_err_t m5pm1_update(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t clear_mask, uint8_t set_mask)
{
    uint8_t val = 0;
    esp_err_t err = m5pm1_read(dev, reg, &val);
    if (err != ESP_OK) {
        return err;
    }
    val = (uint8_t)((val & ~clear_mask) | set_mask);
    return m5pm1_write(dev, reg, val);
}

/* Bring up the LCD/MIC/SPK rail (L3B) through the PMIC. */
static esp_err_t lcd_power_on(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus), TAG, "i2c bus init failed");

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = M5PM1_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_config, &dev), TAG, "pmic add failed");

    /* The PMIC may drop the first transfer while waking from idle, so retry. */
    uint8_t id = 0;
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) {
        err = m5pm1_read(dev, M5PM1_REG_DEVICE_ID, &id);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "M5PM1 not responding on I2C");
    if (id != M5PM1_DEVICE_ID) {
        ESP_LOGW(TAG, "unexpected PMIC id 0x%02x (expected 0x%02x)", id, M5PM1_DEVICE_ID);
    }

    const uint8_t pin_bit = BIT(M5PM1_LCD_POWER_PIN);
    const uint8_t func_mask = (uint8_t)(0x3 << (M5PM1_LCD_POWER_PIN * 2));
    ESP_RETURN_ON_ERROR(m5pm1_update(dev, M5PM1_REG_GPIO_FUNC0, func_mask, 0), TAG, "gpio2 func failed");
    ESP_RETURN_ON_ERROR(m5pm1_update(dev, M5PM1_REG_GPIO_MODE, 0, pin_bit), TAG, "gpio2 output failed");
    ESP_RETURN_ON_ERROR(m5pm1_update(dev, M5PM1_REG_GPIO_DRV, pin_bit, 0), TAG, "gpio2 push-pull failed");
    ESP_RETURN_ON_ERROR(m5pm1_update(dev, M5PM1_REG_GPIO_OUT, 0, pin_bit), TAG, "L3B enable failed");
    /* Disable I2C idle sleep; the PMIC keeps its registers across a shutdown,
     * so a value left over from other firmware would break communication. */
    ESP_RETURN_ON_ERROR(m5pm1_write(dev, M5PM1_REG_I2C_CFG, 0x00), TAG, "i2c cfg failed");

    vTaskDelay(pdMS_TO_TICKS(100)); /* vendor rail settle time */
    return ESP_OK;
}

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer_config = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG, "ledc timer failed");

    ledc_channel_config_t channel_config = {
        .gpio_num = LCD_BL_GPIO,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&channel_config);
}

static esp_err_t backlight_set(uint8_t brightness /* 0-100 */)
{
    uint32_t duty = 0;
    if (brightness > 0) {
        duty = BL_DUTY_MIN + ((BL_DUTY_MAX - BL_DUTY_MIN) * brightness) / 100;
    }
    ESP_RETURN_ON_ERROR(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty), TAG, "ledc duty failed");
    return ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

static esp_err_t panel_init(void)
{
    spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_SCLK_GPIO,
        .mosi_io_num = LCD_MOSI_GPIO,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_FILL_LINES * LCD_WIDTH * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO), TAG, "spi bus init failed");

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_CS_GPIO,
        .dc_gpio_num = LCD_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io), TAG,
                        "panel io init failed");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_config, &s_panel), TAG, "st7789 init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    /* The StickS3 panel is wired inverted, and the visible 135x240 window sits
     * at this offset inside the controller's frame memory. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "panel invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, LCD_Y_GAP), TAG, "panel gap failed");
    return esp_lcd_panel_disp_on_off(s_panel, true);
}

/* Paint the whole panel one color. */
static esp_err_t panel_fill(RGB_color_t rgb)
{
    if (!s_panel || !s_line_buf) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t color = (uint16_t)(((rgb.red & 0xF8) << 8) | ((rgb.green & 0xFC) << 3) | (rgb.blue >> 3));
    color = (uint16_t)((color >> 8) | (color << 8)); /* the panel expects big-endian pixels */

    const size_t pixels = LCD_FILL_LINES * LCD_WIDTH;
    for (size_t i = 0; i < pixels; i++) {
        s_line_buf[i] = color;
    }

    for (int y = 0; y < LCD_HEIGHT; y += LCD_FILL_LINES) {
        int lines = LCD_HEIGHT - y < LCD_FILL_LINES ? LCD_HEIGHT - y : LCD_FILL_LINES;
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + lines, s_line_buf), TAG,
                            "draw failed");
    }
    return ESP_OK;
}

/* Push the current state to the panel: color on the pixels, level on the
 * backlight. Off means a black screen with the backlight down. */
static esp_err_t light_refresh(void)
{
    if (!s_power) {
        RGB_color_t black = {0, 0, 0};
        esp_err_t err = panel_fill(black);
        return err != ESP_OK ? err : backlight_set(0);
    }
    ESP_RETURN_ON_ERROR(panel_fill(s_rgb), TAG, "fill failed");
    return backlight_set(s_brightness);
}

esp_err_t stick_s3_light_init(void)
{
    ESP_LOGI(TAG, "initializing StickS3 LCD light");

    s_line_buf = heap_caps_malloc(LCD_FILL_LINES * LCD_WIDTH * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_line_buf, ESP_ERR_NO_MEM, TAG, "no memory for the line buffer");

    ESP_RETURN_ON_ERROR(lcd_power_on(), TAG, "LCD power-up failed");
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init failed");
    ESP_RETURN_ON_ERROR(panel_init(), TAG, "panel init failed");

    return light_refresh(); /* starts off: black screen, backlight down */
}

esp_err_t stick_s3_light_set_power(bool power)
{
    s_power = power;
    return light_refresh();
}

esp_err_t stick_s3_light_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
    return s_power ? backlight_set(s_brightness) : ESP_OK;
}

esp_err_t stick_s3_light_set_hue(uint16_t hue)
{
    s_hs.hue = hue;
    /* The panel shows the fully saturated color; the level lives on the
     * backlight, so the color itself is always converted at full value. */
    hsv_to_rgb(s_hs, 100, &s_rgb);
    return light_refresh();
}

esp_err_t stick_s3_light_set_saturation(uint8_t saturation)
{
    s_hs.saturation = saturation;
    hsv_to_rgb(s_hs, 100, &s_rgb);
    return light_refresh();
}

esp_err_t stick_s3_light_set_temperature(uint32_t temperature)
{
    temp_to_hs(temperature, &s_hs);
    hsv_to_rgb(s_hs, 100, &s_rgb);
    return light_refresh();
}

esp_err_t stick_s3_light_set_xy(uint16_t x, uint16_t y)
{
    XY_color_t xy = {x, y};
    xy_to_rgb(xy, 100, &s_rgb);
    return light_refresh();
}
