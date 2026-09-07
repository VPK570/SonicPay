idf_component_register(
    SRCS "main.c" "monocypher.c" "ssd1306.c" "peripherals.c" "ledger.c"
    INCLUDE_DIRS "."
    REQUIRES spiffs driver esp_adc esp_timer u8g2
)
target_link_libraries(${COMPONENT_LIB} m)
