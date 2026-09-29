idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_video esp_lcd esp_lcd_jd9365
                                driver esp_psram heap freertos)