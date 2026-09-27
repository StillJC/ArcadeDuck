#ifndef H8_IR_H
#define H8_IR_H

#include "types.h"

#define H8_IR_BUFFER_LEN 8

typedef struct
{
  h8_byte_t rx[H8_IR_BUFFER_LEN];
  unsigned rx_len;
  h8_byte_t tx[H8_IR_BUFFER_LEN];
  unsigned tx_len;
} h8_ir_t;

h8_bool h8_ir_in(h8_ir_t *ir, h8_byte_t *value);

/**
 * Outputs one byte to the TX buffer
 */
h8_bool h8_ir_out(h8_ir_t *ir, h8_byte_t out);

void h8_ir_receive(h8_ir_t *ir);

/**
 * Transmit the finalized TX buffer via TCP/IP
 * Should be called when the TE bit in SCR is cleared to 0 (?)
 * Or maybe when TEND flag is read
 */
void h8_ir_transmit(h8_ir_t *ir);

#endif
