#ifndef UNIFROG_RA_MD5_H
#define UNIFROG_RA_MD5_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hex MD5 of a byte buffer. `out` must hold 33 bytes. */
void ra_md5_hex(const void *data, size_t size, char out[33]);

#ifdef __cplusplus
}
#endif

#endif
