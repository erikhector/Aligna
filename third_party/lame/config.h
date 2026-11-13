// Hand-written replacement for LAME's autotools config.h. Encoder-only static build:
// no mpglib (HAVE_MPGLIB undefined), no SIMD helper (HAVE_XMMINTRIN_H undefined).
#ifndef ALIGNA_LAME_CONFIG_H
#define ALIGNA_LAME_CONFIG_H

#define PACKAGE "lame"
#define PACKAGE_VERSION "3.100"
#define STDC_HEADERS 1
#define HAVE_STDINT_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1

#ifndef _MSC_VER
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1
#endif

#endif
