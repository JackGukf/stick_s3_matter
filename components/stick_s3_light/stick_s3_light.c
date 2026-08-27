/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/* The StickS3 has no LED, so the Matter light is rendered as a picture of a
 * light on the on-board ST7789P3 LCD: a bulb whose glass fills to the commanded
 * dim level, the level as a percentage, and the node's Matter identity and
 * firmware build along the bottom. OnOff still cuts the backlight, so "off" is
 * a dark panel.
 *
 * This file owns the hardware and the light's state; face.c draws the picture.
 * Rendering is banded -- the face is composed into a 30-line buffer that is
 * flushed a band at a time, because a full 135x240 framebuffer is 63 kB and
 * this target has no PSRAM.
 *
 * Pin map and the power-up sequence follow the M5Stack StickS3 PinMap and
 * M5GFX's board_M5StickS3: the LCD rail (L3B) is not wired to the SoC, it is
 * gated by GPIO2 of the M5PM1 PMIC on I2C, and must be up ~100 ms before the
 * panel is initialized.
 */

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
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

#include "face.h"

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
#define BAND_LINES 30 /* 240 splits into 8 bands of 8100 bytes */

_Static_assert(LCD_WIDTH == FACE_WIDTH && LCD_HEIGHT == FACE_HEIGHT, "panel and face disagree on size");
_Static_assert((int)STICK_S3_NET_OFFLINE == (int)FACE_NET_OFFLINE &&
                   (int)STICK_S3_NET_PAIRING == (int)FACE_NET_PAIRING &&
                   (int)STICK_S3_NET_ONLINE == (int)FACE_NET_ONLINE,
               "net state enums must stay in step");

/* Backlight PWM. The level no longer rides on the backlight, so it only has an
 * on and an off duty. */
#define BL_LEDC_TIMER LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BL_LEDC_RES LEDC_TIMER_10_BIT
#define BL_LEDC_FREQ_HZ 5000
#define BL_DUTY_ON ((1 << 10) - 1)
#define BL_DUTY_OFF 0

/* Bands come from the face; a state change dirties whole bands and the repaint
 * spans from the lowest dirty band to the highest. */
#define DIRTY_STATUS BIT(0)
#define DIRTY_BULB BIT(1)
#define DIRTY_LEVEL BIT(2)
#define DIRTY_FOOTER BIT(3)
#define DIRTY_ALL (DIRTY_STATUS | DIRTY_BULB | DIRTY_LEVEL | DIRTY_FOOTER)

/* A slider drag in the Home app arrives as a burst of CurrentLevel writes. The
 * render task waits this long after the first one so the burst collapses into a
 * single repaint instead of flooding the SPI bus. */
#define UI_COALESCE_MS 40

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_band; /* BAND_LINES * LCD_WIDTH pixels, DMA capable */
static TaskHandle_t s_render_task;
static SemaphoreHandle_t s_lock;
/* esp_lcd_panel_draw_bitmap() queues the colour transfer and returns while DMA
 * is still reading the band buffer, so the next band cannot be drawn into it
 * until this says the wire is clear. */
static SemaphoreHandle_t s_flush_done;

/* Light state. Hue is 0-360 and saturation 0-100, as handed over by app_driver
 * after remapping; the level stays in the Matter 1-254 range because the face
 * shows it raw. */
static bool s_power;
static uint8_t s_level = 64;
static HS_color_t s_hs = {0, 0};
static RGB_color_t s_rgb = {255, 180, 90};
static stick_s3_net_state_t s_net = STICK_S3_NET_OFFLINE;
static char s_line_fw[FACE_MAX_CHARS + 1] = "";
static uint32_t s_dirty;

/* esp-matter's hsv_to_rgb() works in 0-100 per channel while xy_to_rgb() works
 * in 0-255, so the hue/saturation path is scaled up to keep s_rgb at 0-255. The
 * bulb is always drawn at full value: the level is the fill height, not the
 * color's brightness. */
static void hs_to_rgb(void)
{
    hsv_to_rgb(s_hs, 100, &s_rgb);
    s_rgb.red = (uint8_t)(s_rgb.red * 255 / 100);
    s_rgb.green = (uint8_t)(s_rgb.green * 255 / 100);
    s_rgb.blue = (uint8_t)(s_rgb.blue * 255 / 100);
}

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

static esp_err_t backlight_set(uint32_t duty)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty), TAG, "ledc duty failed");
    return ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

/* Runs in the SPI ISR when a band has finished going out. */
static bool IRAM_ATTR panel_flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata,
                                       void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

static esp_err_t panel_init(void)
{
    spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_SCLK_GPIO,
        .mosi_io_num = LCD_MOSI_GPIO,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BAND_LINES * LCD_WIDTH * sizeof(uint16_t),
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
        .on_color_trans_done = panel_flush_done,
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

/* Repaint the panel rows in [y0, y1), band by band. The caller holds no lock;
 * this runs only on the render task. */
static esp_err_t face_render(int y0, int y1)
{
    if (!s_panel || !s_band) {
        return ESP_ERR_INVALID_STATE;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 > LCD_HEIGHT) {
        y1 = LCD_HEIGHT;
    }

    /* Snapshot the state so a write arriving mid-repaint cannot tear the
     * picture; it will raise the dirty flags again and land on the next pass. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    face_state_t state = {
        .power = s_power,
        .level = s_level,
        .red = s_rgb.red,
        .green = s_rgb.green,
        .blue = s_rgb.blue,
        .net = (face_net_t)s_net,
        .line_fw = s_line_fw,
    };
    xSemaphoreGive(s_lock);

    for (int y = y0; y < y1; y += BAND_LINES) {
        int lines = y1 - y < BAND_LINES ? y1 - y : BAND_LINES;
        face_draw_band(s_band, y, lines, &state);
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_WIDTH, y + lines, s_band), TAG,
                            "draw failed");
        /* The band buffer is single: wait for DMA before drawing over it. */
        ESP_RETURN_ON_FALSE(xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG,
                            "panel flush timed out");
    }
    return ESP_OK;
}

/* Turn the set of dirty bands into one repaint range. The bands are contiguous,
 * so the lowest and highest dirty band bound it. */
static void dirty_range(uint32_t dirty, int *y0, int *y1)
{
    *y0 = LCD_HEIGHT;
    *y1 = 0;
    const struct {
        uint32_t bit;
        int y0, y1;
    } bands[] = {
        {DIRTY_STATUS, FACE_BAND_STATUS_Y0, FACE_BAND_STATUS_Y1},
        {DIRTY_BULB, FACE_BAND_BULB_Y0, FACE_BAND_BULB_Y1},
        {DIRTY_LEVEL, FACE_BAND_LEVEL_Y0, FACE_BAND_LEVEL_Y1},
        {DIRTY_FOOTER, FACE_BAND_FOOTER_Y0, FACE_BAND_FOOTER_Y1},
    };
    for (size_t i = 0; i < sizeof(bands) / sizeof(bands[0]); i++) {
        if (dirty & bands[i].bit) {
            if (bands[i].y0 < *y0) {
                *y0 = bands[i].y0;
            }
            if (bands[i].y1 > *y1) {
                *y1 = bands[i].y1;
            }
        }
    }
}

static void mark_dirty(uint32_t bands)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_dirty |= bands;
    xSemaphoreGive(s_lock);
    if (s_render_task) {
        xTaskNotifyGive(s_render_task);
    }
}

static void render_task(void *arg)
{
    bool backlit = false;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Let a burst of updates settle, then swallow the notifications it
         * raised so the repaint happens once. */
        vTaskDelay(pdMS_TO_TICKS(UI_COALESCE_MS));
        ulTaskNotifyTake(pdTRUE, 0);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t dirty = s_dirty;
        s_dirty = 0;
        bool power = s_power;
        xSemaphoreGive(s_lock);

        if (dirty == 0) {
            continue;
        }

        int y0, y1;
        dirty_range(dirty, &y0, &y1);

        /* Coming out of off, drop the backlight first so the redraw is not
         * visible; going into off, blank the panel before cutting it. */
        if (power && !backlit) {
            backlight_set(BL_DUTY_OFF);
        }
        esp_err_t err = face_render(y0, y1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "render failed: %s", esp_err_to_name(err));
            mark_dirty(dirty); /* the panel is stale; try again on the next change */
            continue;
        }
        if (power != backlit) {
            backlight_set(power ? BL_DUTY_ON : BL_DUTY_OFF);
            backlit = power;
        }
    }
}

/* ---- public API --------------------------------------------------------- */

esp_err_t stick_s3_light_init(void)
{
    ESP_LOGI(TAG, "initializing StickS3 LCD light");

    s_band = heap_caps_malloc(BAND_LINES * LCD_WIDTH * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_band, ESP_ERR_NO_MEM, TAG, "no memory for the band buffer");

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "no memory for the state lock");

    s_flush_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_flush_done, ESP_ERR_NO_MEM, TAG, "no memory for the flush semaphore");

    ESP_RETURN_ON_ERROR(lcd_power_on(), TAG, "LCD power-up failed");
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init failed");
    ESP_RETURN_ON_ERROR(panel_init(), TAG, "panel init failed");

    /* Starts off: black panel, backlight down. */
    ESP_RETURN_ON_ERROR(face_render(0, LCD_HEIGHT), TAG, "first render failed");
    ESP_RETURN_ON_ERROR(backlight_set(BL_DUTY_OFF), TAG, "backlight off failed");

    ESP_RETURN_ON_FALSE(xTaskCreate(render_task, "stick_s3_ui", 4096, NULL, 4, &s_render_task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "render task failed to start");
    return ESP_OK;
}

esp_err_t stick_s3_light_set_power(bool power)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_power = power;
    xSemaphoreGive(s_lock);
    mark_dirty(DIRTY_ALL);
    return ESP_OK;
}

esp_err_t stick_s3_light_set_level(uint8_t level)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_level = level;
    xSemaphoreGive(s_lock);
    mark_dirty(DIRTY_BULB | DIRTY_LEVEL);
    return ESP_OK;
}

esp_err_t stick_s3_light_set_hue(uint16_t hue)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_hs.hue = hue;
    hs_to_rgb();
    xSemaphoreGive(s_lock);
    mark_dirty(DIRTY_BULB);
    return ESP_OK;
}

esp_err_t stick_s3_light_set_saturation(uint8_t saturation)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_hs.saturation = saturation;
    hs_to_rgb();
    xSemaphoreGive(s_lock);
    mark_dirty(DIRTY_BULB);
    return ESP_OK;
}

esp_err_t stick_s3_light_set_temperature(uint32_t temperature)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    temp_to_hs(temperature, &s_hs);
    hs_to_rgb();
    xSemaphoreGive(s_lock);
    mark_dirty(DIRTY_BULB);
    return ESP_OK;
}

esp_err_t stick_s3_light_set_xy(uint16_t x, uint16_t y)
{
    XY_color_t xy = {x, y};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    xy_to_rgb(xy, 255, &s_rgb); /* 0-255 scale, unlike hsv_to_rgb */
    xSemaphoreGive(s_lock);
    mark_dirty(DIRTY_BULB);
    return ESP_OK;
}

esp_err_t stick_s3_light_set_net_state(stick_s3_net_state_t state)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_net != state;
    s_net = state;
    xSemaphoreGive(s_lock);
    if (changed) {
        mark_dirty(DIRTY_STATUS);
    }
    return ESP_OK;
}

esp_err_t stick_s3_light_set_firmware(const char *firmware)
{
    ESP_RETURN_ON_FALSE(firmware, ESP_ERR_INVALID_ARG, TAG, "no firmware string");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_line_fw, firmware, sizeof(s_line_fw));
    xSemaphoreGive(s_lock);
    mark_dirty(DIRTY_FOOTER);
    return ESP_OK;
}
