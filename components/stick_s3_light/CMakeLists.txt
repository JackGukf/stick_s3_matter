# The color conversions (HSV / mireds / CIE xy -> RGB) are reused from
# esp-matter's device_hal, so this board behaves like the stock light example.
idf_component_register(SRCS         "stick_s3_light.c"
                                    "$ENV{ESP_MATTER_PATH}/device_hal/led_driver/utils/color_format.c"
                       INCLUDE_DIRS "include"
                                    "$ENV{ESP_MATTER_PATH}/device_hal/led_driver/include"
                       PRIV_REQUIRES driver esp_lcd)
