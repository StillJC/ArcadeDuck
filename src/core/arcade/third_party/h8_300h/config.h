#ifndef H8_CONFIG_H
#define H8_CONFIG_H

#ifndef H8_BIG_ENDIAN
/**
 * Whether or not the target compilation platform uses big-endian byte order
 * Swaps the layout of some data structures
 */
#define H8_BIG_ENDIAN 0
#endif

#ifndef H8_LOGGER_DEFAULT_LEVEL
/**
 * The default severity level the logger will process
 */
#define H8_LOGGER_DEFAULT_LEVEL 3
#endif

#ifndef H8_NO_DMA
#define H8_NO_DMA 0
#endif

#ifndef H8_SAFETY
/**
 * Enables some additional error handling and bounds checking for situations
 * that should never happen
 */
#define H8_SAFETY 0
#endif

#ifndef H8_PROFILING
#define H8_PROFILING 1
#endif

#ifndef H8_REVERSE_BITFIELDS
/**
 * Reverses bitfields for compilers where most significant bit is defined first
 */
#define H8_REVERSE_BITFIELDS 0
#endif

#ifndef H8_TESTS
#define H8_TESTS 0
#endif

#ifndef H8_HAVE_NETWORK_IMPL
/**
 * Whether or not to use the default network implementation
 * Set to 1 to use networking without additional code.
 * Set to 0 if you provide your own implementation, or if you do not use
 * networking at all.
 */
#define H8_HAVE_NETWORK_IMPL 1
#endif

#endif
