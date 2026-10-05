/* xxtea_encrypt / xxtea_decrypt at 0x4a7440 / 0x4a74ec and helpers 0x4a6c38..0x4a735c. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xxtea.h"

static const uint32_t DELTA = 0x9e3779b9U;
#define MX ((((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((sum ^ y) + (key[(p & 3) ^ e] ^ z)))

static uint32_t *xxtea_to_uint_array(const uint8_t *data, size_t len, int inc_len, size_t *out_len)
{
  size_t n = (len & 3) == 0 ? (len >> 2) : (len >> 2) + 1;
  uint32_t *out = NULL;
  if (inc_len)
  {
    out = (uint32_t *)calloc(n + 1, sizeof(uint32_t));
    if (!out)
      return NULL;
    out[n] = (uint32_t)len;
    *out_len = n + 1;
  }
  else
  {
    out = (uint32_t *)calloc(n, sizeof(uint32_t));
    if (!out)
      return NULL;
    *out_len = n;
  }
  memcpy(out, data, len); /* little-endian target */
  return out;
}

static uint8_t *xxtea_to_ubyte_array(const uint32_t *data, size_t len, int inc_len, size_t *out_len)
{
  size_t m = 0;
  size_t n = len << 2;
  uint8_t *out = NULL;
  if (inc_len)
  {
    m = data[len - 1];
    n -= 4;
    if (m < n - 3 || m > n)
      return NULL;
    n = m;
  }
  out = (uint8_t *)malloc(n + 1);
  memcpy(out, data, n);
  out[n] = '\0';
  *out_len = n;
  return out;
}

static uint32_t *xxtea_uint_encrypt(uint32_t *data, size_t len, const uint32_t *key)
{
  uint32_t n = (uint32_t)len - 1;
  uint32_t z = data[n];
  uint32_t y = 0;
  uint32_t p = 0;
  uint32_t q = 6 + (52 / (n + 1));
  uint32_t sum = 0;
  uint32_t e = 0;
  if (n < 1)
    return data;
  while (0 < q--)
  {
    sum += DELTA;
    e = sum >> 2 & 3;
    for (p = 0; p < n; p++)
    {
      y = data[p + 1];
      z = data[p] += MX;
    }
    y = data[0];
    z = data[n] += MX;
  }
  return data;
}

static uint32_t *xxtea_uint_decrypt(uint32_t *data, size_t len, const uint32_t *key)
{
  uint32_t n = (uint32_t)len - 1;
  uint32_t z = 0;
  uint32_t y = data[0];
  uint32_t p = 0;
  uint32_t q = 6 + (52 / (n + 1));
  uint32_t sum = q * DELTA;
  uint32_t e = 0;
  if (n < 1)
    return data;
  while (sum != 0)
  {
    e = sum >> 2 & 3;
    for (p = n; p > 0; p--)
    {
      z = data[p - 1];
      y = data[p] -= MX;
    }
    z = data[n];
    y = data[0] -= MX;
    sum -= DELTA;
  }
  return data;
}

static uint8_t *xxtea_ubyte_encrypt(const uint8_t *data, size_t len, const uint8_t *key, size_t *out_len)
{
  uint8_t *out = NULL;
  uint32_t *d = NULL;
  uint32_t *k = NULL;
  size_t dl = 0;
  size_t kl = 0;
  if (!len)
    return NULL;
  d = xxtea_to_uint_array(data, len, 1, &dl);
  if (!d)
    return NULL;
  k = xxtea_to_uint_array(key, 16, 0, &kl);
  if (!k)
  {
    free(d);
    return NULL;
  }
  out = xxtea_to_ubyte_array(xxtea_uint_encrypt(d, dl, k), dl, 0, out_len);
  free(d);
  free(k);
  return out;
}

static uint8_t *xxtea_ubyte_decrypt(const uint8_t *data, size_t len, const uint8_t *key, size_t *out_len)
{
  uint8_t *out = NULL;
  uint32_t *d = NULL;
  uint32_t *k = NULL;
  size_t dl = 0;
  size_t kl = 0;
  if (!len)
    return NULL;
  d = xxtea_to_uint_array(data, len, 0, &dl);
  if (!d)
    return NULL;
  k = xxtea_to_uint_array(key, 16, 0, &kl);
  if (!k)
  {
    free(d);
    return NULL;
  }
  out = xxtea_to_ubyte_array(xxtea_uint_decrypt(d, dl, k), dl, 1, out_len);
  free(d);
  free(k);
  return out;
}

/* The original memcpy()s 16 bytes from the key pointer and zeroes everything after the first
 * NUL; copying only up to the NUL gives the same key without over-reading. */
static void fix_key(uint8_t fixed[16], const void *key)
{
  const uint8_t *k = (const uint8_t *)key;
  size_t i = 0;
  memset(fixed, 0, 16);
  for (i = 0; i < 16 && k[i] != 0; ++i)
    fixed[i] = k[i];
}

void *xxtea_encrypt(const void *data, size_t len, const void *key, size_t *out_len)
{
  uint8_t k[16];
  fix_key(k, key);
  return xxtea_ubyte_encrypt((const uint8_t *)data, len, k, out_len);
}

void *xxtea_decrypt(const void *data, size_t len, const void *key, size_t *out_len)
{
  uint8_t k[16];
  fix_key(k, key);
  return xxtea_ubyte_decrypt((const uint8_t *)data, len, k, out_len);
}
