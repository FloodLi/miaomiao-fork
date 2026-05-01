if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR
        "This firmware targets ESP32-S3 only. Current IDF_TARGET='${IDF_TARGET}'. "
        "Run: idf.py fullclean && idf.py set-target esp32s3")
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "device_identity.c"
        "wifi_manager.c"
        "spiffs_mount.c"
        "http_app.c"
        "image_convert.c"
        "epd_stub.c"
        "lodepng.c"
        "time_sync.c"
        "scheduler.c"
        "fb_render.c"
        "weather.c"
        "clock_display.c"
        "display_policy.c"
        "message_board.c"
        "lunar.c"
        "calendar_display.c"
        "timetable.c"
        "todo.c"
        "button.c"
        "power_mgr.c"
        "countdown.c"
        "nvs_utils.c"
        "http_gallery.c"
        "http_features.c"
        "http_canvas.c"
        "canvas_board.c"
        "display_mode.c"
        "battery_mon.c"
    INCLUDE_DIRS "."
    EMBED_FILES
        "${PROJECT_DIR}/web/index.html"
        "${PROJECT_DIR}/web/config.html"
        "${PROJECT_DIR}/web/timetable.html"
        "${PROJECT_DIR}/web/todo.html"
        "${PROJECT_DIR}/web/countdown.html"
        "${PROJECT_DIR}/web/board.html"
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        spiffs
        json
        esp_driver_gpio
        esp_driver_spi
        app_update
        esp_http_client
        esp-tls
        esp_timer
        esp_adc
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LODEPNG_NO_COMPILE_ENCODER=1
    LODEPNG_NO_COMPILE_DISK=1
    LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS=1
    LODEPNG_NO_COMPILE_CPP=1
)

