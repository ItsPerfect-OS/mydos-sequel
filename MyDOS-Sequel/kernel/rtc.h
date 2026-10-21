#ifndef RTC_H
#define RTC_H
#include "types.h"
u32  rtc_unix_time(void);                                   /* seconds since 1970 (UTC as the CMOS clock reports it); 0 if unreadable */
void rtc_split(u32 t, u32 *y, u32 *mo, u32 *d, u32 *h, u32 *mi, u32 *s);
#endif
