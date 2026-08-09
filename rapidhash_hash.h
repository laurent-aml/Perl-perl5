/* rapidhash V3, a fast 64-bit non-cryptographic hash by Nicolas De Carli,
 * based on wyhash (Wang Yi).  Imported from https://github.com/Nicoshev/rapidhash
 *
 * Copyright 2025 Nicolas De Carli
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * --- Perl integration notes ---------------------------------------------------
 * Exposed here as the PERL_HASH_FUNC_RAPIDHASH option, the successor to wyhash.
 * Short keys still go through sbox32 (see hv_func.h: len <= SBOX32_MAX_LEN), so
 * this only runs for longer keys, where its word-at-a-time mixing is far faster
 * than a byte-at-a-time hash.  rapidhash is NOT collision-attack resistant on
 * its own; short-key flooding resistance is provided by the sbox32 layer that
 * wraps it.
 *
 * This is the RAPIDHASH_COMPACT form (a single 112-byte bulk loop): it computes
 * exactly the same hash values as the fully-unrolled upstream default, the
 * unrolling being only a throughput optimisation for very long keys. */
#ifndef PERL_SEEN_RAPIDHASH_H
#define PERL_SEEN_RAPIDHASH_H

#ifndef PERL_SEEN_HV_FUNC_H_
#include <stdint.h>
#include <string.h>
#if !defined(U32)
#define U32 uint32_t
#endif
#if !defined(U64)
#define U64 uint64_t
#endif
#ifndef STRLEN
#define STRLEN int
#endif
#endif

#ifndef LIKELY
#define LIKELY(x) (x)
#endif

/* the default rapidhash secret */
static const U64 S_rapid_secret[8] = {
    0x2d358dccaa6c78a5ULL, 0x8bb84b93962eacc9ULL, 0x4b33a62ed433d4a3ULL,
    0x4d5a2da51de1aa47ULL, 0xa0761d6478bd642fULL, 0xe7037ed1a0b428dbULL,
    0x90ed1765281c388cULL, 0xaaaaaaaaaaaaaaaaULL
};

/* 64x64 -> 128 multiply, folded into (lo, hi) */
PERL_STATIC_INLINE void
S_rapid_mum(U64 *A, U64 *B) {
#if defined(__SIZEOF_INT128__)
    __uint128_t r = *A;
    r *= *B;
    *A = (U64)r;
    *B = (U64)(r >> 64);
#elif defined(_MSC_VER) && defined(_M_X64)
    *A = _umul128(*A, *B, B);
#else
    U64 ha = *A >> 32, hb = *B >> 32, la = (U32)*A, lb = (U32)*B, hi, lo;
    U64 rh = ha * hb, rm0 = ha * lb, rm1 = hb * la, rl = la * lb;
    U64 t = rl + (rm0 << 32), c = t < rl;
    lo = t + (rm1 << 32);
    c += lo < t;
    hi = rh + (rm0 >> 32) + (rm1 >> 32) + c;
    *A = lo;
    *B = hi;
#endif
}
PERL_STATIC_INLINE U64 S_rapid_mix(U64 A, U64 B) { S_rapid_mum(&A, &B); return A ^ B; }
PERL_STATIC_INLINE U64 S_rapid_read64(const unsigned char *p) { U64 v; memcpy(&v, p, 8); return v; }
PERL_STATIC_INLINE U64 S_rapid_read32(const unsigned char *p) { U32 v; memcpy(&v, p, 4); return v; }

PERL_STATIC_INLINE void
rapidhash_seed_state(const unsigned char * const seed_buf, unsigned char * state_buf) {
    memcpy(state_buf, seed_buf, sizeof(U64));   /* a 64-bit per-process seed */
}

PERL_STATIC_INLINE U32
rapidhash_hash_with_state(const unsigned char * const state, const unsigned char *str, const STRLEN len) {
    const unsigned char *p = str;
    const U64 *secret = S_rapid_secret;
    U64 seed, a = 0, b = 0, h;
    STRLEN i = len;

    memcpy(&seed, state, sizeof(U64));
    seed ^= S_rapid_mix(seed ^ secret[2], secret[1]);

    if (LIKELY(len <= 16)) {
        if (len >= 4) {
            seed ^= len;
            if (len >= 8) {
                a = S_rapid_read64(p);
                b = S_rapid_read64(p + len - 8);
            } else {
                a = S_rapid_read32(p);
                b = S_rapid_read32(p + len - 4);
            }
        } else if (len > 0) {
            a = (((U64)p[0]) << 45) | p[len - 1];
            b = p[len >> 1];
        } /* else len == 0: a = b = 0 */
    } else {
        if (len > 112) {
            U64 see1 = seed, see2 = seed, see3 = seed;
            U64 see4 = seed, see5 = seed, see6 = seed;
            do {
                seed = S_rapid_mix(S_rapid_read64(p)       ^ secret[0], S_rapid_read64(p + 8)   ^ seed);
                see1 = S_rapid_mix(S_rapid_read64(p + 16)  ^ secret[1], S_rapid_read64(p + 24)  ^ see1);
                see2 = S_rapid_mix(S_rapid_read64(p + 32)  ^ secret[2], S_rapid_read64(p + 40)  ^ see2);
                see3 = S_rapid_mix(S_rapid_read64(p + 48)  ^ secret[3], S_rapid_read64(p + 56)  ^ see3);
                see4 = S_rapid_mix(S_rapid_read64(p + 64)  ^ secret[4], S_rapid_read64(p + 72)  ^ see4);
                see5 = S_rapid_mix(S_rapid_read64(p + 80)  ^ secret[5], S_rapid_read64(p + 88)  ^ see5);
                see6 = S_rapid_mix(S_rapid_read64(p + 96)  ^ secret[6], S_rapid_read64(p + 104) ^ see6);
                p += 112;
                i -= 112;
            } while (i > 112);
            seed ^= see1; see2 ^= see3; see4 ^= see5; seed ^= see6; see2 ^= see4; seed ^= see2;
        }
        if (i > 16) {
            seed = S_rapid_mix(S_rapid_read64(p)      ^ secret[2], S_rapid_read64(p + 8)  ^ seed);
            if (i > 32) {
                seed = S_rapid_mix(S_rapid_read64(p + 16) ^ secret[2], S_rapid_read64(p + 24) ^ seed);
                if (i > 48) {
                    seed = S_rapid_mix(S_rapid_read64(p + 32) ^ secret[1], S_rapid_read64(p + 40) ^ seed);
                    if (i > 64) {
                        seed = S_rapid_mix(S_rapid_read64(p + 48) ^ secret[1], S_rapid_read64(p + 56) ^ seed);
                        if (i > 80) {
                            seed = S_rapid_mix(S_rapid_read64(p + 64) ^ secret[2], S_rapid_read64(p + 72) ^ seed);
                            if (i > 96) {
                                seed = S_rapid_mix(S_rapid_read64(p + 80) ^ secret[1], S_rapid_read64(p + 88) ^ seed);
                            }
                        }
                    }
                }
            }
        }
        a = S_rapid_read64(p + i - 16) ^ i;
        b = S_rapid_read64(p + i - 8);
    }
    a ^= secret[1];
    b ^= seed;
    S_rapid_mum(&a, &b);
    h = S_rapid_mix(a ^ secret[7], b ^ secret[1] ^ i);
    return (U32)(h ^ (h >> 32));   /* fold the 64-bit result to Perl's U32 */
}

#endif
