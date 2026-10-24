/*
 * WinHTTP transport for the sync core.
 *
 * One POST per call keeps the code small and is fine for the handful of
 * requests a sync makes. WinHTTP validates the RetroAchievements certificate
 * chain through the normal Windows trust store.
 */
#include "ra_http_winhttp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winhttp.h>

#include "../core/ra_sync_core.h"

#define RA_HTTP_CHUNK 16384

static void set_error(char *error, size_t size, const char *text)
{
   if (!error || size == 0)
      return;
   snprintf(error, size, "%s", text ? text : "network error");
}

static void set_error_code(char *error, size_t size, const char *prefix,
   DWORD code)
{
   if (!error || size == 0)
      return;
   snprintf(error, size, "%s (winhttp error %lu)", prefix,
      (unsigned long)code);
}

static int read_response(HINTERNET request, char **response, char *error,
   size_t error_size)
{
   struct ra_buf {
      char *data;
      size_t length;
      size_t capacity;
   } buffer;
   DWORD available = 0;

   memset(&buffer, 0, sizeof(buffer));
   for (;;) {
      DWORD received = 0;
      char chunk[RA_HTTP_CHUNK + 1];

      if (!WinHttpReadData(request, chunk, RA_HTTP_CHUNK, &received)) {
         set_error_code(error, error_size, "Reading the response failed",
            GetLastError());
         free(buffer.data);
         return -1;
      }
      if (received == 0)
         break;
      chunk[received] = '\0';
      if (buffer.length + received + 1u > buffer.capacity) {
         size_t capacity = buffer.capacity ? buffer.capacity * 2u : 4096u;
         char *grown;

         while (capacity < buffer.length + received + 1u)
            capacity *= 2u;
         grown = (char *)realloc(buffer.data, capacity);
         if (!grown) {
            set_error(error, error_size, "Out of memory for the response");
            free(buffer.data);
            return -1;
         }
         buffer.data = grown;
         buffer.capacity = capacity;
      }
      memcpy(buffer.data + buffer.length, chunk, received);
      buffer.length += received;
      buffer.data[buffer.length] = '\0';
   }
   (void)available;
   if (!buffer.data) {
      buffer.data = (char *)malloc(1);
      if (!buffer.data) {
         set_error(error, error_size, "Out of memory for the response");
         return -1;
      }
      buffer.data[0] = '\0';
   }
   *response = buffer.data;
   return 0;
}

int ra_http_winhttp_post(void *userdata, const char *body, char **response,
   char *error, size_t error_size)
{
   static const wchar_t *url = L"https://retroachievements.org/dorequest.php";
   static const wchar_t *agent = L"UniFrog-RA-Sync/1.0";
   HINTERNET session = NULL;
   HINTERNET connection = NULL;
   HINTERNET request = NULL;
   URL_COMPONENTS parts;
   wchar_t host[128];
   wchar_t path[256];
   DWORD status = 0;
   DWORD status_size = sizeof(status);
   DWORD body_size = (DWORD)strlen(body ? body : "");
   int result = -1;

   (void)userdata;
   *response = NULL;

   memset(&parts, 0, sizeof(parts));
   parts.dwStructSize = sizeof(parts);
   parts.lpszHostName = host;
   parts.dwHostNameLength = (DWORD)(sizeof(host) / sizeof(host[0]));
   parts.lpszUrlPath = path;
   parts.dwUrlPathLength = (DWORD)(sizeof(path) / sizeof(path[0]));
   if (!WinHttpCrackUrl(url, 0, 0, &parts)) {
      set_error_code(error, error_size, "Could not parse the API URL",
         GetLastError());
      return -1;
   }

   session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
   if (!session) {
      set_error_code(error, error_size, "Could not start WinHTTP",
         GetLastError());
      goto done;
   }
   (void)WinHttpSetTimeouts(session, 20000, 20000, 20000, 20000);

   connection = WinHttpConnect(session, host, parts.nPort, 0);
   if (!connection) {
      set_error_code(error, error_size, "Could not connect to the server",
         GetLastError());
      goto done;
   }
   request = WinHttpOpenRequest(connection, L"POST", path, NULL,
      WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
   if (!request) {
      set_error_code(error, error_size, "Could not create the request",
         GetLastError());
      goto done;
   }

   {
      static const wchar_t *headers =
         L"Content-Type: application/x-www-form-urlencoded\r\n"
         L"Accept: application/json\r\n";

      if (!WinHttpSendRequest(request, headers, (DWORD)-1L,
          (LPVOID)body, body_size, body_size, 0)) {
         set_error_code(error, error_size, "Could not send the request",
            GetLastError());
         goto done;
      }
   }
   if (!WinHttpReceiveResponse(request, NULL)) {
      set_error_code(error, error_size, "No response from the server",
         GetLastError());
      goto done;
   }
   if (!WinHttpQueryHeaders(request,
       WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
       WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
       WINHTTP_NO_HEADER_INDEX)) {
      set_error_code(error, error_size, "Could not read the HTTP status",
         GetLastError());
      goto done;
   }
   if (status < 200 || status >= 300) {
      snprintf(error, error_size, "server returned HTTP %lu",
         (unsigned long)status);
      /* Still read the body so a JSON error message can be shown. */
   }
   if (read_response(request, response, error, error_size) != 0)
      goto done;
   result = 0;

done:
   if (request)
      WinHttpCloseHandle(request);
   if (connection)
      WinHttpCloseHandle(connection);
   if (session)
      WinHttpCloseHandle(session);
   return result;
}
