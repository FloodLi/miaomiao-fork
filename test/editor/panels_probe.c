/* Read-only production profile inventory. No panel initialization or test execution. */
#define main epd_tests_main
#include "../epd/test_epd.c"
#undef main

static void json_name(const char *s)
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
        epd_image_profile_t p;
        if (!epd_panel_image_profile(id, &p)) continue;
        if (!first) puts(",");
        first = false;
        printf("{\"panel\":%d,\"panel_width\":%d,\"panel_height\":%d,\"panel_name\":",
               id, p.width, p.height);
        json_name(epd_panel_name(id));
        printf(",\"epd_ready\":true,\"panel_has_red\":%s,\"panel_has_yellow\":%s,"
               "\"image_profile\":{\"id\":\"%s\",\"color_type\":\"%s\",\"inks\":[\"black\",\"white\"",
               p.has_red ? "true" : "false", p.has_yellow ? "true" : "false", p.id, p.color_type);
        if (p.has_red) printf(",\"red\"");
        if (p.has_yellow) printf(",\"yellow\"");
        if (id == EPD_PANEL_E6_37_6C) printf(",\"blue\",\"green\"");
        printf("]}}");
    }
    puts("\n]");
    return 0;
}
