#ifndef H8_RTC_H
#define H8_RTC_H

#include "types.h"

#include <time.h>

/**
 * RSECDR counts the BCD-coded second value. The setting range is decimal
 * 00 to 59. It is an 8-bit read register used as a counter, when it operates
 * as a free running counter.
 * Mapped to 0xF068.
 */
typedef union
{
  H8_BITFIELD_3
  (
    /**
     * RTC Busy.
     * This bit is set to 1 when the RTC is updating (operating) the values of
     * second, minute, hour, and day-of-week data registers. When this bit is
     * 0, the values of second, minute, hour, and day-of-week data registers
     * must be adopted.
     */
    h8_u8 bsy : 1,

    /** Counting Ten's Position. Counts from 0-5. */
    h8_u8 ct : 3,

    /** Counting One's Position. Counts from 0-9. */
    h8_u8 co : 4
  ) flags;
  h8_byte_t raw;
} h8_rsecdr_t;

/**
 * RMINDR counts the BCD-coded minute value on the carry generated once per
 * minute by the RSECDR counting. The setting range is decimal 00 to 59.
 * Mapped to 0xF069.
 */
typedef union
{
  H8_BITFIELD_3
  (
    /**
     * RTC Busy.
     * This bit is set to 1 when the RTC is updating (operating) the values of
     * second, minute, hour, and day-of-week data registers. When this bit is
     * 0, the values of second, minute, hour, and day-of-week data registers
     * must be adopted.
     */
    h8_u8 bsy : 1,

    /** Counting Ten's Position. Counts from 0-5. */
    h8_u8 ct : 3,

    /** Counting One's Position. Counts from 0-9. */
    h8_u8 co : 4
  ) flags;
  h8_byte_t raw;
} h8_rmindr_t;

/**
 * RHRDR counts the BCD-coded hour value on the carry generated once per hour
 * by RMINDR. The setting range is either decimal 00 to 11 or 00 to 23 by the
 * selection of the 12/24 bit in RTCCR1.
 * Mapped to 0xF06A.
 */
typedef union
{
  H8_BITFIELD_4
  (
    /**
     * RTC Busy.
     * This bit is set to 1 when the RTC is updating (operating) the values of
     * second, minute, hour, and day-of-week data registers. When this bit is
     * 0, the values of second, minute, hour, and day-of-week data registers
     * must be adopted.
     */
    h8_u8 bsy : 1,

    /** Reserved. This bit is always read as 0. */
    h8_u8 r : 1,

    /** Counting Ten's Position. Counts from 0-2. */
    h8_u8 ct : 2,

    /** Counting One's Position. Counts from 0-9. */
    h8_u8 co : 4
  ) flags;
  h8_byte_t raw;
} h8_rhrdr_t;

enum
{
  H8_RTC_SUNDAY = 0,

  H8_RTC_MONDAY,
  H8_RTC_TUESDAY,
  H8_RTC_WEDNESDAY,
  H8_RTC_THURSDAY,
  H8_RTC_FRIDAY,
  H8_RTC_SATURDAY,

  H8_RTC_RESERVED
};

/**
 * RWKDR counts the BCD-coded day-of-week value on the carry generated once per
 * day by RHRDR. The setting range is decimal 0 to 6 using bits WK2 to WK0.
 */
typedef union
{
  H8_BITFIELD_3
  (
    /**
     * RTC Busy.
     * This bit is set to 1 when the RTC is updating (operating) the values of
     * second, minute, hour, and day-of-week data registers. When this bit is
     * 0, the values of second, minute, hour, and day-of-week data registers
     * must be adopted.
     */
    h8_u8 bsy : 1,

    /** Reserved. These bits are always read as 0. */
    h8_u8 r : 4,

    /** Day-of-Week Counting. Counts from 0-6. */
    h8_u8 wk : 3
  ) flags;
  h8_byte_t raw;
} h8_rwkdr_t;

enum
{
  H8_RTC_12H = 0,
  H8_RTC_24H = 1
};

/**
 * RTCCR1 controls start/stop and reset of the clock timer.
 */
typedef union
{
  H8_BITFIELD_6
  (
    /** RTC Operation Start. Represents whether RTC is active. */
    h8_u8 run : 1,

    /** Operating Mode. Using 24H mode when set. */
    h8_u8 om : 1,

    /** AM/PM. Set when after noon in 12H mode. */
    h8_u8 pm : 1,

    /**
     * Reset. Resets registers and control circuits except RTCCSR and this
     * bit. Clear this bit to 0 after having been set to 1.
     */
    h8_u8 rst : 1,

    /** Interrupt Occurrence Timing. @todo */
    h8_u8 intr : 1,

    /** Reserved. These bits are always read as 0. */
    h8_u8 r : 3
  ) flags;
  h8_byte_t raw;
} h8_rtccr1_t;

/** RTCCR2. @todo Not particularly needed for an emulator. */
typedef union
{
  h8_byte_t raw;
} h8_rtccr2_t;

/** RTCCSR. @todo Not particularly needed for an emulator. */
typedef union
{
  h8_byte_t raw;
} h8_rtccsr_t;

/** RTCFLG. @todo Not particularly needed for an emulator. */
typedef union
{
  h8_byte_t raw;
} h8_rtcflg_t;

typedef struct
{
  h8_rsecdr_t rsecdr;
  h8_rmindr_t rmindr;
  h8_rhrdr_t rhrdr;
  h8_rwkdr_t rwkdr;
  h8_rtccr1_t rtccr1;
  h8_rtccr2_t rtccr2;
  h8_rtccsr_t rtccsr;
  h8_rtcflg_t rtcflg;
} h8_rtc_t;

/**
 * Updates the system's RTC registers to match a given time_t.
 */
void h8_rtc_set(h8_rtc_t *rtc, const time_t time);

/**
 * Updates the system's RTC registers to match the current time.
 */
void h8_rtc_set_current(h8_rtc_t *rtc, const time_t offset);

#endif
