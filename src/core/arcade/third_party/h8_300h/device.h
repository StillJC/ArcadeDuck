#ifndef H8_DEVICE_H
#define H8_DEVICE_H

#include "types.h"

/**
 * An enumeration of unique identifiers for any plugged-in devices that can be
 * emulated
 */
typedef enum
{
  H8_DEVICE_INVALID = 0,

  /* A device using only generic implementations */
  H8_DEVICE_GENERIC,

  /* 8KB EEPROM device */
  H8_DEVICE_EEPROM_8K,

  /* 64KB EEPROM device */
  H8_DEVICE_EEPROM_64K,

  /* Bosch Sensortec - Digital, triaxial acceleration sensor */
  H8_DEVICE_BMA150,

  /* A 4-color 128x64 LCD */
  H8_DEVICE_LCD,

  /* An LED that can light either red or green */
  H8_DEVICE_LED,

  /* The SPI bus connector on the NTR-031 cartridge */
  H8_DEVICE_SPI_BUS,

  /* A simple sound output device */
  H8_DEVICE_BUZZER,

  /* A single-button input device */
  H8_DEVICE_1BUTTON,

  /* A three-button input device */
  H8_DEVICE_3BUTTON,

  /* A factory test controller that can tell the device to check itself */
  H8_DEVICE_FACTORY_CONTROL,

  /* A controller to setup IrDA */
  H8_DEVICE_IRDA_CONTROL,

  /* A 2-axis analog accelerometer */
  H8_DEVICE_ACCELEROMETER_X,
  H8_DEVICE_ACCELEROMETER_Y,

  /* A generic device that represents battery level */
  H8_DEVICE_BATTERY,

  H8_DEVICE_SIZE
} h8_device_id;

/**
 * An enumeration of unique identifiers for preset systems that can be
 * configured to contain all necessary devices
 */
typedef enum
{
  H8_SYSTEM_INVALID = 0,

  H8_SYSTEM_NTR_027,
  H8_SYSTEM_NTR_031,
  H8_SYSTEM_NTR_032,

  H8_SYSTEM_SIZE
} h8_system_id;

/**
 * An enumeration of IO ports devices can be hooked up to
 */
typedef enum
{
  H8_HOOKUP_PORT_INVALID = 0,

  H8_HOOKUP_PORT_1,
  H8_HOOKUP_PORT_3,
  H8_HOOKUP_PORT_8,
  H8_HOOKUP_PORT_9,
  H8_HOOKUP_PORT_B,

  H8_HOOKUP_PORT_SIZE
} h8_hookup_port;

struct h8_device_t;

/**
 * The function used to initialize and allocate data for a device on the system
 */
typedef void H8D_OP_INIT_T(struct h8_device_t*);

/**
 * The function used to deinitialize and free data for a device on the system
 */
typedef void H8D_OP_FREE_T(struct h8_device_t*);

/**
 * The function used to save the state of a device on the system.
 * If NULL, this device does not support savestates.
 */
typedef h8_bool H8D_OP_SAVE_T(const struct h8_device_t*, h8_u8**, unsigned*);

/**
 * The function used to load the state of a device on the system.
 * If NULL, this device does not support savestates.
 */
typedef h8_bool H8D_OP_LOAD_T(struct h8_device_t*, const h8_u8**, unsigned*);

typedef h8_bool H8D_OP_PDR_IN_T(struct h8_device_t*);
typedef void H8D_OP_PDR_OUT_T(struct h8_device_t*, const h8_bool);
typedef void H8D_OP_SSU_IN_T(struct h8_device_t*, h8_byte_t*);
typedef void H8D_OP_SSU_OUT_T(struct h8_device_t*, h8_byte_t*, h8_byte_t);
typedef h8_word_t H8D_OP_ADRR_T(struct h8_device_t*);

typedef struct h8_device_t
{
  void *device;

  h8_device_id type;
  const char *name;
  h8_hookup_port port;

  /** A pointer to general device data without any additional state */
  void *data;

  /** The size, in bytes, of what `data` points to */
  unsigned size;

  H8D_OP_INIT_T *init;

  /**
   * A function to be called when the SSU requests data from a device. This
   * function is polled for every SSU-enabled device when data is to be read,
   * and each implementation should handle its own chip select.
   */
  H8D_OP_SSU_IN_T *ssu_in;

  /**
   * A function to be called when the SSU writes data to a device. This
   * function is polled for every SSU-enabled device when data is to be written,
   * and each implementation should handle its own chip select.
   */
  H8D_OP_SSU_OUT_T *ssu_out;

  /**
   * PDR1: 3 pins - 0, 1, 2
   * PDR3: 3 pins - 0, 1, 2
   * PDR8: 3 pins - 2, 3, 4
   * PDR9: 4 pins - 0, 1, 2, 3
   * PDRB: 6 pins - 0, 1, 2, 3, 4, 5
   */

  H8D_OP_PDR_IN_T *pdr_ins[6];

  H8D_OP_PDR_OUT_T *pdr_outs[6];

  /**
   * A function to be called to serialize the device state
   */
  H8D_OP_SAVE_T *save;

  /**
   * A function to be called to load the device state from serialized data
   */
  H8D_OP_LOAD_T *load;
} h8_device_t;

/** An arbitrary maximum for how many ROMs are associated with a system */
#define H8_CRC32_MAX 4

/** An arbitrary maximum for how many port associations can be created */
#define H8_HOOKUP_MAX 32

typedef struct h8_adc_hookup_t
{
  /* An identifier specifying which of the implemented devices this is */
  h8_device_id type;

  /* The A/DC channel this hookup uses */
  unsigned channel;

  H8D_OP_ADRR_T *func;
} h8_adc_hookup_t;

typedef struct h8_pdr_hookup_t
{
  /* An identifier specifying which of the implemented devices this is */
  h8_device_id type;

  /* The port this hookup takes place in */
  h8_hookup_port port;

  H8D_OP_PDR_IN_T *pdr_ins[6];

  H8D_OP_PDR_OUT_T *pdr_outs[6];
} h8_pdr_hookup_t;

typedef struct h8_system_preset_t
{
  const char *title;
  h8_system_id system;
  unsigned crc32[H8_CRC32_MAX];
  h8_adc_hookup_t adc_hookups[6];
  h8_pdr_hookup_t pdr_hookups[H8_HOOKUP_MAX];
} h8_system_preset_t;

#endif
