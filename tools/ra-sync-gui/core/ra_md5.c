/*
 * Compact MD5 for the RetroAchievements request signature.
 *
 * The signature is the only cryptographic primitive the sync client needs:
 * RetroAchievements defines it as md5(id + user + hardcore [+ id + seconds]).
 * This implementation is a plain RFC 1321 transform and is shared by the
 * Windows build and the host test.
 */
#include "ra_md5.h"

#include <stdint.h>
#include <string.h>

struct md5_ctx {
   uint32_t state[4];
   uint64_t bytes;
   uint8_t block[64];
   size_t used;
};

static const uint32_t md5_k[64] = {
   0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
   0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
   0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
   0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
   0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
   0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
   0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
   0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
   0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
   0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
   0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
   0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
   0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
   0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
   0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
   0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

static const uint8_t md5_shift[64] = {
   7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
   5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
   6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static uint32_t md5_left_rotate(uint32_t value, unsigned shift)
{
   return (value << shift) | (value >> (32u - shift));
}

static void md5_transform(struct md5_ctx *ctx, const uint8_t block[64])
{
   uint32_t words[16];
   uint32_t a = ctx->state[0];
   uint32_t b = ctx->state[1];
   uint32_t c = ctx->state[2];
   uint32_t d = ctx->state[3];
   unsigned i;

   for (i = 0; i < 16; i++) {
      words[i] = (uint32_t)block[i * 4] |
         ((uint32_t)block[i * 4 + 1] << 8) |
         ((uint32_t)block[i * 4 + 2] << 16) |
         ((uint32_t)block[i * 4 + 3] << 24);
   }

   for (i = 0; i < 64; i++) {
      uint32_t f;
      unsigned g;

      if (i < 16) {
         f = (b & c) | (~b & d);
         g = i;
      } else if (i < 32) {
         f = (d & b) | (~d & c);
         g = (5u * i + 1u) & 15u;
      } else if (i < 48) {
         f = b ^ c ^ d;
         g = (3u * i + 5u) & 15u;
      } else {
         f = c ^ (b | ~d);
         g = (7u * i) & 15u;
      }
      f = f + a + md5_k[i] + words[g];
      a = d;
      d = c;
      c = b;
      b = b + md5_left_rotate(f, md5_shift[i]);
   }

   ctx->state[0] += a;
   ctx->state[1] += b;
   ctx->state[2] += c;
   ctx->state[3] += d;
}

static void md5_init(struct md5_ctx *ctx)
{
   memset(ctx, 0, sizeof(*ctx));
   ctx->state[0] = 0x67452301u;
   ctx->state[1] = 0xefcdab89u;
   ctx->state[2] = 0x98badcfeu;
   ctx->state[3] = 0x10325476u;
}

static void md5_update(struct md5_ctx *ctx, const uint8_t *data, size_t size)
{
   ctx->bytes += (uint64_t)size;
   while (size > 0) {
      size_t room = sizeof(ctx->block) - ctx->used;
      size_t take = size < room ? size : room;

      memcpy(ctx->block + ctx->used, data, take);
      ctx->used += take;
      data += take;
      size -= take;
      if (ctx->used == sizeof(ctx->block)) {
         md5_transform(ctx, ctx->block);
         ctx->used = 0;
      }
   }
}

static void md5_finish(struct md5_ctx *ctx, uint8_t digest[16])
{
   uint64_t bits = ctx->bytes * 8u;
   uint8_t tail[8];
   unsigned i;

   for (i = 0; i < 8; i++)
      tail[i] = (uint8_t)(bits >> (i * 8u));

   /* 0x80 padding, then the length. The block buffer always has room for the
    * terminator because a full block is transformed as soon as it fills. */
   {
      uint8_t one = 0x80;

      md5_update(ctx, &one, 1);
   }
   while (ctx->used != 56) {
      uint8_t zero = 0;

      md5_update(ctx, &zero, 1);
   }
   md5_update(ctx, tail, sizeof(tail));
   /* md5_update counted the padding in ctx->bytes, but the length field was
    * captured before; only the transform state matters now. */

   for (i = 0; i < 4; i++) {
      digest[i * 4] = (uint8_t)ctx->state[i];
      digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 8);
      digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 16);
      digest[i * 4 + 3] = (uint8_t)(ctx->state[i] >> 24);
   }
}

void ra_md5_hex(const void *data, size_t size, char out[33])
{
   static const char hex[] = "0123456789abcdef";
   struct md5_ctx ctx;
   uint8_t digest[16];
   unsigned i;

   md5_init(&ctx);
   if (data && size > 0)
      md5_update(&ctx, (const uint8_t *)data, size);
   md5_finish(&ctx, digest);
   for (i = 0; i < 16; i++) {
      out[i * 2] = hex[digest[i] >> 4];
      out[i * 2 + 1] = hex[digest[i] & 15u];
   }
   out[32] = '\0';
}
