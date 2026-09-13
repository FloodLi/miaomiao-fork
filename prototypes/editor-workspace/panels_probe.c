/* Read-only host inventory. The renamed test entry point is never executed. */
#define main epd_test_main
#include "../../test/epd/test_epd.c"
#undef main

static void json_string(const char *s)
{
    putchar('"');
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\') putchar('\\');
        putchar(*s);
    }
    putchar('"');
}

int main(void)
{
    bool first = true;
    puts("[");
    for (int id = 0; id < EPD_PANEL_COUNT; ++id) {
        const epd_panel_desc_t *p = &s_specs[id];
        if (!p->name) continue;
        const char *family = p->data_mode == EPD_CMD_DATA_E6_3IN7_6C ? "e6" :
                             p->has_yellow ? "bwry" : p->has_red ? "bwr" : "bw";
        if (!first) puts(",");
        first = false;
        printf("{\"id\":%d,\"width\":%d,\"height\":%d,\"family\":\"%s\",\"name\":",id,p->w,p->h,family);
        json_string(p->name);
        printf(",\"profileId\":\"preview-%d\",\"source\":\"driver-descriptor\"}",id);
    }
    puts("\n]");
    return 0;
}
