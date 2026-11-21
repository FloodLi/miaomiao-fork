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
        "http_receive.c"
        "image_convert.c"
        "epd_stub.c"
        "lodepng.c"
        "time_sync.c"
        "scheduler.c"
        "fb_render.c"
        "font_ext.c"
        "ui_theme.c"
        "ui_color_theme.c"
        "welcome_config.c"
        "ui_crisp_font.c"
        "weather.c"
        "weather_icons_qw.c"
        "clock_display.c"
        "display_policy.c"
        "message_board.c"
        "wecom_bot.c"
        "wechat_bot.c"
        "lunar.c"
        "calendar_display.c"
        "timetable.c"
        "todo.c"
        "button.c"
        "power_mgr.c"
        "countdown.c"
        "codex_quota.c"
        "nvs_utils.c"
        "http_gallery.c"
        "http_features.c"
        "http_canvas.c"
        "canvas_board.c"
        "display_mode.c"
        "battery_mon.c"
        "service_status.c"
    INCLUDE_DIRS "."
    EMBED_FILES
        "${PROJECT_DIR}/web/index.html"
        "${PROJECT_DIR}/web/config.html"
        "${PROJECT_DIR}/web/gallery.html"
        "${PROJECT_DIR}/web/weather.html"
        "${PROJECT_DIR}/web/clock.html"
        "${PROJECT_DIR}/web/calendar.html"
        "${PROJECT_DIR}/web/message.html"
        "${PROJECT_DIR}/web/codex.html"
        "${PROJECT_DIR}/web/timetable.html"
        "${PROJECT_DIR}/web/todo.html"
        "${PROJECT_DIR}/web/countdown.html"
        "${PROJECT_DIR}/web/board.html"
        "${PROJECT_DIR}/web/feature_help.js"
        "${PROJECT_DIR}/web/qr.js"
        "${PROJECT_DIR}/web/image_processor.js"
        "${PROJECT_DIR}/web/miaooaim-mark.png"
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        lwip
        spiffs
        json
        esp_driver_gpio
        esp_driver_spi
        app_update
        esp_http_client
        esp_websocket_client
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

# 屏幕尺寸裁剪：默认三个宏都不定义 = 对应尺寸的驱动与表项不编译（省 flash）。
# 需要某档屏幕时取消对应注释重新编译（同时放开 web/config.html 里
# “[EPD_SIZE 门控]” 标记的面板选项）：
# target_compile_definitions(${COMPONENT_LIB} PRIVATE EPD_SIZE_13_37)   # 1.3-1.7"
# target_compile_definitions(${COMPONENT_LIB} PRIVATE EPD_SIZE_42_43)   # 4.2-4.3"（含4.26"）
# target_compile_definitions(${COMPONENT_LIB} PRIVATE EPD_SIZE_97_10)   # 9.7-10.02"（含10.2"）

# Under -Os (CONFIG_COMPILER_OPTIMIZATION_SIZE) GCC's stricter static analysis
# emits false-positive -Wformat-truncation/-Wformat-overflow on snprintf calls
# whose runtime values are bounded (e.g. struct tm month/day) but unknown to the
# compiler. Demote these two diagnostics from fatal (-Werror) back to plain
# warnings for our own app component only; IDF components keep their full
# warnings-as-errors integrity.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format-truncation
    -Wno-error=format-overflow
)

