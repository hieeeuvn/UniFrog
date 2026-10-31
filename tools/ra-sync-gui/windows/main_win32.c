/*
 * UniFrog RA Sync for Windows.
 *
 * A small Win32 front end around the shared sync core. The worker thread does
 * the network and card I/O; the UI thread only drains log lines through a
 * timer so the window stays responsive.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/ra_sync_core.h"
#include "ra_fs_win32.h"
#include "ra_http_winhttp.h"

#define IDC_USER 1001
#define IDC_KEY 1002
#define IDC_FOLDER 1003
#define IDC_BROWSE 1004
#define IDC_SYNC 1005
#define IDC_PULL 1006
#define IDC_PUSH 1007
#define IDC_LIST 1008
#define IDC_LOG 1009
#define IDC_STATUS 1010
#define IDC_HINT 1011
#define IDC_PASSWORD 1012
#define TIMER_DRAIN 1
#define WM_JOB_DONE (WM_APP + 1)

#define WINDOW_WIDTH 760
#define WINDOW_HEIGHT 600

struct job {
   char user[128];
   char key[256];
   char password[128];
   char token[RA_SYNC_TOKEN_MAX];
   char root[MAX_PATH];
   int pull;
   int push;
   int list;
   int rc;
   struct ra_sync_stats stats;
   CRITICAL_SECTION lock;
   char *pending;
   size_t pending_length;
   size_t pending_capacity;
   int running;
   int finished;
   HANDLE thread;
};

static struct job current;
static HWND main_window;
static HFONT log_font;
static char settings_path[MAX_PATH];

static void job_log(void *userdata, const char *line)
{
   struct job *job = (struct job *)userdata;
   size_t length = line ? strlen(line) : 0;
   size_t needed;

   if (!job || length == 0)
      return;
   EnterCriticalSection(&job->lock);
   needed = job->pending_length + length + 2u;
   if (needed > job->pending_capacity) {
      size_t capacity = job->pending_capacity ? job->pending_capacity * 2u :
         4096u;
      char *grown;

      while (capacity < needed)
         capacity *= 2u;
      grown = (char *)realloc(job->pending, capacity);
      if (!grown) {
         LeaveCriticalSection(&job->lock);
         return;
      }
      job->pending = grown;
      job->pending_capacity = capacity;
   }
   memcpy(job->pending + job->pending_length, line, length);
   job->pending_length += length;
   job->pending[job->pending_length++] = '\n';
   job->pending[job->pending_length] = '\0';
   LeaveCriticalSection(&job->lock);
}

static void append_log(const char *text)
{
   HWND log = GetDlgItem(main_window, IDC_LOG);
   int length = GetWindowTextLengthA(log);

   SendMessageA(log, EM_SETSEL, (WPARAM)length, (LPARAM)length);
   SendMessageA(log, EM_REPLACESEL, FALSE, (LPARAM)(text ? text : ""));
   SendMessageA(log, EM_SCROLLCARET, 0, 0);
}

static void drain_log(void)
{
   char *chunk;
   size_t length;

   EnterCriticalSection(&current.lock);
   chunk = current.pending;
   length = current.pending_length;
   current.pending = NULL;
   current.pending_length = 0;
   current.pending_capacity = 0;
   LeaveCriticalSection(&current.lock);

   if (chunk && length > 0) {
      chunk[length] = '\0';
      append_log(chunk);
   }
   free(chunk);
}

static void load_settings(void)
{
   char appdata[MAX_PATH];
   char directory[MAX_PATH];

   settings_path[0] = '\0';
   if (FAILED(SHGetFolderPathA(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL,
       0, appdata)))
      return;
   snprintf(directory, sizeof(directory), "%s\\UniFrogRASync", appdata);
   CreateDirectoryA(directory, NULL);
   snprintf(settings_path, sizeof(settings_path), "%s\\settings.ini",
      directory);
}

static void read_settings(void)
{
   char user[128] = "";
   char key[256] = "";
   char root[MAX_PATH] = "";

   load_settings();
   if (!settings_path[0])
      return;
   GetPrivateProfileStringA("sync", "user", "", user, sizeof(user),
      settings_path);
   GetPrivateProfileStringA("sync", "key", "", key, sizeof(key),
      settings_path);
   GetPrivateProfileStringA("sync", "root", "", root, sizeof(root),
      settings_path);
   SetDlgItemTextA(main_window, IDC_USER, user);
   SetDlgItemTextA(main_window, IDC_KEY, key);
   SetDlgItemTextA(main_window, IDC_FOLDER, root);
}

static void write_settings(void)
{
   char user[128];
   char key[256];
   char root[MAX_PATH];

   if (!settings_path[0])
      return;
   GetDlgItemTextA(main_window, IDC_USER, user, sizeof(user));
   GetDlgItemTextA(main_window, IDC_KEY, key, sizeof(key));
   GetDlgItemTextA(main_window, IDC_FOLDER, root, sizeof(root));
   WritePrivateProfileStringA("sync", "user", user, settings_path);
   WritePrivateProfileStringA("sync", "key", key, settings_path);
   WritePrivateProfileStringA("sync", "root", root, settings_path);
}

static void set_running_controls(int running)
{
   EnableWindow(GetDlgItem(main_window, IDC_SYNC), !running);
   EnableWindow(GetDlgItem(main_window, IDC_PULL), !running);
   EnableWindow(GetDlgItem(main_window, IDC_PUSH), !running);
   EnableWindow(GetDlgItem(main_window, IDC_LIST), !running);
   EnableWindow(GetDlgItem(main_window, IDC_BROWSE), !running);
}

static DWORD WINAPI sync_thread(LPVOID parameter)
{
   struct job *job = (struct job *)parameter;
   struct ra_sync_options options;
   ra_sync_http_t http;
   char resolved[MAX_PATH];

   if (ra_fs_win32_resolve_root(job->root, resolved, sizeof(resolved)) != 0) {
      job_log(job, "Pick the SD card folder that contains unifrog_data");
      job->rc = -1;
      snprintf(job->stats.error, sizeof(job->stats.error),
         "No card folder selected");
   } else if (ra_fs_win32_init(job->root) != 0) {
      job_log(job, "Could not open the selected folder");
      job->rc = -1;
   } else {
      memset(&options, 0, sizeof(options));
      options.user = job->user;
      options.key = job->key;
      options.password = job->password;
      options.token_out = job->token;
      options.token_out_size = sizeof(job->token);
      options.pull = job->pull;
      options.push = job->push;
      options.list = job->list;
      http.userdata = NULL;
      http.post = ra_http_winhttp_post;
      job->rc = ra_sync_run(&http, &ra_fs_win32, &options, job_log, job,
         &job->stats);
   }

   EnterCriticalSection(&job->lock);
   job->finished = 1;
   LeaveCriticalSection(&job->lock);
   PostMessageA(main_window, WM_JOB_DONE, 0, 0);
   return 0;
}

static void start_sync(int pull, int push, int list)
{
   char root[MAX_PATH];

   if (current.running)
      return;
   GetDlgItemTextA(main_window, IDC_FOLDER, root, sizeof(root));
   if (!root[0]) {
      MessageBoxA(main_window,
         "Select the SD card folder that contains unifrog_data first.",
         "UniFrog RA Sync", MB_ICONINFORMATION | MB_OK);
      return;
   }
   write_settings();
   memset(&current, 0, sizeof(current));
   InitializeCriticalSection(&current.lock);
   GetDlgItemTextA(main_window, IDC_USER, current.user, sizeof(current.user));
   GetDlgItemTextA(main_window, IDC_KEY, current.key, sizeof(current.key));
   GetDlgItemTextA(main_window, IDC_PASSWORD, current.password,
      sizeof(current.password));
   snprintf(current.root, sizeof(current.root), "%s", root);
   current.pull = pull;
   current.push = push;
   current.list = list;
   current.running = 1;
   SetDlgItemTextA(main_window, IDC_STATUS, "Working...");
   append_log("\r\n");
   set_running_controls(1);
   current.thread = CreateThread(NULL, 0, sync_thread, &current, 0, NULL);
   if (!current.thread) {
      current.running = 0;
      set_running_controls(0);
      SetDlgItemTextA(main_window, IDC_STATUS, "Could not start the worker");
      DeleteCriticalSection(&current.lock);
      return;
   }
   SetTimer(main_window, TIMER_DRAIN, 120, NULL);
}

static void finish_sync(void)
{
   char summary[256];
   char stats_line[256];

   drain_log();
   KillTimer(main_window, TIMER_DRAIN);
   if (current.thread) {
      WaitForSingleObject(current.thread, 2000);
      CloseHandle(current.thread);
      current.thread = NULL;
   }
   if (current.rc != 0) {
      snprintf(summary, sizeof(summary), "%s",
         current.stats.error[0] ? current.stats.error : "Sync failed");
   } else if (current.stats.awarded || current.stats.failed ||
       current.stats.cached || current.stats.unknown) {
      snprintf(summary, sizeof(summary),
         "Done: cached %d, unknown %d, awarded %d, failed %d, pending %d",
         current.stats.cached, current.stats.unknown, current.stats.awarded,
         current.stats.failed, current.stats.pending);
   } else {
      snprintf(summary, sizeof(summary), "Done");
   }
   SetDlgItemTextA(main_window, IDC_STATUS, summary);
   snprintf(stats_line, sizeof(stats_line),
      "Games cached: %d   Unknown: %d   Awards sent: %d   Pending: %d",
      current.stats.cached, current.stats.unknown, current.stats.awarded,
      current.stats.pending);
   SetDlgItemTextA(main_window, IDC_HINT, stats_line);
   if (current.token[0]) {
      /* Keep the Connect token so the password field can stay empty. */
      SetDlgItemTextA(main_window, IDC_KEY, current.token);
      write_settings();
   }
   current.running = 0;
   set_running_controls(0);
   DeleteCriticalSection(&current.lock);
}

static void browse_folder(void)
{
   BROWSEINFOA info;
   LPITEMIDLIST item;
   char path[MAX_PATH];

   memset(&info, 0, sizeof(info));
   info.hwndOwner = main_window;
   info.lpszTitle = "Select unifrog_data on the SD card";
   info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
   item = SHBrowseForFolderA(&info);
   if (!item)
      return;
   if (SHGetPathFromIDListA(item, path))
      SetDlgItemTextA(main_window, IDC_FOLDER, path);
   CoTaskMemFree(item);
}

static HWND make_control(const char *class_name, const char *text, DWORD style,
   int x, int y, int width, int height, int id)
{
   HWND control = CreateWindowExA(0, class_name, text,
      WS_CHILD | WS_VISIBLE | style, x, y, width, height, main_window,
      (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);

   return control;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
   LPARAM lparam)
{
   switch (message) {
   case WM_CREATE: {
      main_window = window;

      make_control("STATIC", "Username", SS_LEFT, 16, 18, 90, 20, 0);
      make_control("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 110, 15, 220, 24,
         IDC_USER);
      make_control("STATIC", "Connect token", SS_LEFT, 16, 50, 90, 20, 0);
      make_control("EDIT", "", WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL, 110,
         47, 220, 24, IDC_KEY);
      make_control("STATIC", "RA password", SS_LEFT, 344, 50, 90, 20, 0);
      make_control("EDIT", "", WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL, 438,
         47, 292, 24, IDC_PASSWORD);
      make_control("STATIC", "SD folder", SS_LEFT, 16, 82, 90, 20, 0);
      make_control("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 110, 79, 520, 24,
         IDC_FOLDER);
      make_control("BUTTON", "Browse...", BS_PUSHBUTTON, 640, 78, 90, 26,
         IDC_BROWSE);
      make_control("BUTTON", "Sync", BS_DEFPUSHBUTTON, 110, 118, 90, 30,
         IDC_SYNC);
      make_control("BUTTON", "Pull definitions", BS_PUSHBUTTON, 208, 118, 130,
         30, IDC_PULL);
      make_control("BUTTON", "Push unlocks", BS_PUSHBUTTON, 346, 118, 110, 30,
         IDC_PUSH);
      make_control("BUTTON", "List card", BS_PUSHBUTTON, 464, 118, 100, 30,
         IDC_LIST);
      make_control("STATIC", "", SS_LEFT, 110, 154, 620, 20, IDC_STATUS);
      make_control("STATIC", "", SS_LEFT, 16, 178, 720, 20, IDC_HINT);
      {
         HWND log = make_control("EDIT", "", WS_BORDER | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | ES_NOHIDESEL,
            16, 202, 714, 344, IDC_LOG);
         LOGFONTA log_font_info;

         memset(&log_font_info, 0, sizeof(log_font_info));
         log_font_info.lfHeight = -13;
         log_font_info.lfWeight = FW_NORMAL;
         log_font_info.lfCharSet = ANSI_CHARSET;
         snprintf(log_font_info.lfFaceName, sizeof(log_font_info.lfFaceName),
            "Consolas");
         log_font = CreateFontIndirectA(&log_font_info);
         if (log_font)
            SendMessageA(log, WM_SETFONT, (WPARAM)log_font, TRUE);
      }
      read_settings();
      SetDlgItemTextA(window, IDC_STATUS,
         "Ready. Select the card, then press Sync.");
      return 0;
   }
   case WM_COMMAND:
      switch (LOWORD(wparam)) {
      case IDC_BROWSE:
         browse_folder();
         return 0;
      case IDC_SYNC:
         start_sync(1, 1, 0);
         return 0;
      case IDC_PULL:
         start_sync(1, 0, 0);
         return 0;
      case IDC_PUSH:
         start_sync(0, 1, 0);
         return 0;
      case IDC_LIST:
         start_sync(0, 0, 1);
         return 0;
      default:
         break;
      }
      break;
   case WM_TIMER:
      if (wparam == TIMER_DRAIN)
         drain_log();
      return 0;
   case WM_JOB_DONE:
      finish_sync();
      return 0;
   case WM_CLOSE:
      if (current.running) {
         MessageBoxA(window, "A sync is still running. Wait for it to finish.",
            "UniFrog RA Sync", MB_ICONINFORMATION | MB_OK);
         return 0;
      }
      DestroyWindow(window);
      return 0;
   case WM_DESTROY:
      if (log_font)
         DeleteObject(log_font);
      PostQuitMessage(0);
      return 0;
   default:
      break;
   }
   return DefWindowProcA(window, message, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line,
   int show_command)
{
   WNDCLASSEXA window_class;
   MSG message;

   (void)previous;
   (void)command_line;
   CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
   memset(&window_class, 0, sizeof(window_class));
   window_class.cbSize = sizeof(window_class);
   window_class.lpfnWndProc = window_proc;
   window_class.hInstance = instance;
   window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
   window_class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
   window_class.lpszClassName = "UniFrogRASyncWindow";
   window_class.hIcon = LoadIcon(NULL, IDI_APPLICATION);
   if (!RegisterClassExA(&window_class)) {
      MessageBoxA(NULL, "Could not create the window class.", "UniFrog RA Sync",
         MB_ICONERROR | MB_OK);
      return 1;
   }
   main_window = CreateWindowExA(0, window_class.lpszClassName,
      "UniFrog RA Sync", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
      WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, WINDOW_WIDTH,
      WINDOW_HEIGHT, NULL, NULL, instance, NULL);
   if (!main_window) {
      MessageBoxA(NULL, "Could not create the window.", "UniFrog RA Sync",
         MB_ICONERROR | MB_OK);
      return 1;
   }
   ShowWindow(main_window, show_command);
   UpdateWindow(main_window);
   while (GetMessageA(&message, NULL, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageA(&message);
   }
   CoUninitialize();
   return (int)message.wParam;
}
