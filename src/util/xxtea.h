// XXTEA (Ma Bingyao's xxtea-c): 16-byte zero-padded key, plaintext length stored in the last word.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif
  void* xxtea_encrypt(const void* data, size_t len, const void* key, size_t* out_len);
  void* xxtea_decrypt(const void* data, size_t len, const void* key, size_t* out_len);
#ifdef __cplusplus
}
#endif
