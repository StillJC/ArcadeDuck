#ifndef H8_REGISTERS_H
#define H8_REGISTERS_H

#include "types.h"

/**
 * FLMCR1 is a register that makes the flash memory enter the programming mode,
 * programming-verifying mode, erasing mode, or erasing-verifying mode.
 * Mapped to 0xF020.
 */
typedef union
{
  H8_BITFIELD_8
  (
    /** Program */
    h8_u8 p : 1,

    /** Erase */
    h8_u8 e : 1,

    /** Program-Verify */
    h8_u8 pv : 1,

    /** Erase-Verify */
    h8_u8 ev : 1,

    /** Program Setup */
    h8_u8 psu : 1,

    /** Erase Setup */
    h8_u8 esu : 1,

    /** Software Write Enable */
    h8_u8 swe : 1,

    /** Reserved */
    h8_u8 r : 1
  ) flags;
  h8_byte_t raw;
} h8_flmcr1_t;

typedef union
{
  H8_BITFIELD_6
  (
    /**
     * RXD3/IrRXD Pin Input Data Inversion Switch (unimplemented)
     * Selects whether input data of the RXD3/IrRXD pin is inverted or not.
     */
    h8_u8 scinv0 : 1,

    h8_u8 scinv1 : 1,

    h8_u8 reserved1 : 2,

    /**
     * P32/TXD3/IrTXD Pin Function Switch
     * Selects whether pin P32/TXD3/IrTXD is used as P32 or as TXD3/IrTXD.
     */
    h8_u8 spc3 : 1,

    h8_u8 reserved2 : 1,

    h8_u8 reserved3 : 2
  ) flags;
  h8_byte_t raw;
} h8_spcr_t;
#define H8_REG_SPCR 0xFF91

typedef union
{
  H8_BITFIELD_8
  (
    /**
     * Clock Enable 0 (CKE0)
     * Selects the clock source.
     * Asynchronous mode:
     *   0: Internal baud rate generator (SCK3 pin as I/O)
     *   1: Internal baud rate generator (outputs bit rate clock on SCK3)
     * Clock synchronous mode:
     *   0: Internal clock (SCK3 as clock output)
     *   1: External clock (SCK3 as clock input)
     */
    h8_u8 cke0 : 1,

    /**
     * Clock Enable 1 (CKE1)
     * See CKE0 for description.
     */
    h8_u8 cke1 : 1,

    /**
     * Transmit End Interrupt Enable (TEIE)
     * 1: TEI3 interrupt request enabled
     * TEI3 can be released by clearing TDRE and TEND in SSR, or by clearing TEIE
     */
    h8_u8 teie : 1,

    /**
     * Reserved (MPIE)
     */
    h8_u8 mpi : 1,

    /**
     * Receive Enable (RE)
     * 1: Reception enabled
     * Clearing RE does not affect RDRF, FER, PER, OER in SSR
     */
    h8_u8 re : 1,

    /**
     * Transmit Enable (TE)
     * 1: Transmission enabled
     * When 0, TDRE bit in SSR is fixed at 1
     */
    h8_u8 te : 1,

    /**
     * Receive Interrupt Enable (RIE)
     * 1: RXI3 and ERI3 interrupt requests enabled
     * Can be released by clearing RDRF or FER, PER, OER or RIE itself
     */
    h8_u8 rie : 1,

    /**
     * Transmit Interrupt Enable (TIE)
     * 1: TXI3 interrupt request enabled
     * Can be released by clearing TDRE or TI bit
     */
    h8_u8 tie : 1
  ) flags;
  h8_byte_t raw;
} h8_scr3_t;
#define H8_REG_SCR3 0xFF9A

#define H8_REG_TDR3 0xFF9B

typedef union
{
  H8_BITFIELD_8
  (
    /**
     * Reserved
     * The write value should always be 0.
     */
    h8_u8 mpbt : 1,

    /**
     * Reserved
     * This bit is always read as 0 and cannot be modified.
     */
    h8_u8 mpbr : 1,

    /**
     * Transmit End (TEND)
     * [Setting conditions]
     *   - When the TE bit in SCR is 0
     *   - When TDRE = 1 at transmission of the last bit of a 1-byte serial
     *     transmit character
     * [Clearing conditions]
     *   - When 0 is written to TDRE after reading TDRE = 1
     *   - When the transmit data is written to TDR
     */
    h8_u8 tend : 1,

    /**
     * Parity Error (PER)
     * [Setting condition]
     *   - When a parity error is generated during reception
     * [Clearing condition]
     *   - When 0 is written to PER after reading PER = 1
     *
     * Notes:
     *   - Receive data with a parity error is still transferred to RDR,
     *     but RDRF is not set.
     *   - Reception cannot continue with PER = 1.
     *   - In clock synchronous mode, neither transmission nor reception
     *     is possible when PER = 1.
     */
    h8_u8 per : 1,

    /**
     * Framing Error (FER)
     * [Setting condition]
     *   - When a framing error occurs in reception
     * [Clearing condition]
     *   - When 0 is written to FER after reading FER = 1
     *
     * Notes:
     *   - In 2-stop-bit mode, only the first stop bit is checked.
     *   - On a framing error, data is transferred to RDR, but RDRF is not set.
     *   - Reception cannot continue with FER = 1.
     *   - In clock synchronous mode, neither transmission nor reception
     *     is possible with FER = 1.
     */
    h8_u8 fer : 1,

    /**
     * Overrun Error (OER)
     * [Setting condition]
     *   - When an overrun error occurs in reception
     * [Clearing condition]
     *   - When 0 is written to OER after reading OER = 1
     *
     * Notes:
     *   - RDR retains data from before the overrun.
     *   - Newly received data is lost.
     *   - Reception cannot continue with OER = 1.
     *   - In clock synchronous mode, transmission cannot continue either.
     */
    h8_u8 oer : 1,

    /**
     * Receive Data Register Full (RDRF)
     * [Setting condition]
     *   - When serial reception ends normally and data is moved
     *     from RSR to RDR
     * [Clearing conditions]
     *   - When 0 is written to RDRF after reading RDRF = 1
     *   - When data is read from RDR
     *
     * Notes:
     *   - If data reception completes while RDRF = 1,
     *     an overrun error (OER) occurs and new data is lost.
     */
    h8_u8 rdrf : 1,

    /**
     * Transmit Data Register Empty (TDRE)
     * [Setting conditions]
     *   - When the TE bit in SCR is 0
     *   - When data is transferred from TDR to TSR
     * [Clearing conditions]
     *   - When 0 is written to TDRE after reading TDRE = 1
     *   - When new transmit data is written to TDR
     */
    h8_u8 tdre : 1
  ) flags;
  h8_byte_t raw;
} h8_ssr3_t;
#define H8_REG_SSR3 0xFF9C

#define H8_REG_RDR3 0xFF9D

typedef union
{
  H8_BITFIELD_3
  (
    /**
     * Reserved
     * These bits are always read as 0 and cannot be modified.
     */
    h8_u8 reserved : 4,

    /**
     * IrDA Clock Select (unimplemented)
     */
    h8_u8 clock_select : 3,

    /**
     * IrDA Enable
     * Selects whether the SCI3 I/O pins function as the SCI3 or IrDA.
     */
    h8_u8 enable : 1
  ) flags;
  h8_byte_t raw;
} h8_ircr_t;
#define H8_REG_IRCR 0xFFA7

typedef struct
{
  h8_word_be_t ecpwcr;
  h8_word_be_t ecpwdr;

  h8_byte_t unknown1;

  h8_spcr_t spcr;

  h8_byte_t aegsr;
  h8_byte_t unknown2;
  h8_byte_t eccr;
  h8_byte_t eccsr;
  h8_byte_t ech;
  h8_byte_t ecl;

  h8_byte_t smr3;
  h8_byte_t brr3;
  h8_scr3_t scr3;
  h8_byte_t tdr3;
  h8_ssr3_t ssr3;
  h8_byte_t rdr3;
  h8_byte_t semr;
  h8_byte_t unknown3[8];
  h8_ircr_t ircr;
  h8_byte_t unknown4[8];
} h8_aec_sci3_t;

typedef union
{
  H8_BITFIELD_2
  (
    h8_u8 cks : 4,
    h8_u8 reserved : 4
  ) flags;
  h8_byte_t raw;
} h8_tmwd_t;

typedef union
{
  H8_BITFIELD_8
  (
    h8_u8 wrst : 1,
    h8_u8 b0wi : 1,
    h8_u8 wdon : 1,
    h8_u8 b2wi : 1,
    h8_u8 tcsrwe : 1,
    h8_u8 b4wi : 1,
    h8_u8 tcwe : 1,
    h8_u8 b6wi : 1
  ) flags;
  h8_byte_t raw;
} h8_tcsrwd1_t;

typedef struct
{
  h8_tmwd_t tmwd;
  h8_tcsrwd1_t tcsrwd1;
  h8_byte_t tcsrwd2;
  h8_byte_t tcwd;
} h8_wdt_t;

/** 15.3.1 SS Control Register H (SSCRH) */
typedef union h8_sscrh_t
{
  H8_BITFIELD_7
  (
    /** SCS Pin Select */
    h8_u8 css : 2,

    /** SSCK Pin Select */
    h8_u8 scks : 1,

    /** SOL Write Protect */
    h8_u8 solp : 1,

    /** Serial Data Output Level Setting */
    h8_u8 sol : 1,

    /** Serial Data Open-Drain Output Select */
    h8_u8 soos : 1,

    /** Bidirectional Mode Enable */
    h8_u8 bide : 1,

    /** Master/Slave Device Select */
    h8_u8 mss : 1
  ) flags;
  h8_byte_t raw;
} h8_sscrh_t;
#define H8_REG_SSCRH 0xF0E0

/** 15.3.2 SS Control Register L (SSCRL) */
typedef union h8_sscrl_t
{
  h8_byte_t raw;
} h8_sscrl_t;
#define H8_REG_SSCRL 0xF0E1

/** 15.3.3 SS Mode Register (SSMR) */
typedef union h8_ssmr_t
{
  H8_BITFIELD_7
  (
    /** Transfer clock rate select 0 */
    h8_u8 cks0 : 1,

    /** Transfer clock rate select 1 */
    h8_u8 cks1 : 1,

    /** Transfer clock rate select 2 */
    h8_u8 cks2 : 1,

    h8_u8 reserved : 2,

    /** Clock Phase Select */
    h8_u8 cphs : 1,

    /** Clock Polarity Select */
    h8_u8 cpos : 1,

    /** MSB-First/LSB-First Select */
    h8_u8 mls : 1
  ) flags;
  h8_byte_t raw;
} h8_ssmr_t;
#define H8_REG_SSMR 0xF0E2

typedef union
{
  H8_BITFIELD_8
  (
    h8_u8 ceie : 1,
    h8_u8 rie : 1,
    h8_u8 tie : 1,
    h8_u8 teie : 1,
    h8_u8 reserved : 1,
    h8_u8 rsstp : 1,
    h8_u8 re : 1,
    h8_u8 te : 1
  ) flags;
  h8_byte_t raw;
} h8_sser_t;
#define H8_REG_SSER 0xF0E3

typedef union h8_sssr_t
{
  H8_BITFIELD_8
  (
    /** Conflict Error Flag */
    h8_u8 ce : 1,

    /** Receive Data Register Full */
    h8_u8 rdrf : 1,

    /** Transmit Data Empty */
    h8_u8 tdre : 1,

    /** Transmit End */
    h8_u8 tend : 1,

    h8_u8 reserved1 : 1,
    h8_u8 reserved2 : 1,

    /** Overrun Error Flag */
    h8_u8 orer : 1,

    h8_u8 reserved3 : 1
  ) flags;
  h8_byte_t raw;
} h8_sssr_t;
#define H8_REG_SSSR 0xF0E4

/** @todo Where is SSTRSR? */
typedef struct
{
  h8_sscrh_t sscrh;
  h8_sscrl_t sscrl;
  h8_ssmr_t ssmr;
  h8_sser_t sser;
  h8_sssr_t sssr;
  h8_byte_t unused[4];
  h8_byte_t ssrdr;
  h8_byte_t unused2;
  h8_byte_t sstdr;
  h8_byte_t unused3[4];
} h8_ssu_t;

/** @todo Timer W */
typedef struct
{
  h8_byte_t tmrw;
  h8_byte_t tcrw;
  h8_byte_t tierw;
  h8_byte_t tsrw;
  h8_byte_t tior0;
  h8_byte_t tior1;
  h8_word_be_t tcnt;
  h8_word_be_t gra;
  h8_word_be_t grb;
  h8_word_be_t grc;
  h8_word_be_t grd;
} h8_tw_t;

/**
 * 17.3.1 A/D Result Register (ADRR)
 * Mapped to FFBC / FFBD
 */
typedef union h8_adrr_t
{
  h8_word_be_t raw;
} h8_adrr_t;

typedef enum
{
  H8_ADC_AN0 = B0100,
  H8_ADC_AN1,
  H8_ADC_AN2,
  H8_ADC_AN3,
  H8_ADC_AN4,
  H8_ADC_AN5,

  H8_ADC_MAX
} h8_adc_channel;

/**
 * 7.3.2 A/D Mode Register (AMR)
 * Mapped to FFBE
 */
typedef union
{
  H8_BITFIELD_4
  (
    /** Channel Select */
    h8_u8 ch : 4,

    /** Clock Select (unimplemented) */
    h8_u8 cks : 2,

    /** External Trigger Select */
    h8_u8 trge : 1,

    h8_u8 reserved : 1
  ) flags;
  h8_byte_t raw;
} h8_amr_t;

/**
 * 7.3.3 A/D Start Register (ADSR)
 * Mapped to FFBF
 */
typedef union
{
  H8_BITFIELD_3
  (
    h8_u8 reserved : 6,

    /** Ladder Resistance Select */
    h8_u8 lads : 1,

    /**
     * A/D Start/Finish? Set to 1 to start, then it sets itself to 0 when
     * finished
     */
    h8_u8 adsf : 1
  ) flags;
  h8_byte_t raw;
} h8_adsr_t;

typedef struct
{
  h8_adrr_t adrr;
  h8_amr_t amr;
  h8_adsr_t adsr;
} h8_adc_t;

#endif
