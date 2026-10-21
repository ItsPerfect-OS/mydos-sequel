/* rtc.c - CMOS real-time clock (ports 0x70/0x71) and date arithmetic */
#include "rtc.h"
#include "io.h"

static u8 cmos(u8 reg) { outb(0x70, reg); return inb(0x71); }

static u32 days_from_civil(u32 y, u32 m, u32 d) {          /* days since 1970-01-01 (valid for y >= 1970) */
    if (m <= 2) y--;
    u32 era = y / 400;
    u32 yoe = y - era * 400;
    u32 doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void rtc_split(u32 t, u32 *y, u32 *mo, u32 *d, u32 *h, u32 *mi, u32 *s) {
    u32 days = t / 86400, rem = t % 86400;
    *h = rem / 3600; *mi = (rem % 3600) / 60; *s = rem % 60;
    u32 z = days + 719468;
    u32 era = z / 146097;
    u32 doe = z - era * 146097;
    u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    u32 yy = yoe + era * 400;
    u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    u32 mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *mo = mp < 10 ? mp + 3 : mp - 9;
    *y = yy + (*mo <= 2);
}

static u32 bcd(u32 v) { return (v & 0x0F) + ((v >> 4) * 10); }

u32 rtc_unix_time(void) {
    u32 sec = 0, min = 0, hour = 0, day = 0, mon = 0, year = 0, b = 0;
    u32 prev = 0xFFFFFFFFu;
    for (int tries = 0; tries < 5; tries++) {
        for (u32 i = 0; i < 100000 && (cmos(0x0A) & 0x80); i++) {}   /* wait out an update in progress */
        sec = cmos(0x00); min = cmos(0x02); hour = cmos(0x04);
        day = cmos(0x07); mon = cmos(0x08); year = cmos(0x09); b = cmos(0x0B);
        u32 sig = sec | (min << 8) | (hour << 16) | (day << 24);
        if (sig == prev) break;                                       /* two identical reads: consistent */
        prev = sig;
    }
    int pm = 0;
    if (!(b & 0x02) && (hour & 0x80)) { pm = 1; hour &= 0x7F; }       /* 12-hour mode */
    if (!(b & 0x04)) { sec = bcd(sec); min = bcd(min); hour = bcd(hour); day = bcd(day); mon = bcd(mon); year = bcd(year); }
    if (!(b & 0x02) && pm) hour = (hour % 12) + 12;
    else if (!(b & 0x02) && hour == 12) hour = 0;
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59) return 0;
    year += 2000;
    return days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
}
