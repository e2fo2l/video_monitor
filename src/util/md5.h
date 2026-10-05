// RFC 1321 reference MD5 (MD5Init/MD5Update/MD5Final at 0x487d40..0x4881e4).
#pragma once

struct MD5_CTX
{
  unsigned int count[2];
  unsigned int state[4];
  unsigned char buffer[64];
};

void MD5Init(MD5_CTX* ctx);
void MD5Update(MD5_CTX* ctx, const unsigned char* input, unsigned int len);
void MD5Final(MD5_CTX* ctx, unsigned char digest[16]);
void MD5Encode(unsigned char* out, const unsigned int* in, unsigned int len);
void MD5Decode(unsigned int* out, const unsigned char* in, unsigned int len);
void MD5Transform(unsigned int state[4], const unsigned char block[64]);
