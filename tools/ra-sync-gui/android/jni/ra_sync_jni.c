/*
 * JNI glue between the Android GUI and the shared C sync core.
 *
 * Only two things happen here: Java strings/arrays are converted to C, and the
 * core callbacks are forwarded to the SyncBridge object. All protocol logic
 * stays in core/ra_sync_core.c, which the Windows build and the host tests use
 * unchanged.
 */
#include <jni.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra_sync_core.h"

struct bridge {
   JNIEnv *env;
   jobject object;
   jmethodID exists;
   jmethodID list;
   jmethodID read;
   jmethodID write;
   jmethodID remove;
   jmethodID rename;
   jmethodID post;
   jmethodID last_error;
   jmethodID log;
   jmethodID status;
};

static char *copy_to_c(struct bridge *bridge, jstring value);

static int bridge_ok(struct bridge *bridge)
{
   if ((*bridge->env)->ExceptionCheck(bridge->env)) {
      (*bridge->env)->ExceptionClear(bridge->env);
      return 0;
   }
   return 1;
}

static jstring bridge_string(struct bridge *bridge, const char *text)
{
   return (*bridge->env)->NewStringUTF(bridge->env, text ? text : "");
}

static void bridge_log(struct bridge *bridge, const char *text)
{
   jstring value = bridge_string(bridge, text);

   if (!value)
      return;
   (*bridge->env)->CallVoidMethod(bridge->env, bridge->object, bridge->log,
      value);
   (*bridge->env)->DeleteLocalRef(bridge->env, value);
   (void)bridge_ok(bridge);
}

static char *copy_to_c(struct bridge *bridge, jstring value)
{
   const char *chars;
   char *copy;

   if (!value)
      return NULL;
   chars = (*bridge->env)->GetStringUTFChars(bridge->env, value, NULL);
   if (!chars)
      return NULL;
   copy = (char *)malloc(strlen(chars) + 1u);
   if (copy)
      memcpy(copy, chars, strlen(chars) + 1u);
   (*bridge->env)->ReleaseStringUTFChars(bridge->env, value, chars);
   return copy;
}

static int fs_exists(void *userdata, const char *path)
{
   struct bridge *bridge = (struct bridge *)userdata;
   jstring value = bridge_string(bridge, path);
   jboolean result;
   int exists = 0;

   if (!value)
      return 0;
   result = (*bridge->env)->CallBooleanMethod(bridge->env, bridge->object,
      bridge->exists, value);
   (*bridge->env)->DeleteLocalRef(bridge->env, value);
   if (bridge_ok(bridge))
      exists = result ? 1 : 0;
   return exists;
}

static int fs_list(void *userdata, const char *dir,
   int (*callback)(void *ud, const char *name), void *callback_ud)
{
   struct bridge *bridge = (struct bridge *)userdata;
   jstring value = bridge_string(bridge, dir);
   jobjectArray names;
   jsize count;
   jsize index;
   int result = 0;

   if (!value)
      return -1;
   names = (jobjectArray)(*bridge->env)->CallObjectMethod(bridge->env,
      bridge->object, bridge->list, value);
   (*bridge->env)->DeleteLocalRef(bridge->env, value);
   if (!bridge_ok(bridge) || !names)
      return -1;
   count = (*bridge->env)->GetArrayLength(bridge->env, names);
   for (index = 0; index < count; index++) {
      jstring name = (jstring)(*bridge->env)->GetObjectArrayElement(
         bridge->env, names, index);
      char *text;

      if (!name)
         continue;
      text = copy_to_c(bridge, name);
      (*bridge->env)->DeleteLocalRef(bridge->env, name);
      if (!bridge_ok(bridge)) {
         free(text);
         result = -1;
         break;
      }
      if (text && callback && callback(callback_ud, text) != 0) {
         free(text);
         break;
      }
      free(text);
   }
   (*bridge->env)->DeleteLocalRef(bridge->env, names);
   return result;
}

static int fs_read(void *userdata, const char *path, char **data, size_t *size)
{
   struct bridge *bridge = (struct bridge *)userdata;
   jstring value = bridge_string(bridge, path);
   jbyteArray bytes;
   jsize length;
   char *buffer;

   if (!value)
      return -1;
   bytes = (jbyteArray)(*bridge->env)->CallObjectMethod(bridge->env,
      bridge->object, bridge->read, value);
   (*bridge->env)->DeleteLocalRef(bridge->env, value);
   if (!bridge_ok(bridge) || !bytes)
      return -1;
   length = (*bridge->env)->GetArrayLength(bridge->env, bytes);
   buffer = (char *)malloc((size_t)length + 1u);
   if (!buffer) {
      (*bridge->env)->DeleteLocalRef(bridge->env, bytes);
      return -1;
   }
   if (length > 0)
      (*bridge->env)->GetByteArrayRegion(bridge->env, bytes, 0, length,
         (jbyte *)buffer);
   (*bridge->env)->DeleteLocalRef(bridge->env, bytes);
   if (!bridge_ok(bridge)) {
      free(buffer);
      return -1;
   }
   buffer[length] = '\0';
   *data = buffer;
   if (size)
      *size = (size_t)length;
   return 0;
}

static int fs_write(void *userdata, const char *path, const char *data,
   size_t size)
{
   struct bridge *bridge = (struct bridge *)userdata;
   jstring value = bridge_string(bridge, path);
   jbyteArray bytes;
   jboolean result = JNI_FALSE;

   if (!value)
      return -1;
   bytes = (*bridge->env)->NewByteArray(bridge->env, (jsize)size);
   if (!bytes) {
      (*bridge->env)->DeleteLocalRef(bridge->env, value);
      return -1;
   }
   if (size > 0)
      (*bridge->env)->SetByteArrayRegion(bridge->env, bytes, 0, (jsize)size,
         (const jbyte *)data);
   result = (*bridge->env)->CallBooleanMethod(bridge->env, bridge->object,
      bridge->write, value, bytes);
   (*bridge->env)->DeleteLocalRef(bridge->env, bytes);
   (*bridge->env)->DeleteLocalRef(bridge->env, value);
   if (!bridge_ok(bridge))
      return -1;
   return result ? 0 : -1;
}

static int fs_remove(void *userdata, const char *path)
{
   struct bridge *bridge = (struct bridge *)userdata;
   jstring value = bridge_string(bridge, path);
   jboolean result;

   if (!value)
      return -1;
   result = (*bridge->env)->CallBooleanMethod(bridge->env, bridge->object,
      bridge->remove, value);
   (*bridge->env)->DeleteLocalRef(bridge->env, value);
   if (!bridge_ok(bridge))
      return -1;
   return result ? 0 : -1;
}

static int fs_rename(void *userdata, const char *from, const char *to)
{
   struct bridge *bridge = (struct bridge *)userdata;
   jstring source = bridge_string(bridge, from);
   jstring target = bridge_string(bridge, to);
   jboolean result = JNI_FALSE;

   if (!source || !target)
      return -1;
   result = (*bridge->env)->CallBooleanMethod(bridge->env, bridge->object,
      bridge->rename, source, target);
   (*bridge->env)->DeleteLocalRef(bridge->env, source);
   (*bridge->env)->DeleteLocalRef(bridge->env, target);
   if (!bridge_ok(bridge))
      return -1;
   return result ? 0 : -1;
}

static int http_post(void *userdata, const char *body, char **response,
   char *error, size_t error_size)
{
   struct bridge *bridge = (struct bridge *)userdata;
   jstring request = bridge_string(bridge, body);
   jbyteArray bytes;
   jsize length;
   char *buffer;

   if (!request)
      return -1;
   bytes = (jbyteArray)(*bridge->env)->CallObjectMethod(bridge->env,
      bridge->object, bridge->post, request);
   (*bridge->env)->DeleteLocalRef(bridge->env, request);
   if (!bridge_ok(bridge) || !bytes) {
      jstring reason = (jstring)(*bridge->env)->CallObjectMethod(bridge->env,
         bridge->object, bridge->last_error);
      char *text = copy_to_c(bridge, reason);

      if (reason)
         (*bridge->env)->DeleteLocalRef(bridge->env, reason);
      if (text && text[0])
         snprintf(error, error_size, "%s", text);
      else
         snprintf(error, error_size, "network error");
      free(text);
      (void)bridge_ok(bridge);
      return -1;
   }
   length = (*bridge->env)->GetArrayLength(bridge->env, bytes);
   buffer = (char *)malloc((size_t)length + 1u);
   if (!buffer) {
      (*bridge->env)->DeleteLocalRef(bridge->env, bytes);
      snprintf(error, error_size, "out of memory");
      return -1;
   }
   if (length > 0)
      (*bridge->env)->GetByteArrayRegion(bridge->env, bytes, 0, length,
         (jbyte *)buffer);
   (*bridge->env)->DeleteLocalRef(bridge->env, bytes);
   if (!bridge_ok(bridge)) {
      free(buffer);
      snprintf(error, error_size, "network error");
      return -1;
   }
   buffer[length] = '\0';
   *response = buffer;
   return 0;
}

static void core_log(void *userdata, const char *line)
{
   bridge_log((struct bridge *)userdata, line);
}

static void core_status(struct bridge *bridge, int result,
   const struct ra_sync_stats *stats)
{
   char summary[320];
   jstring value;

   if (result != 0 && stats->error[0]) {
      snprintf(summary, sizeof(summary), "%.*s", (int)sizeof(summary) - 1,
         stats->error);
   } else {
      snprintf(summary, sizeof(summary),
         "requests=%d cached=%d unknown=%d awarded=%d failed=%d pending=%d",
         stats->requests, stats->cached, stats->unknown, stats->awarded,
         stats->failed, stats->pending);
   }
   value = bridge_string(bridge, summary);
   if (!value)
      return;
   (*bridge->env)->CallVoidMethod(bridge->env, bridge->object, bridge->status,
      value);
   (*bridge->env)->DeleteLocalRef(bridge->env, value);
   (void)bridge_ok(bridge);
}

#define LOOKUP(field, name, signature) \
   do { \
      bridge.field = (*env)->GetMethodID(env, klass, name, signature); \
      if (!bridge.field) \
         goto fail; \
   } while (0)

JNIEXPORT jint JNICALL
Java_com_unifrog_rasync_NativeSync_run(JNIEnv *env, jclass type,
   jstring user, jstring key, jboolean pull, jboolean push, jboolean list,
   jboolean dry_run, jobject callback)
{
   struct bridge bridge;
   struct ra_sync_options options;
   struct ra_sync_stats stats;
   ra_sync_http_t http;
   ra_sync_fs_t fs;
   jclass klass;
   char *user_text = NULL;
   char *key_text = NULL;
   int result;

   (void)type;
   if (!callback)
      return -1;
   memset(&bridge, 0, sizeof(bridge));
   bridge.env = env;
   bridge.object = callback;
   klass = (*env)->GetObjectClass(env, callback);
   if (!klass)
      return -1;
   LOOKUP(exists, "exists", "(Ljava/lang/String;)Z");
   LOOKUP(list, "list", "(Ljava/lang/String;)[Ljava/lang/String;");
   LOOKUP(read, "read", "(Ljava/lang/String;)[B");
   LOOKUP(write, "write", "(Ljava/lang/String;[B)Z");
   LOOKUP(remove, "delete", "(Ljava/lang/String;)Z");
   LOOKUP(rename, "rename", "(Ljava/lang/String;Ljava/lang/String;)Z");
   LOOKUP(post, "post", "(Ljava/lang/String;)[B");
   LOOKUP(last_error, "lastError", "()Ljava/lang/String;");
   LOOKUP(log, "log", "(Ljava/lang/String;)V");
   LOOKUP(status, "status", "(Ljava/lang/String;)V");

   user_text = copy_to_c(&bridge, user);
   key_text = copy_to_c(&bridge, key);
   if ((user && !user_text) || (key && !key_text)) {
      (*env)->DeleteLocalRef(env, klass);
      free(user_text);
      free(key_text);
      return -1;
   }

   memset(&options, 0, sizeof(options));
   options.user = user_text ? user_text : "";
   options.key = key_text ? key_text : "";
   options.pull = pull ? 1 : 0;
   options.push = push ? 1 : 0;
   options.list = list ? 1 : 0;
   options.dry_run = dry_run ? 1 : 0;

   memset(&http, 0, sizeof(http));
   http.userdata = &bridge;
   http.post = http_post;

   memset(&fs, 0, sizeof(fs));
   fs.userdata = &bridge;
   fs.exists = fs_exists;
   fs.list = fs_list;
   fs.read = fs_read;
   fs.write = fs_write;
   fs.remove = fs_remove;
   fs.rename = fs_rename;

   memset(&stats, 0, sizeof(stats));
   result = ra_sync_run(&http, &fs, &options, core_log, &bridge, &stats);
   core_status(&bridge, result, &stats);

   (*env)->DeleteLocalRef(env, klass);
   free(user_text);
   free(key_text);
   return result;

fail:
   (*env)->DeleteLocalRef(env, klass);
   free(user_text);
   free(key_text);
   return -1;
}
