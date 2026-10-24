#include "ir.h"
#include "logger.h"

#include <stdarg.h>

void h8_log(h8_log_level level, h8_log_source source, const char *fmt, ...)
{
  (void)level;
  (void)source;
  (void)fmt;
}

h8_bool h8_ir_out(h8_ir_t *ir, h8_byte_t out)
{
  (void)ir;
  (void)out;
  return TRUE;
}

h8_bool h8_ir_in(h8_ir_t *ir, h8_byte_t *value)
{
  (void)ir;
  (void)value;
  return FALSE;
}

void h8_ir_receive(h8_ir_t *ir)
{
  (void)ir;
}

void h8_ir_transmit(h8_ir_t *ir)
{
  (void)ir;
}