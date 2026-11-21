#pragma once

#include "esp_err.h"
#include "display_policy.h"
#include <stdbool.h>
#include <stdint.h>

#define MSG_MAX_LEN  500

typedef enum {
    MSG_ALIGN_LEFT   = 0,
    MSG_ALIGN_CENTER = 1,
} msg_align_t;

typedef struct {
    char        text[MSG_MAX_LEN + 1];
    uint8_t     font_size;     /* 0=1x(16px), 1=2x(32px), 2=3x(48px), 3=4x(64px), 4=5x(80px) */
    msg_align_t align;
    uint8_t     color;         /* 0 black, 1 red, 2 yellow, 3 blue, 4 green */
    int16_t     x_offset;     /* pixel offset from computed position */
    int16_t     y_offset;
    uint8_t     px;            /* >0: 固件渲染直接用该像素值（覆盖 font_size 档位）；
                                * 微信来信固定写 20。0=按档位渲染。 */
} msg_config_t;

esp_err_t message_board_init(void);
esp_err_t message_board_get_config(msg_config_t *out);
esp_err_t message_board_set_config(const msg_config_t *cfg);
esp_err_t message_board_show(void);
esp_err_t message_board_show_queued(unsigned *out_epoch);
display_result_t message_board_wait_result(unsigned epoch);
void message_board_wait_idle(void);

/** 外部 IM 消息（企业微信/个人微信）上屏：固定样式（默认档位、字号 px=25、
 *  黑色、左对齐、偏移归零），\r\n 归一、UTF-8 安全截断到 500 字，
 *  写配置并排队渲染。ESP_OK=已排队（调用方按结果回执 IM）。 */
esp_err_t message_board_show_inbound_text(const char *text);
