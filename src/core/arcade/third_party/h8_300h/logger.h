#ifndef H8_LOGGER_H
#define H8_LOGGER_H

typedef enum
{
  H8_LOG_SOURCE_INVALID = 0,

  H8_LOG_CPU,
  H8_LOG_LCD,
  H8_LOG_EEP,
  H8_LOG_SSU,
  H8_LOG_IR,

  H8_LOG_SOURCE_SIZE
} h8_log_source;

typedef enum
{
  H8_LOG_LEVEL_INVALID = 0,

  H8_LOG_DEBUG,
  H8_LOG_INFO,
  H8_LOG_WARN,
  H8_LOG_ERROR,

  H8_LOG_LEVEL_SIZE
} h8_log_level;

void h8_log(h8_log_level level, h8_log_source source, const char *fmt, ...);

#endif
