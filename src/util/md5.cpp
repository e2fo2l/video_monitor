#include "md5.h"

#include <cstring>

namespace
{
const unsigned char PADDING[64] = {0x80};

inline unsigned F(unsigned x, unsigned y, unsigned z) { return (x & y) | (~x & z); }
inline unsigned G(unsigned x, unsigned y, unsigned z) { return (x & z) | (y & ~z); }
inline unsigned H(unsigned x, unsigned y, unsigned z) { return x ^ y ^ z; }
inline unsigned I(unsigned x, unsigned y, unsigned z) { return y ^ (x | ~z); }
inline unsigned rotl(unsigned x, int n) { return (x << n) | (x >> (32 - n)); }

template <unsigned (*Fn)(unsigned, unsigned, unsigned)>
inline void step(unsigned& a, unsigned b, unsigned c, unsigned d, unsigned x, int s, unsigned ac)
{
  a += Fn(b, c, d) + x + ac;
  a = rotl(a, s) + b;
}
} // namespace

void MD5Init(MD5_CTX* ctx)
{
  ctx->count[0] = ctx->count[1] = 0;
  ctx->state[0] = 0x67452301;
  ctx->state[1] = 0xefcdab89;
  ctx->state[2] = 0x98badcfe;
  ctx->state[3] = 0x10325476;
}

void MD5Update(MD5_CTX* ctx, const unsigned char* input, unsigned int len)
{
  unsigned index = (ctx->count[0] >> 3) & 0x3f;
  ctx->count[0] += len << 3;
  if (ctx->count[0] < (len << 3))
    ctx->count[1]++;
  ctx->count[1] += len >> 29;
  const unsigned partLen = 64 - index;
  unsigned i = 0;
  if (len >= partLen)
  {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index): index is masked to 0..63
    memcpy(&ctx->buffer[index], input, partLen);
    MD5Transform(ctx->state, ctx->buffer);
    for (i = partLen; i + 63 < len; i += 64)
      // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound): false positive, MD5Final passes at most 64 bytes of PADDING
      MD5Transform(ctx->state, &input[i]);
    index = 0;
  }
  // Index is masked to 0..63. The analyzer warning is a false positive: MD5Final passes at most 64
  // bytes of PADDING.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index,clang-analyzer-security.ArrayBound)
  memcpy(&ctx->buffer[index], &input[i], len - i);
}

void MD5Final(MD5_CTX* ctx, unsigned char digest[16])
{
  unsigned char bits[8];
  MD5Encode(bits, ctx->count, 8);
  const unsigned index = (ctx->count[0] >> 3) & 0x3f;
  const unsigned padLen = (index < 56) ? (56 - index) : (120 - index);
  MD5Update(ctx, PADDING, padLen);
  MD5Update(ctx, bits, 8);
  MD5Encode(digest, ctx->state, 16);
}

void MD5Encode(unsigned char* out, const unsigned int* in, unsigned int len)
{
  for (unsigned i = 0, j = 0; j < len; ++i, j += 4)
  {
    out[j] = in[i] & 0xff;
    out[j + 1] = (in[i] >> 8) & 0xff;
    out[j + 2] = (in[i] >> 16) & 0xff;
    out[j + 3] = (in[i] >> 24) & 0xff;
  }
}

void MD5Decode(unsigned int* out, const unsigned char* in, unsigned int len)
{
  for (unsigned i = 0, j = 0; j < len; ++i, j += 4)
    out[i] = in[j] | (in[j + 1] << 8) | (in[j + 2] << 16) | (static_cast<unsigned>(in[j + 3]) << 24);
}

void MD5Transform(unsigned int state[4], const unsigned char block[64])
{
  unsigned a = state[0];
  unsigned b = state[1];
  unsigned c = state[2];
  unsigned d = state[3];
  unsigned x[16];
  MD5Decode(x, block, 64);

  step<F>(a, b, c, d, x[0], 7, 0xd76aa478);
  step<F>(d, a, b, c, x[1], 12, 0xe8c7b756);
  step<F>(c, d, a, b, x[2], 17, 0x242070db);
  step<F>(b, c, d, a, x[3], 22, 0xc1bdceee);
  step<F>(a, b, c, d, x[4], 7, 0xf57c0faf);
  step<F>(d, a, b, c, x[5], 12, 0x4787c62a);
  step<F>(c, d, a, b, x[6], 17, 0xa8304613);
  step<F>(b, c, d, a, x[7], 22, 0xfd469501);
  step<F>(a, b, c, d, x[8], 7, 0x698098d8);
  step<F>(d, a, b, c, x[9], 12, 0x8b44f7af);
  step<F>(c, d, a, b, x[10], 17, 0xffff5bb1);
  step<F>(b, c, d, a, x[11], 22, 0x895cd7be);
  step<F>(a, b, c, d, x[12], 7, 0x6b901122);
  step<F>(d, a, b, c, x[13], 12, 0xfd987193);
  step<F>(c, d, a, b, x[14], 17, 0xa679438e);
  step<F>(b, c, d, a, x[15], 22, 0x49b40821);

  step<G>(a, b, c, d, x[1], 5, 0xf61e2562);
  step<G>(d, a, b, c, x[6], 9, 0xc040b340);
  step<G>(c, d, a, b, x[11], 14, 0x265e5a51);
  step<G>(b, c, d, a, x[0], 20, 0xe9b6c7aa);
  step<G>(a, b, c, d, x[5], 5, 0xd62f105d);
  step<G>(d, a, b, c, x[10], 9, 0x02441453);
  step<G>(c, d, a, b, x[15], 14, 0xd8a1e681);
  step<G>(b, c, d, a, x[4], 20, 0xe7d3fbc8);
  step<G>(a, b, c, d, x[9], 5, 0x21e1cde6);
  step<G>(d, a, b, c, x[14], 9, 0xc33707d6);
  step<G>(c, d, a, b, x[3], 14, 0xf4d50d87);
  step<G>(b, c, d, a, x[8], 20, 0x455a14ed);
  step<G>(a, b, c, d, x[13], 5, 0xa9e3e905);
  step<G>(d, a, b, c, x[2], 9, 0xfcefa3f8);
  step<G>(c, d, a, b, x[7], 14, 0x676f02d9);
  step<G>(b, c, d, a, x[12], 20, 0x8d2a4c8a);

  step<H>(a, b, c, d, x[5], 4, 0xfffa3942);
  step<H>(d, a, b, c, x[8], 11, 0x8771f681);
  step<H>(c, d, a, b, x[11], 16, 0x6d9d6122);
  step<H>(b, c, d, a, x[14], 23, 0xfde5380c);
  step<H>(a, b, c, d, x[1], 4, 0xa4beea44);
  step<H>(d, a, b, c, x[4], 11, 0x4bdecfa9);
  step<H>(c, d, a, b, x[7], 16, 0xf6bb4b60);
  step<H>(b, c, d, a, x[10], 23, 0xbebfbc70);
  step<H>(a, b, c, d, x[13], 4, 0x289b7ec6);
  step<H>(d, a, b, c, x[0], 11, 0xeaa127fa);
  step<H>(c, d, a, b, x[3], 16, 0xd4ef3085);
  step<H>(b, c, d, a, x[6], 23, 0x04881d05);
  step<H>(a, b, c, d, x[9], 4, 0xd9d4d039);
  step<H>(d, a, b, c, x[12], 11, 0xe6db99e5);
  step<H>(c, d, a, b, x[15], 16, 0x1fa27cf8);
  step<H>(b, c, d, a, x[2], 23, 0xc4ac5665);

  step<I>(a, b, c, d, x[0], 6, 0xf4292244);
  step<I>(d, a, b, c, x[7], 10, 0x432aff97);
  step<I>(c, d, a, b, x[14], 15, 0xab9423a7);
  step<I>(b, c, d, a, x[5], 21, 0xfc93a039);
  step<I>(a, b, c, d, x[12], 6, 0x655b59c3);
  step<I>(d, a, b, c, x[3], 10, 0x8f0ccc92);
  step<I>(c, d, a, b, x[10], 15, 0xffeff47d);
  step<I>(b, c, d, a, x[1], 21, 0x85845dd1);
  step<I>(a, b, c, d, x[8], 6, 0x6fa87e4f);
  step<I>(d, a, b, c, x[15], 10, 0xfe2ce6e0);
  step<I>(c, d, a, b, x[6], 15, 0xa3014314);
  step<I>(b, c, d, a, x[13], 21, 0x4e0811a1);
  step<I>(a, b, c, d, x[4], 6, 0xf7537e82);
  step<I>(d, a, b, c, x[11], 10, 0xbd3af235);
  step<I>(c, d, a, b, x[2], 15, 0x2ad7d2bb);
  step<I>(b, c, d, a, x[9], 21, 0xeb86d391);

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}
