#ifndef UNIFROG_RA_HTTP_WINHTTP_H
#define UNIFROG_RA_HTTP_WINHTTP_H

#include <stddef.h>

int ra_http_winhttp_post(void *userdata, const char *body, char **response,
   char *error, size_t error_size);

#endif
