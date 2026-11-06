/* SHA-1 (FIPS 180-4) for the PC tests only; transmem.library uses
 * gfopencrypto.library's. Also tm_now for the tests: TM_NOW or 1700000000.
 * MIT, Copyright (c) 2026 AmiCradle Team. */
#include <stdlib.h>
#include <string.h>
#include "../src/tm_core.h"

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void block(unsigned int h[5], const unsigned char *p)
{
	unsigned int w[80], a, b, c, d, e, t;
	int i;
	for (i = 0; i < 16; i++) w[i] = (unsigned)p[4*i] << 24 | (unsigned)p[4*i+1] << 16 | (unsigned)p[4*i+2] << 8 | p[4*i+3];
	for (; i < 80; i++) { t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16]; w[i] = ROL(t, 1); }
	a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
	for (i = 0; i < 80; i++) {
		unsigned int f, k;
		if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
		else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
		else { f = b ^ c ^ d; k = 0xCA62C1D6; }
		t = ROL(a, 5) + f + e + k + w[i];
		e = d; d = c; c = ROL(b, 30); b = a; a = t;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

int tm_sha1(const void *data, unsigned long n, tm_u8 out[20])
{
	unsigned int h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
	const unsigned char *p = (const unsigned char *)data;
	unsigned char last[128];
	unsigned long k, rest = n % 64, full = n - rest;
	unsigned long long bits = (unsigned long long)n * 8;
	int i, m;
	for (k = 0; k < full; k += 64) block(h, p + k);
	memset(last, 0, sizeof last);
	memcpy(last, p + full, rest);
	last[rest] = 0x80;
	m = rest < 56 ? 64 : 128;
	for (i = 0; i < 8; i++) last[m - 1 - i] = (unsigned char)(bits >> (8 * i));
	block(h, last);
	if (m == 128) block(h, last + 64);
	for (i = 0; i < 20; i++) out[i] = (tm_u8)(h[i / 4] >> (24 - 8 * (i % 4)));
	return 0;
}

long tm_now(void)
{
	const char *s = getenv("TM_NOW");
	return s ? atol(s) : 1700000000L;
}
