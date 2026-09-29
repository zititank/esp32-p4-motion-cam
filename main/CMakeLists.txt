idf_component_register(SRCS "app_main.c"
                            "display.c"
                            "camera.c"
                            "diagnostics.c"
                            "motion.c"
                            "overlay.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_video esp_lcd esp_lcd_jd9365
                                driver esp_psram esp_mm heap freertos esp_timer)
