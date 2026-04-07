#include "lunar.h"
#include <stdio.h>
#include <string.h>

/*
 * Classic lunar calendar lookup table, years 1900-2100.
 * Encoding per entry (20 bits):
 *   Bits 3-0   : Leap month number (0 = none, 1-12)
 *   Bits 15-4  : Months 1-12 (bit 15 = month 1, bit 4 = month 12)
 *                1 = 大月 (30 days), 0 = 小月 (29 days)
 *   Bit 16     : Leap month has 30 days (1) or 29 days (0)
 */
static const uint32_t LUNAR_INFO[] = {
    0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260, /* 1900-1904 */
    0x0d950, 0x16554, 0x056a0, 0x09ad0, 0x055d2, /* 1905-1909 */
    0x04ae0, 0x0a5b6, 0x0a4d0, 0x0d250, 0x1d255, /* 1910-1914 */
    0x0b540, 0x0d6a0, 0x0ada2, 0x095b0, 0x14977, /* 1915-1919 */
    0x04970, 0x0a4b0, 0x0b4b5, 0x06a50, 0x06d40, /* 1920-1924 */
    0x1ab54, 0x02b60, 0x09570, 0x052f2, 0x04970, /* 1925-1929 */
    0x06566, 0x0d4a0, 0x0ea50, 0x16e95, 0x05ad0, /* 1930-1934 */
    0x02b60, 0x186e3, 0x092e0, 0x1c8d7, 0x0c950, /* 1935-1939 */
    0x0d4a0, 0x1d8a6, 0x0b550, 0x056a0, 0x1a5b4, /* 1940-1944 */
    0x025d0, 0x092d0, 0x0d2b2, 0x0a950, 0x0b557, /* 1945-1949 */
    0x06ca0, 0x0b550, 0x15355, 0x04da0, 0x0a5b0, /* 1950-1954 */
    0x14573, 0x052b0, 0x0a9a8, 0x0e950, 0x06aa0, /* 1955-1959 */
    0x0aea6, 0x0ab50, 0x04b60, 0x0aae4, 0x0a570, /* 1960-1964 */
    0x05260, 0x0f263, 0x0d950, 0x05b57, 0x056a0, /* 1965-1969 */
    0x096d0, 0x04dd5, 0x04ad0, 0x0a4d0, 0x0d4d4, /* 1970-1974 */
    0x0d250, 0x0d558, 0x0b540, 0x0b6a0, 0x195a6, /* 1975-1979 */
    0x095b0, 0x049b0, 0x0a974, 0x0a4b0, 0x0b27a, /* 1980-1984 */
    0x06a50, 0x06d40, 0x0af46, 0x0ab60, 0x09570, /* 1985-1989 */
    0x04af5, 0x04970, 0x064b0, 0x074a3, 0x0ea50, /* 1990-1994 */
    0x06b58, 0x05ac0, 0x0ab60, 0x096d5, 0x092e0, /* 1995-1999 */
    0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552, /* 2000-2004 */
    0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5, /* 2005-2009 */
    0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, /* 2010-2014 */
    0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930, /* 2015-2019 */
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, /* 2020-2024 */
    0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530, /* 2025-2029 */
    0x05aa0, 0x076a3, 0x096d0, 0x04afb, 0x04ad0, /* 2030-2034 */
    0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45, /* 2035-2039 */
    0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577, /* 2040-2044 */
    0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0, /* 2045-2049 */
    0x14b63, 0x09370, 0x049f8, 0x04970, 0x064b0, /* 2050-2054 */
    0x168a6, 0x0ea50, 0x06aa0, 0x1a6c4, 0x0aae0, /* 2055-2059 */
    0x092e0, 0x0d2e3, 0x0c960, 0x0d557, 0x0d4a0, /* 2060-2064 */
    0x0da50, 0x05d55, 0x056a0, 0x0a6d0, 0x055d4, /* 2065-2069 */
    0x052d0, 0x0a9b8, 0x0a950, 0x0b4a0, 0x0b6a6, /* 2070-2074 */
    0x0ad50, 0x055a0, 0x0aba4, 0x0a5b0, 0x052b0, /* 2075-2079 */
    0x0b273, 0x06930, 0x07337, 0x06aa0, 0x0ad50, /* 2080-2084 */
    0x14b55, 0x04b60, 0x0a570, 0x054e4, 0x0d160, /* 2085-2089 */
    0x0e968, 0x0d520, 0x0daa0, 0x16aa6, 0x056d0, /* 2090-2094 */
    0x04ae0, 0x0a9d4, 0x0a4d0, 0x0d150, 0x0f252, /* 2095-2099 */
    0x0d520,                                       /* 2100       */
};

#define LUNAR_BASE_YEAR 1900
#define LUNAR_TABLE_LEN (sizeof(LUNAR_INFO) / sizeof(LUNAR_INFO[0]))

static int lunar_leap_month(int y)
{
    return LUNAR_INFO[y - LUNAR_BASE_YEAR] & 0xF;
}

static int lunar_leap_days(int y)
{
    if (!lunar_leap_month(y)) return 0;
    return (LUNAR_INFO[y - LUNAR_BASE_YEAR] & 0x10000) ? 30 : 29;
}

static int lunar_month_days(int y, int m)
{
    return (LUNAR_INFO[y - LUNAR_BASE_YEAR] & (0x10000 >> m)) ? 30 : 29;
}

static int lunar_year_days(int y)
{
    int sum = 348;
    uint32_t info = LUNAR_INFO[y - LUNAR_BASE_YEAR];
    for (int i = 0x8000; i > 0x8; i >>= 1)
        if (info & i) sum++;
    return sum + lunar_leap_days(y);
}

static int to_jdn(int y, int m, int d)
{
    int a = (14 - m) / 12;
    int yy = y + 4800 - a;
    int mm = m + 12 * a - 3;
    return d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
}

bool lunar_from_solar(int y, int m, int d, lunar_date_t *out)
{
    if (y < LUNAR_BASE_YEAR || y > (int)(LUNAR_BASE_YEAR + LUNAR_TABLE_LEN - 1))
        return false;

    int offset = to_jdn(y, m, d) - to_jdn(1900, 1, 31);
    if (offset < 0) return false;

    int ly = LUNAR_BASE_YEAR;
    int yd;
    while (ly < (int)(LUNAR_BASE_YEAR + LUNAR_TABLE_LEN) &&
           offset >= (yd = lunar_year_days(ly))) {
        offset -= yd;
        ly++;
    }
    if (ly >= (int)(LUNAR_BASE_YEAR + LUNAR_TABLE_LEN)) return false;

    int lm_leap = lunar_leap_month(ly);
    bool is_leap = false;
    int lm = 1;

    for (; lm <= 12; lm++) {
        int md = lunar_month_days(ly, lm);
        if (offset < md) break;
        offset -= md;

        if (lm == lm_leap) {
            int ld = lunar_leap_days(ly);
            if (!is_leap && offset < ld) {
                is_leap = true;
                break;
            }
            offset -= ld;
        }
    }
    if (lm > 12) return false;

    out->year    = ly;
    out->month   = lm;
    out->day     = offset + 1;
    out->is_leap = is_leap;
    return true;
}

/* ── lunar date strings ─────────────────────────────────────────── */

static const char *DAY_STR[] = {
    "", "初一","初二","初三","初四","初五","初六","初七","初八","初九","初十",
    "十一","十二","十三","十四","十五","十六","十七","十八","十九","二十",
    "廿一","廿二","廿三","廿四","廿五","廿六","廿七","廿八","廿九","三十",
};

static const char *MONTH_STR[] = {
    "", "正月","二月","三月","四月","五月","六月",
    "七月","八月","九月","十月","冬月","腊月",
};

const char *lunar_day_str(int day)
{
    if (day < 1 || day > 30) return "";
    return DAY_STR[day];
}

const char *lunar_month_str(int month)
{
    if (month < 1 || month > 12) return "";
    return MONTH_STR[month];
}

/* ── 天干地支 + 生肖 ─────────────────────────────────────────── */

static const char *TIAN_GAN[] = {
    "甲","乙","丙","丁","戊","己","庚","辛","壬","癸"
};
static const char *DI_ZHI[] = {
    "子","丑","寅","卯","辰","巳","午","未","申","酉","戌","亥"
};
static const char *SHENG_XIAO[] = {
    "鼠","牛","虎","兔","龙","蛇","马","羊","猴","鸡","狗","猪"
};

static char s_gz_buf[16];
static char s_sx_buf[8];

const char *lunar_year_gz(int lunar_year)
{
    int idx = (lunar_year - 4) % 60;
    if (idx < 0) idx += 60;
    snprintf(s_gz_buf, sizeof(s_gz_buf), "%s%s",
             TIAN_GAN[idx % 10], DI_ZHI[idx % 12]);
    return s_gz_buf;
}

const char *lunar_year_sx(int lunar_year)
{
    int idx = (lunar_year - 4) % 12;
    if (idx < 0) idx += 12;
    snprintf(s_sx_buf, sizeof(s_sx_buf), "%s", SHENG_XIAO[idx]);
    return s_sx_buf;
}

/* ── 24 solar terms ─────────────────────────────────────────── */

static const char *TERM_NAMES[] = {
    "小寒","大寒","立春","雨水","惊蛰","春分",
    "清明","谷雨","立夏","小满","芒种","夏至",
    "小暑","大暑","立秋","处暑","白露","秋分",
    "寒露","霜降","立冬","小雪","大雪","冬至",
};

static const double ST_C[] = {
    5.4055, 20.12,  3.87,   18.73,  5.63,   20.646,
    4.81,   20.1,   5.52,   21.04,  5.678,  21.37,
    7.108,  22.83,  7.5,    23.13,  7.646,  23.042,
    8.318,  23.438, 7.438,  22.36,  7.18,   21.94,
};

const char *lunar_solar_term(int y, int m, int d)
{
    if (y < 2000 || y > 2099) return NULL;
    int base = (m - 1) * 2;
    if (base < 0 || base > 22) return NULL;

    int cy = y - 2000;
    for (int i = 0; i < 2; i++) {
        int idx = base + i;
        int td = (int)(cy * 0.2422 + ST_C[idx]) - (cy / 4);
        if (td == d) return TERM_NAMES[idx];
    }
    return NULL;
}

/* ── festivals ───────────────────────────────────────────────── */

const char *lunar_festival(int y, int m, int d, const lunar_date_t *ld)
{
    (void)y;
    if (m == 1 && d == 1)  return "元旦";
    if (m == 5 && d == 1)  return "劳动节";
    if (m == 10 && d == 1) return "国庆节";
    if (m == 3 && d == 8)  return "妇女节";
    if (m == 6 && d == 1)  return "儿童节";

    if (!ld) return NULL;
    if (!ld->is_leap) {
        if (ld->month == 1 && ld->day == 1)  return "春节";
        if (ld->month == 1 && ld->day == 15) return "元宵";
        if (ld->month == 5 && ld->day == 5)  return "端午";
        if (ld->month == 7 && ld->day == 7)  return "七夕";
        if (ld->month == 8 && ld->day == 15) return "中秋";
        if (ld->month == 9 && ld->day == 9)  return "重阳";
        if (ld->month == 12 && ld->day == 8) return "腊八";
        if (ld->month == 12 && ld->day == 30) return "除夕";
    }
    return NULL;
}
