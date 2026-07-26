idf_component_register(
    SRCS
        "app_main.cpp"
        "selftest.cpp"
        "app_config.cpp"
        "model.cpp"
        "bms/daly.cpp"
        "bms/control.cpp"
        "bms/bms_task.cpp"
        "can/mcp2515.cpp"
        "can/solis.cpp"
        "net/wifi.cpp"
        "net/ha_mqtt.cpp"
        "net/ota.cpp"
        "ui/theme.cpp"
        "ui/tiles.cpp"
        "ui/pages.cpp"
        "ui/screens.cpp"
    INCLUDE_DIRS "." "ui" "gen" "bms" "can" "net"
    REQUIRES
        driver esp_driver_spi esp_driver_uart
        nvs_flash fatfs sdmmc esp_driver_sdspi
        esp_wifi esp_netif esp_event mqtt
        app_update esp_app_format esp_timer
)

# Gerätetest: beim Start alle Screens mit eingespielten Werten durchzeichnen und
# base64 über die serielle Konsole dumpen (tools/grab_screens.py, ~150 s).
# ⚠️ Fuer den Normalbetrieb AUS — der Dump haelt die Anzeige fest und der
# BMS-Task wird dabei nicht gestartet.
# set(SOLIS_SELFTEST 1)
if(SOLIS_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SOLIS_SELFTEST=1)
endif()
