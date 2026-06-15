// ed25519_verify.cpp -- minimal verify-only ed25519 (RFC 8032) for ExoSnap.
// Derived from public-domain SUPERCOP/ref10 (Bernstein, Lange, Schwabe).
// Only verify() is exposed; no signing, no key generation.
// SHA-512 implemented inline (FIPS 180-4). No external dependencies.

#include <cstring>
#include <update/ed25519_verify.h>

namespace exosnap::update {
namespace {

// ---------------------------------------------------------------------------
// SHA-512 (FIPS 180-4)
// ---------------------------------------------------------------------------
constexpr uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};
inline uint64_t ror64(uint64_t v, int n) noexcept {
    return (v >> n) | (v << (64 - n));
}

struct SHA512 {
    uint64_t h[8];
    uint8_t buf[128];
    uint64_t lo, hi;
    size_t n;

    SHA512() noexcept {
        h[0] = 0x6a09e667f3bcc908ULL;
        h[1] = 0xbb67ae8584caa73bULL;
        h[2] = 0x3c6ef372fe94f82bULL;
        h[3] = 0xa54ff53a5f1d36f1ULL;
        h[4] = 0x510e527fade682d1ULL;
        h[5] = 0x9b05688c2b3e6c1fULL;
        h[6] = 0x1f83d9abfb41bd6bULL;
        h[7] = 0x5be0cd19137e2179ULL;
        lo = 0;
        hi = 0;
        n = 0;
    }

    void compress(const uint8_t* b) noexcept {
        uint64_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = ((uint64_t)b[i * 8] << 56) | ((uint64_t)b[i * 8 + 1] << 48) | ((uint64_t)b[i * 8 + 2] << 40) |
                   ((uint64_t)b[i * 8 + 3] << 32) | ((uint64_t)b[i * 8 + 4] << 24) | ((uint64_t)b[i * 8 + 5] << 16) |
                   ((uint64_t)b[i * 8 + 6] << 8) | (uint64_t)b[i * 8 + 7];
        }
        for (int i = 16; i < 80; ++i) {
            uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t a = h[0], b_ = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hv = h[7];
        for (int i = 0; i < 80; ++i) {
            uint64_t S1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
            uint64_t ch = (e & f) ^ (~e & g);
            uint64_t t1 = hv + S1 + ch + K512[i] + w[i];
            uint64_t S0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
            uint64_t maj = (a & b_) ^ (a & c) ^ (b_ & c);
            uint64_t t2 = S0 + maj;
            hv = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b_;
            b_ = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b_;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hv;
    }

    void update(const uint8_t* data, size_t len) noexcept {
        while (len) {
            size_t sp = 128 - n;
            size_t tk = (len < sp) ? len : sp;
            memcpy(buf + n, data, tk);
            n += tk;
            data += tk;
            len -= tk;
            uint64_t b2 = (uint64_t)tk * 8;
            lo += b2;
            if (lo < b2)
                ++hi;
            if (n == 128) {
                compress(buf);
                n = 0;
            }
        }
    }

    void final(uint8_t out[64]) noexcept {
        buf[n++] = 0x80;
        if (n > 112) {
            while (n < 128)
                buf[n++] = 0;
            compress(buf);
            n = 0;
        }
        while (n < 112)
            buf[n++] = 0;
        for (int i = 0; i < 8; ++i)
            buf[112 + i] = (uint8_t)(hi >> (56 - 8 * i));
        for (int i = 0; i < 8; ++i)
            buf[120 + i] = (uint8_t)(lo >> (56 - 8 * i));
        compress(buf);
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 8; ++j)
                out[i * 8 + j] = (uint8_t)(h[i] >> (56 - 8 * j));
    }
};

// ---------------------------------------------------------------------------
// GF(2^255-19) field arithmetic (ref10, 25.5-bit limb representation)
// ---------------------------------------------------------------------------
using fe = int32_t[10];

void fe_0(fe h) noexcept {
    memset(h, 0, 40);
}
void fe_1(fe h) noexcept {
    fe_0(h);
    h[0] = 1;
}
void fe_copy(fe h, const fe f) noexcept {
    memcpy(h, f, 40);
}
void fe_neg(fe h, const fe f) noexcept {
    for (int i = 0; i < 10; ++i)
        h[i] = -f[i];
}
void fe_add(fe h, const fe f, const fe g) noexcept {
    for (int i = 0; i < 10; ++i)
        h[i] = f[i] + g[i];
}
void fe_sub(fe h, const fe f, const fe g) noexcept {
    for (int i = 0; i < 10; ++i)
        h[i] = f[i] - g[i];
}

void fe_mul(fe h, const fe f, const fe g) noexcept {
    int32_t f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    int32_t f5 = f[5], f6 = f[6], f7 = f[7], f8 = f[8], f9 = f[9];
    int32_t g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
    int32_t g5 = g[5], g6 = g[6], g7 = g[7], g8 = g[8], g9 = g[9];
    // Use int64_t for gX_19 and fX_2 to avoid int32_t overflow when inputs
    // come from ge_p2_dbl/ge_add with even limbs that can reach ~2^27.
    // (e.g., F = 2Z^2 - G where each term is ~2^26, difference up to 2^27)
    int64_t g1_19 = 19LL * g1, g2_19 = 19LL * g2, g3_19 = 19LL * g3, g4_19 = 19LL * g4;
    int64_t g5_19 = 19LL * g5, g6_19 = 19LL * g6, g7_19 = 19LL * g7, g8_19 = 19LL * g8, g9_19 = 19LL * g9;
    int64_t f1_2 = 2LL * f1, f3_2 = 2LL * f3, f5_2 = 2LL * f5, f7_2 = 2LL * f7, f9_2 = 2LL * f9;
    int64_t h0 = (int64_t)f0 * g0 + (int64_t)f1_2 * g9_19 + (int64_t)f2 * g8_19 + (int64_t)f3_2 * g7_19 +
                 (int64_t)f4 * g6_19 + (int64_t)f5_2 * g5_19 + (int64_t)f6 * g4_19 + (int64_t)f7_2 * g3_19 +
                 (int64_t)f8 * g2_19 + (int64_t)f9_2 * g1_19;
    int64_t h1 = (int64_t)f0 * g1 + (int64_t)f1 * g0 + (int64_t)f2 * g9_19 + (int64_t)f3 * g8_19 + (int64_t)f4 * g7_19 +
                 (int64_t)f5 * g6_19 + (int64_t)f6 * g5_19 + (int64_t)f7 * g4_19 + (int64_t)f8 * g3_19 +
                 (int64_t)f9 * g2_19;
    int64_t h2 = (int64_t)f0 * g2 + (int64_t)f1_2 * g1 + (int64_t)f2 * g0 + (int64_t)f3_2 * g9_19 +
                 (int64_t)f4 * g8_19 + (int64_t)f5_2 * g7_19 + (int64_t)f6 * g6_19 + (int64_t)f7_2 * g5_19 +
                 (int64_t)f8 * g4_19 + (int64_t)f9_2 * g3_19;
    int64_t h3 = (int64_t)f0 * g3 + (int64_t)f1 * g2 + (int64_t)f2 * g1 + (int64_t)f3 * g0 + (int64_t)f4 * g9_19 +
                 (int64_t)f5 * g8_19 + (int64_t)f6 * g7_19 + (int64_t)f7 * g6_19 + (int64_t)f8 * g5_19 +
                 (int64_t)f9 * g4_19;
    int64_t h4 = (int64_t)f0 * g4 + (int64_t)f1_2 * g3 + (int64_t)f2 * g2 + (int64_t)f3_2 * g1 + (int64_t)f4 * g0 +
                 (int64_t)f5_2 * g9_19 + (int64_t)f6 * g8_19 + (int64_t)f7_2 * g7_19 + (int64_t)f8 * g6_19 +
                 (int64_t)f9_2 * g5_19;
    int64_t h5 = (int64_t)f0 * g5 + (int64_t)f1 * g4 + (int64_t)f2 * g3 + (int64_t)f3 * g2 + (int64_t)f4 * g1 +
                 (int64_t)f5 * g0 + (int64_t)f6 * g9_19 + (int64_t)f7 * g8_19 + (int64_t)f8 * g7_19 +
                 (int64_t)f9 * g6_19;
    int64_t h6 = (int64_t)f0 * g6 + (int64_t)f1_2 * g5 + (int64_t)f2 * g4 + (int64_t)f3_2 * g3 + (int64_t)f4 * g2 +
                 (int64_t)f5_2 * g1 + (int64_t)f6 * g0 + (int64_t)f7_2 * g9_19 + (int64_t)f8 * g8_19 +
                 (int64_t)f9_2 * g7_19;
    int64_t h7 = (int64_t)f0 * g7 + (int64_t)f1 * g6 + (int64_t)f2 * g5 + (int64_t)f3 * g4 + (int64_t)f4 * g3 +
                 (int64_t)f5 * g2 + (int64_t)f6 * g1 + (int64_t)f7 * g0 + (int64_t)f8 * g9_19 + (int64_t)f9 * g8_19;
    int64_t h8 = (int64_t)f0 * g8 + (int64_t)f1_2 * g7 + (int64_t)f2 * g6 + (int64_t)f3_2 * g5 + (int64_t)f4 * g4 +
                 (int64_t)f5_2 * g3 + (int64_t)f6 * g2 + (int64_t)f7_2 * g1 + (int64_t)f8 * g0 + (int64_t)f9_2 * g9_19;
    int64_t h9 = (int64_t)f0 * g9 + (int64_t)f1 * g8 + (int64_t)f2 * g7 + (int64_t)f3 * g6 + (int64_t)f4 * g5 +
                 (int64_t)f5 * g4 + (int64_t)f6 * g3 + (int64_t)f7 * g2 + (int64_t)f8 * g1 + (int64_t)f9 * g0;

    int64_t c0, c1, c2, c3, c4, c5, c6, c7, c8, c9;
    c0 = (h0 + (1LL << 25)) >> 26;
    h1 += c0;
    h0 -= c0 << 26;
    c4 = (h4 + (1LL << 25)) >> 26;
    h5 += c4;
    h4 -= c4 << 26;
    c1 = (h1 + (1LL << 24)) >> 25;
    h2 += c1;
    h1 -= c1 << 25;
    c5 = (h5 + (1LL << 24)) >> 25;
    h6 += c5;
    h5 -= c5 << 25;
    c2 = (h2 + (1LL << 25)) >> 26;
    h3 += c2;
    h2 -= c2 << 26;
    c6 = (h6 + (1LL << 25)) >> 26;
    h7 += c6;
    h6 -= c6 << 26;
    c3 = (h3 + (1LL << 24)) >> 25;
    h4 += c3;
    h3 -= c3 << 25;
    c7 = (h7 + (1LL << 24)) >> 25;
    h8 += c7;
    h7 -= c7 << 25;
    c4 = (h4 + (1LL << 25)) >> 26;
    h5 += c4;
    h4 -= c4 << 26;
    c8 = (h8 + (1LL << 25)) >> 26;
    h9 += c8;
    h8 -= c8 << 26;
    c9 = (h9 + (1LL << 24)) >> 25;
    h0 += c9 * 19;
    h9 -= c9 << 25;
    c0 = (h0 + (1LL << 25)) >> 26;
    h1 += c0;
    h0 -= c0 << 26;
    h[0] = (int32_t)h0;
    h[1] = (int32_t)h1;
    h[2] = (int32_t)h2;
    h[3] = (int32_t)h3;
    h[4] = (int32_t)h4;
    h[5] = (int32_t)h5;
    h[6] = (int32_t)h6;
    h[7] = (int32_t)h7;
    h[8] = (int32_t)h8;
    h[9] = (int32_t)h9;
}

void fe_sq(fe h, const fe f) noexcept {
    fe_mul(h, f, f);
}

void fe_frombytes(fe h, const uint8_t* s) noexcept {
    auto ld3 = [](const uint8_t* x) -> int64_t { return (int64_t)x[0] | ((int64_t)x[1] << 8) | ((int64_t)x[2] << 16); };
    auto ld4 = [](const uint8_t* x) -> int64_t {
        return (int64_t)x[0] | ((int64_t)x[1] << 8) | ((int64_t)x[2] << 16) | ((int64_t)x[3] << 24);
    };
    int64_t h0 = ld4(s);
    int64_t h1 = ld3(s + 4) << 6;
    int64_t h2 = ld3(s + 7) << 5;
    int64_t h3 = ld3(s + 10) << 3;
    int64_t h4 = ld3(s + 13) << 2;
    int64_t h5 = ld4(s + 16);
    int64_t h6 = ld3(s + 20) << 7;
    int64_t h7 = ld3(s + 23) << 5;
    int64_t h8 = ld3(s + 26) << 4;
    int64_t h9 = (ld3(s + 29) & 8388607) << 2;
    // Floor carry: produce canonical non-negative limbs in [0, 2^25) or [0, 2^26).
    // Round-half-up carry (the alternative) produces signed balanced limbs which
    // break fe_tobytes's q computation.  See RFC 8032 / ref10 fe_frombytes.
    int64_t cv;
    cv = h9 >> 25;
    h0 += cv * 19;
    h9 &= 0x1FFFFFF;
    cv = h1 >> 25;
    h2 += cv;
    h1 &= 0x1FFFFFF;
    cv = h3 >> 25;
    h4 += cv;
    h3 &= 0x1FFFFFF;
    cv = h5 >> 25;
    h6 += cv;
    h5 &= 0x1FFFFFF;
    cv = h7 >> 25;
    h8 += cv;
    h7 &= 0x1FFFFFF;
    cv = h0 >> 26;
    h1 += cv;
    h0 &= 0x3FFFFFF;
    cv = h2 >> 26;
    h3 += cv;
    h2 &= 0x3FFFFFF;
    cv = h4 >> 26;
    h5 += cv;
    h4 &= 0x3FFFFFF;
    cv = h6 >> 26;
    h7 += cv;
    h6 &= 0x3FFFFFF;
    cv = h8 >> 26;
    h9 += cv;
    h8 &= 0x3FFFFFF;
    h[0] = (int32_t)h0;
    h[1] = (int32_t)h1;
    h[2] = (int32_t)h2;
    h[3] = (int32_t)h3;
    h[4] = (int32_t)h4;
    h[5] = (int32_t)h5;
    h[6] = (int32_t)h6;
    h[7] = (int32_t)h7;
    h[8] = (int32_t)h8;
    h[9] = (int32_t)h9;
}

void fe_tobytes(uint8_t* s, const fe h) noexcept {
    // fe_tobytes must handle both canonical (non-negative) and balanced (signed)
    // limb representations produced by fe_mul/fe_sq.
    //
    // Step 1: full floor-carry normalization using 64-bit temporaries so we
    // safely propagate even large/negative int32 limbs without overflow.
    // The order (9→0 first, then 0→9) matches fe_frombytes and converts any
    // balanced representation to canonical non-negative limbs.
    int64_t t[10];
    for (int i = 0; i < 10; ++i)
        t[i] = (int64_t)h[i];
    int64_t cv;
    cv = t[9] >> 25;
    t[0] += cv * 19;
    t[9] -= cv * (int64_t)(1 << 25);
    cv = t[1] >> 25;
    t[2] += cv;
    t[1] -= cv * (int64_t)(1 << 25);
    cv = t[3] >> 25;
    t[4] += cv;
    t[3] -= cv * (int64_t)(1 << 25);
    cv = t[5] >> 25;
    t[6] += cv;
    t[5] -= cv * (int64_t)(1 << 25);
    cv = t[7] >> 25;
    t[8] += cv;
    t[7] -= cv * (int64_t)(1 << 25);
    cv = t[0] >> 26;
    t[1] += cv;
    t[0] -= cv * (int64_t)(1 << 26);
    cv = t[2] >> 26;
    t[3] += cv;
    t[2] -= cv * (int64_t)(1 << 26);
    cv = t[4] >> 26;
    t[5] += cv;
    t[4] -= cv * (int64_t)(1 << 26);
    cv = t[6] >> 26;
    t[7] += cv;
    t[6] -= cv * (int64_t)(1 << 26);
    cv = t[8] >> 26;
    t[9] += cv;
    t[8] -= cv * (int64_t)(1 << 26);
    // After these two passes, all limbs are in their canonical non-negative
    // ranges: even limbs in [0, 2^26), odd limbs in [0, 2^25).
    // t[9] may still have received a carry from t[8], but since the input
    // is a valid field element, t[9] is now in [0, 2^25).
    //
    // Step 2: compute q = number of p's to subtract to reach [0, p)
    // Standard ref10 formula; works correctly because limbs are now non-negative.
    int64_t q = (19 * t[9] + (int64_t)(1 << 24)) >> 25;
    q = (t[0] + q) >> 26;
    q = (t[1] + q) >> 25;
    q = (t[2] + q) >> 26;
    q = (t[3] + q) >> 25;
    q = (t[4] + q) >> 26;
    q = (t[5] + q) >> 25;
    q = (t[6] + q) >> 26;
    q = (t[7] + q) >> 25;
    q = (t[8] + q) >> 26;
    q = (t[9] + q) >> 25;
    t[0] += 19 * q;
    // Step 3: final carry (floor, subtraction form) to produce exactly canonical limbs
    cv = t[0] >> 26;
    t[1] += cv;
    t[0] -= cv * (int64_t)(1 << 26);
    cv = t[1] >> 25;
    t[2] += cv;
    t[1] -= cv * (int64_t)(1 << 25);
    cv = t[2] >> 26;
    t[3] += cv;
    t[2] -= cv * (int64_t)(1 << 26);
    cv = t[3] >> 25;
    t[4] += cv;
    t[3] -= cv * (int64_t)(1 << 25);
    cv = t[4] >> 26;
    t[5] += cv;
    t[4] -= cv * (int64_t)(1 << 26);
    cv = t[5] >> 25;
    t[6] += cv;
    t[5] -= cv * (int64_t)(1 << 25);
    cv = t[6] >> 26;
    t[7] += cv;
    t[6] -= cv * (int64_t)(1 << 26);
    cv = t[7] >> 25;
    t[8] += cv;
    t[7] -= cv * (int64_t)(1 << 25);
    cv = t[8] >> 26;
    t[9] += cv;
    t[8] -= cv * (int64_t)(1 << 26);
    s[0] = (uint8_t)t[0];
    s[1] = (uint8_t)(t[0] >> 8);
    s[2] = (uint8_t)(t[0] >> 16);
    s[3] = (uint8_t)((t[0] >> 24) | (t[1] << 2));
    s[4] = (uint8_t)(t[1] >> 6);
    s[5] = (uint8_t)(t[1] >> 14);
    s[6] = (uint8_t)((t[1] >> 22) | (t[2] << 3));
    s[7] = (uint8_t)(t[2] >> 5);
    s[8] = (uint8_t)(t[2] >> 13);
    s[9] = (uint8_t)((t[2] >> 21) | (t[3] << 5));
    s[10] = (uint8_t)(t[3] >> 3);
    s[11] = (uint8_t)(t[3] >> 11);
    s[12] = (uint8_t)((t[3] >> 19) | (t[4] << 6));
    s[13] = (uint8_t)(t[4] >> 2);
    s[14] = (uint8_t)(t[4] >> 10);
    s[15] = (uint8_t)(t[4] >> 18);
    s[16] = (uint8_t)t[5];
    s[17] = (uint8_t)(t[5] >> 8);
    s[18] = (uint8_t)(t[5] >> 16);
    s[19] = (uint8_t)((t[5] >> 24) | (t[6] << 1));
    s[20] = (uint8_t)(t[6] >> 7);
    s[21] = (uint8_t)(t[6] >> 15);
    s[22] = (uint8_t)((t[6] >> 23) | (t[7] << 3));
    s[23] = (uint8_t)(t[7] >> 5);
    s[24] = (uint8_t)(t[7] >> 13);
    s[25] = (uint8_t)((t[7] >> 21) | (t[8] << 4));
    s[26] = (uint8_t)(t[8] >> 4);
    s[27] = (uint8_t)(t[8] >> 12);
    s[28] = (uint8_t)((t[8] >> 20) | (t[9] << 6));
    s[29] = (uint8_t)(t[9] >> 2);
    s[30] = (uint8_t)(t[9] >> 10);
    s[31] = (uint8_t)(t[9] >> 18);
}

void fe_invert(fe out, const fe z) noexcept {
    fe t0, t1, t2, t3;
    fe_sq(t0, z);
    fe_sq(t1, t0);
    fe_sq(t1, t1);
    fe_mul(t1, z, t1);
    fe_mul(t0, t0, t1);
    fe_sq(t2, t0);
    fe_mul(t1, t1, t2);
    fe_sq(t2, t1);
    for (int i = 1; i < 5; ++i)
        fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (int i = 1; i < 10; ++i)
        fe_sq(t2, t2);
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (int i = 1; i < 20; ++i)
        fe_sq(t3, t3);
    fe_mul(t2, t3, t2);
    fe_sq(t2, t2);
    for (int i = 1; i < 10; ++i)
        fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (int i = 1; i < 50; ++i)
        fe_sq(t2, t2);
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (int i = 1; i < 100; ++i)
        fe_sq(t3, t3);
    fe_mul(t2, t3, t2);
    fe_sq(t2, t2);
    for (int i = 1; i < 50; ++i)
        fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t1, t1);
    for (int i = 1; i < 5; ++i)
        fe_sq(t1, t1);
    fe_mul(out, t1, t0);
}

void fe_pow22523(fe out, const fe z) noexcept {
    fe t0, t1, t2;
    fe_sq(t0, z);
    fe_sq(t1, t0);
    fe_sq(t1, t1);
    fe_mul(t1, z, t1);
    fe_mul(t0, t0, t1);
    fe_sq(t0, t0);
    fe_mul(t0, t1, t0);
    fe_sq(t1, t0);
    for (int i = 1; i < 5; ++i)
        fe_sq(t1, t1);
    fe_mul(t0, t1, t0);
    fe_sq(t1, t0);
    for (int i = 1; i < 10; ++i)
        fe_sq(t1, t1);
    fe_mul(t1, t1, t0);
    fe_sq(t2, t1);
    for (int i = 1; i < 20; ++i)
        fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t1, t1);
    for (int i = 1; i < 10; ++i)
        fe_sq(t1, t1);
    fe_mul(t0, t1, t0);
    fe_sq(t1, t0);
    for (int i = 1; i < 50; ++i)
        fe_sq(t1, t1);
    fe_mul(t1, t1, t0);
    fe_sq(t2, t1);
    for (int i = 1; i < 100; ++i)
        fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t1, t1);
    for (int i = 1; i < 50; ++i)
        fe_sq(t1, t1);
    fe_mul(t0, t1, t0);
    fe_sq(t0, t0);
    fe_sq(t0, t0);
    fe_mul(out, t0, z);
}

// Curve constants (ref10 representation)
// kD = -121665/121666 mod p  (Edwards curve twist constant)
const fe kD = {-10913610, 13857413, -15372611, 6949391, 114729, -8787816, -6275908, -3247719, -18696448, -12055116};
// sqrt(-1) mod p
const fe kSM = {-32595792, -7943725, 9377950, 3500415, 12389472, -272473, -25146209, -2005654, 326686, 11406482};
// 2*kD
const fe kD2 = {-21827239, -5839606, -30745221, 13898782, 229458, 15978800, -12551817, -6495438, 29715968, 9444199};

// ---------------------------------------------------------------------------
// Edwards points
// ---------------------------------------------------------------------------
struct ge_p3 {
    fe X, Y, Z, T;
};
struct ge_p2 {
    fe X, Y, Z;
};
struct ge_p1p1 {
    fe X, Y, Z, T;
};

void ge_p1p1_to_p3(ge_p3& r, const ge_p1p1& p) noexcept {
    fe_mul(r.X, p.X, p.T);
    fe_mul(r.Y, p.Y, p.Z);
    fe_mul(r.Z, p.Z, p.T);
    fe_mul(r.T, p.X, p.Y);
}
void ge_p3_to_p2(ge_p2& r, const ge_p3& p) noexcept {
    fe_copy(r.X, p.X);
    fe_copy(r.Y, p.Y);
    fe_copy(r.Z, p.Z);
}
void ge_p2_dbl(ge_p1p1& r, const ge_p2& p) noexcept {
    fe t0;
    fe_sq(r.X, p.X);
    fe_sq(r.Z, p.Y);
    fe_sq(r.T, p.Z);
    fe_add(r.T, r.T, r.T);
    fe_add(r.Y, p.X, p.Y);
    fe_sq(t0, r.Y);
    fe_add(r.Y, r.Z, r.X);
    fe_sub(r.Z, r.Z, r.X);
    fe_sub(r.X, t0, r.Y);
    fe_sub(r.T, r.T, r.Z);
}
void ge_add(ge_p1p1& r, const ge_p3& p, const ge_p3& q) noexcept {
    // Unified twisted Edwards addition formula (Bernstein-Lange-Schwabe ref10).
    // A = (Y1-X1)*(Y2-X2)
    // B = (Y1+X1)*(Y2+X2)
    // C = 2*d*T1*T2
    // D = 2*Z1*Z2
    // E = B-A, F = D-C, G = D+C, H = B+A
    //
    // Output in ge_p1p1 completed coordinates (x = X/Z, y = Y/T):
    //   r.X = E, r.Y = H, r.Z = G, r.T = F
    //
    // After ge_p1p1_to_p3: X3=E*F, Y3=G*H, Z3=G*F → x3=E/G, y3=H/F ✓
    // (Previously incorrectly stored E*F, G*H, F*G, E*H which gave y3=G/E.)
    fe A, B, C, D, E, F, G, H;
    fe_sub(A, p.Y, p.X);
    fe_sub(B, q.Y, q.X);
    fe_mul(A, A, B);
    fe_add(B, p.Y, p.X);
    fe_add(C, q.Y, q.X);
    fe_mul(B, B, C);
    fe_mul(C, p.T, q.T);
    fe_mul(C, C, kD2);
    fe_mul(D, p.Z, q.Z);
    fe_add(D, D, D);
    fe_sub(E, B, A);
    fe_sub(F, D, C);
    fe_add(G, D, C);
    fe_add(H, B, A);
    fe_copy(r.X, E);
    fe_copy(r.Y, H);
    fe_copy(r.Z, G);
    fe_copy(r.T, F);
}

bool ge_frombytes(ge_p3& h, const uint8_t s[32]) noexcept {
    fe u, v, v3, vxx, chk;
    fe_frombytes(h.Y, s);
    fe_1(h.Z);
    fe_sq(u, h.Y);
    fe_mul(v, u, kD);
    fe_sub(u, u, h.Z);
    fe_add(v, v, h.Z);
    fe_sq(v3, v);
    fe_mul(v3, v3, v);
    fe_sq(h.X, v3);
    fe_mul(h.X, h.X, v);
    fe_mul(h.X, h.X, u);
    fe_pow22523(h.X, h.X);
    fe_mul(h.X, h.X, v3);
    fe_mul(h.X, h.X, u);
    fe_sq(vxx, h.X);
    fe_mul(vxx, vxx, v);
    fe_sub(chk, vxx, u);
    auto nonzero = [](const fe f) -> bool {
        uint8_t b[32];
        fe_tobytes(b, f);
        for (auto x : b)
            if (x)
                return true;
        return false;
    };
    if (nonzero(chk)) {
        fe_add(chk, vxx, u);
        if (nonzero(chk))
            return false;
        fe_mul(h.X, h.X, kSM);
    }
    uint8_t hxb[32];
    fe_tobytes(hxb, h.X);
    if ((hxb[0] & 1) != (s[31] >> 7))
        fe_neg(h.X, h.X);
    fe_mul(h.T, h.X, h.Y);
    return true;
}

// Reduce a 512-bit scalar mod group order l (ref10 sc_reduce).
// Takes a 64-byte input s and writes the 32-byte reduced result back to s[0..31].
//
// Strategy: interleaved fold+carry from high limbs (s23..s17) keeps every
// intermediate value below 2^49, safely within int64_t range.  s16 needs an
// explicit pre-carry before it is used, because seven folds accumulate a 48-bit
// negative value in it whose product with 666643 would overflow int64_t.
//
// Carry convention: arithmetic (floor) right-shift + mask preserves the two's-
// complement low bits as a non-negative remainder, so the final encoding formula
// (which assumes non-negative limbs) is always correct.
void sc_reduce(uint8_t* s) noexcept {
    // Helpers: load 3 or 4 bytes little-endian from s at byte offset p
    auto ld3 = [](const uint8_t* p) -> int64_t { return (int64_t)p[0] | ((int64_t)p[1] << 8) | ((int64_t)p[2] << 16); };
    auto ld4 = [](const uint8_t* p) -> int64_t {
        return (int64_t)p[0] | ((int64_t)p[1] << 8) | ((int64_t)p[2] << 16) | ((int64_t)p[3] << 24);
    };
    // Arithmetic (floor) truncating carry: c = a >> 21; a &= M; b += c
    // For any int64_t a: a == (a >> 21)*2097152 + (a & M), with (a & M) in [0, 2097151].
#define CARRY(a, b)                                                                                                    \
    do {                                                                                                               \
        int64_t _c = (a) >> 21;                                                                                        \
        (a) &= M;                                                                                                      \
        (b) += _c;                                                                                                     \
    } while (0)

    constexpr int64_t M = 2097151; // 2^21 - 1

    // Decompose 64-byte input into 24 limbs of 21 bits each.
    // Each limb s_k covers bits [k*21 .. k*21+20] of the 512-bit integer.
    int64_t s0 = M & ld4(s + 0);
    int64_t s1 = M & (ld4(s + 2) >> 5);
    int64_t s2 = M & (ld3(s + 5) >> 2);
    int64_t s3 = M & (ld4(s + 7) >> 7);
    int64_t s4 = M & (ld4(s + 10) >> 4);
    int64_t s5 = M & (ld3(s + 13) >> 1);
    int64_t s6 = M & (ld4(s + 15) >> 6);
    int64_t s7 = M & (ld3(s + 18) >> 3);
    int64_t s8 = M & ld3(s + 21);
    int64_t s9 = M & (ld4(s + 23) >> 5);
    int64_t s10 = M & (ld3(s + 26) >> 2);
    int64_t s11 = M & (ld4(s + 28) >> 7);
    int64_t s12 = M & (ld4(s + 31) >> 4);
    int64_t s13 = M & (ld3(s + 34) >> 1);
    int64_t s14 = M & (ld4(s + 36) >> 6);
    int64_t s15 = M & (ld3(s + 39) >> 3);
    int64_t s16 = M & ld3(s + 42);
    int64_t s17 = M & (ld4(s + 44) >> 5);
    int64_t s18 = M & (ld3(s + 47) >> 2);
    int64_t s19 = M & (ld4(s + 49) >> 7);
    int64_t s20 = M & (ld4(s + 52) >> 4);
    int64_t s21 = M & (ld3(s + 55) >> 1);
    int64_t s22 = M & (ld4(s + 57) >> 6);
    int64_t s23 = ld4(s + 60) >> 3; // no mask: top 29-bit chunk

    // Fold s23..s17 into s11..s5, each followed by an immediate carry pass
    // on the affected limbs.  This interleaving keeps every value below 2^49,
    // well within int64_t range.
    //
    // Fold identity: s_k * 2^(k*21) ≡ s_k * (-c_excess) * 2^((k-12)*21) (mod l)
    // where c_excess = l - 2^252 = 27742317777372353535851937790883648493 and
    // its signed 21-bit limbs are: [666643, 470296, 654183, -997805, 136657, -683901, 0, ...].

    // --- s23 ---
    s11 += s23 * 666643;
    s12 += s23 * 470296;
    s13 += s23 * 654183;
    s14 -= s23 * 997805;
    s15 += s23 * 136657;
    s16 -= s23 * 683901;
    CARRY(s11, s12);
    CARRY(s12, s13);
    CARRY(s13, s14);
    CARRY(s14, s15);
    CARRY(s15, s16);

    // --- s22 ---
    s10 += s22 * 666643;
    s11 += s22 * 470296;
    s12 += s22 * 654183;
    s13 -= s22 * 997805;
    s14 += s22 * 136657;
    s15 -= s22 * 683901;
    CARRY(s10, s11);
    CARRY(s11, s12);
    CARRY(s12, s13);
    CARRY(s13, s14);
    CARRY(s14, s15);
    CARRY(s15, s16);

    // --- s21 ---
    s9 += s21 * 666643;
    s10 += s21 * 470296;
    s11 += s21 * 654183;
    s12 -= s21 * 997805;
    s13 += s21 * 136657;
    s14 -= s21 * 683901;
    CARRY(s9, s10);
    CARRY(s10, s11);
    CARRY(s11, s12);
    CARRY(s12, s13);
    CARRY(s13, s14);
    CARRY(s14, s15);
    CARRY(s15, s16);

    // --- s20 ---
    s8 += s20 * 666643;
    s9 += s20 * 470296;
    s10 += s20 * 654183;
    s11 -= s20 * 997805;
    s12 += s20 * 136657;
    s13 -= s20 * 683901;
    CARRY(s8, s9);
    CARRY(s9, s10);
    CARRY(s10, s11);
    CARRY(s11, s12);
    CARRY(s12, s13);
    CARRY(s13, s14);
    CARRY(s14, s15);
    CARRY(s15, s16);

    // --- s19 ---
    s7 += s19 * 666643;
    s8 += s19 * 470296;
    s9 += s19 * 654183;
    s10 -= s19 * 997805;
    s11 += s19 * 136657;
    s12 -= s19 * 683901;
    CARRY(s7, s8);
    CARRY(s8, s9);
    CARRY(s9, s10);
    CARRY(s10, s11);
    CARRY(s11, s12);
    CARRY(s12, s13);
    CARRY(s13, s14);
    CARRY(s14, s15);
    CARRY(s15, s16);

    // --- s18 ---
    s6 += s18 * 666643;
    s7 += s18 * 470296;
    s8 += s18 * 654183;
    s9 -= s18 * 997805;
    s10 += s18 * 136657;
    s11 -= s18 * 683901;
    CARRY(s6, s7);
    CARRY(s7, s8);
    CARRY(s8, s9);
    CARRY(s9, s10);
    CARRY(s10, s11);
    CARRY(s11, s12);
    CARRY(s12, s13);
    CARRY(s13, s14);
    CARRY(s14, s15);
    CARRY(s15, s16);

    // --- s17 ---
    s5 += s17 * 666643;
    s6 += s17 * 470296;
    s7 += s17 * 654183;
    s8 -= s17 * 997805;
    s9 += s17 * 136657;
    s10 -= s17 * 683901;
    CARRY(s5, s6);
    CARRY(s6, s7);
    CARRY(s7, s8);
    CARRY(s8, s9);
    CARRY(s9, s10);
    CARRY(s10, s11);
    CARRY(s11, s12);
    CARRY(s12, s13);
    CARRY(s13, s14);
    CARRY(s14, s15);

    // s16 has accumulated contributions from s23..s17 and is now a 48-bit
    // signed value.  Its product with 666643 would be ~68 bits and overflow
    // int64_t.  Pre-carry s16 into a temporary s17 slot before folding.
    {
        int64_t c17 = s16 >> 21;
        s16 &= M; // s16 now fits in 21 bits
        s5 += c17 * 666643;
        s6 += c17 * 470296;
        s7 += c17 * 654183;
        s8 -= c17 * 997805;
        s9 += c17 * 136657;
        s10 -= c17 * 683901;
    }
    CARRY(s5, s6);
    CARRY(s6, s7);
    CARRY(s7, s8);
    CARRY(s8, s9);
    CARRY(s9, s10);
    CARRY(s10, s11);
    CARRY(s11, s12);
    CARRY(s12, s13);
    CARRY(s13, s14);
    CARRY(s14, s15);

    // --- s16 (now 21-bit, safe) ---
    s4 += s16 * 666643;
    s5 += s16 * 470296;
    s6 += s16 * 654183;
    s7 -= s16 * 997805;
    s8 += s16 * 136657;
    s9 -= s16 * 683901;
    CARRY(s4, s5);
    CARRY(s5, s6);
    CARRY(s6, s7);
    CARRY(s7, s8);
    CARRY(s8, s9);
    CARRY(s9, s10);
    CARRY(s10, s11);
    CARRY(s11, s12);
    CARRY(s12, s13);
    CARRY(s13, s14);

    // --- s15 ---
    s3 += s15 * 666643;
    s4 += s15 * 470296;
    s5 += s15 * 654183;
    s6 -= s15 * 997805;
    s7 += s15 * 136657;
    s8 -= s15 * 683901;
    CARRY(s3, s4);
    CARRY(s4, s5);
    CARRY(s5, s6);
    CARRY(s6, s7);
    CARRY(s7, s8);
    CARRY(s8, s9);
    CARRY(s9, s10);
    CARRY(s10, s11);
    CARRY(s11, s12);
    CARRY(s12, s13);

    // --- s14 ---
    s2 += s14 * 666643;
    s3 += s14 * 470296;
    s4 += s14 * 654183;
    s5 -= s14 * 997805;
    s6 += s14 * 136657;
    s7 -= s14 * 683901;
    CARRY(s2, s3);
    CARRY(s3, s4);
    CARRY(s4, s5);
    CARRY(s5, s6);
    CARRY(s6, s7);
    CARRY(s7, s8);
    CARRY(s8, s9);
    CARRY(s9, s10);
    CARRY(s10, s11);
    CARRY(s11, s12);

    // --- s13 ---
    s1 += s13 * 666643;
    s2 += s13 * 470296;
    s3 += s13 * 654183;
    s4 -= s13 * 997805;
    s5 += s13 * 136657;
    s6 -= s13 * 683901;
    CARRY(s1, s2);
    CARRY(s2, s3);
    CARRY(s3, s4);
    CARRY(s4, s5);
    CARRY(s5, s6);
    CARRY(s6, s7);
    CARRY(s7, s8);
    CARRY(s8, s9);
    CARRY(s9, s10);
    CARRY(s10, s11);

    // --- s12 (last fold) + full sequential carry ---
    s0 += s12 * 666643;
    s1 += s12 * 470296;
    s2 += s12 * 654183;
    s3 -= s12 * 997805;
    s4 += s12 * 136657;
    s5 -= s12 * 683901;
    CARRY(s0, s1);
    CARRY(s1, s2);
    CARRY(s2, s3);
    CARRY(s3, s4);
    CARRY(s4, s5);
    CARRY(s5, s6);
    CARRY(s6, s7);
    CARRY(s7, s8);
    CARRY(s8, s9);
    CARRY(s9, s10);
    CARRY(s10, s11);

    // s11 may still overflow 21 bits; fold its carry back one final time.
    {
        int64_t c12 = s11 >> 21;
        s11 &= M;
        s0 += c12 * 666643;
        s1 += c12 * 470296;
        s2 += c12 * 654183;
        s3 -= c12 * 997805;
        s4 += c12 * 136657;
        s5 -= c12 * 683901;
    }
    CARRY(s0, s1);
    CARRY(s1, s2);
    CARRY(s2, s3);
    CARRY(s3, s4);
    CARRY(s4, s5);
    CARRY(s5, s6);
    CARRY(s6, s7);
    CARRY(s7, s8);
    CARRY(s8, s9);
    CARRY(s9, s10);
    CARRY(s10, s11);

#undef CARRY

    // Encode 12 x 21-bit limbs as a 252-bit little-endian integer in s[0..31].
    // Each limb s_k occupies bits [k*21 .. k*21+20]; adjacent limbs share bytes.
    s[0] = (uint8_t)s0;
    s[1] = (uint8_t)(s0 >> 8);
    s[2] = (uint8_t)((s0 >> 16) | (s1 << 5));
    s[3] = (uint8_t)(s1 >> 3);
    s[4] = (uint8_t)(s1 >> 11);
    s[5] = (uint8_t)((s1 >> 19) | (s2 << 2));
    s[6] = (uint8_t)(s2 >> 6);
    s[7] = (uint8_t)((s2 >> 14) | (s3 << 7));
    s[8] = (uint8_t)(s3 >> 1);
    s[9] = (uint8_t)(s3 >> 9);
    s[10] = (uint8_t)((s3 >> 17) | (s4 << 4));
    s[11] = (uint8_t)(s4 >> 4);
    s[12] = (uint8_t)(s4 >> 12);
    s[13] = (uint8_t)((s4 >> 20) | (s5 << 1));
    s[14] = (uint8_t)(s5 >> 7);
    s[15] = (uint8_t)((s5 >> 15) | (s6 << 6));
    s[16] = (uint8_t)(s6 >> 2);
    s[17] = (uint8_t)(s6 >> 10);
    s[18] = (uint8_t)((s6 >> 18) | (s7 << 3));
    s[19] = (uint8_t)(s7 >> 5);
    s[20] = (uint8_t)(s7 >> 13);
    s[21] = (uint8_t)s8;
    s[22] = (uint8_t)(s8 >> 8);
    s[23] = (uint8_t)((s8 >> 16) | (s9 << 5));
    s[24] = (uint8_t)(s9 >> 3);
    s[25] = (uint8_t)(s9 >> 11);
    s[26] = (uint8_t)((s9 >> 19) | (s10 << 2));
    s[27] = (uint8_t)(s10 >> 6);
    s[28] = (uint8_t)((s10 >> 14) | (s11 << 7));
    s[29] = (uint8_t)(s11 >> 1);
    s[30] = (uint8_t)(s11 >> 9);
    s[31] = (uint8_t)(s11 >> 17);
}

void ge_scalarmult(ge_p3& r, const uint8_t sc[32], const ge_p3& A) noexcept {
    fe_0(r.X);
    fe_1(r.Y);
    fe_1(r.Z);
    fe_0(r.T);
    for (int i = 255; i >= 0; --i) {
        ge_p2 p2;
        ge_p3_to_p2(p2, r);
        ge_p1p1 t;
        ge_p2_dbl(t, p2);
        ge_p1p1_to_p3(r, t);
        if ((sc[i >> 3] >> (i & 7)) & 1) {
            ge_p1p1 q;
            ge_add(q, r, A);
            ge_p1p1_to_p3(r, q);
        }
    }
}

// Base-point encoding: RFC 8032, Section 5.1, Appendix B (y-coord, sign bit 0)
const uint8_t kBY[32] = {0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
                         0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
                         0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public entry point: RFC 8032 SS5.1.7 Verify
// ---------------------------------------------------------------------------
bool ed25519_verify(const uint8_t sig[64], const uint8_t* msg, size_t msg_len, const uint8_t pub_key[32]) noexcept {
    // Step 1: reject obviously invalid signature (cofactor check on S)
    if (sig[63] & 0xe0)
        return false;

    // Step 2: decode public key A
    ge_p3 A;
    if (!ge_frombytes(A, pub_key))
        return false;

    // Step 3: compute k = SHA-512(R || A || M) and reduce mod l
    uint8_t kh[64];
    {
        SHA512 ctx;
        ctx.update(sig, 32); // R (first 32 bytes of sig)
        ctx.update(pub_key, 32);
        ctx.update(msg, msg_len);
        ctx.final(kh);
    }
    sc_reduce(kh);

    // Step 4: decode R from signature
    ge_p3 R;
    if (!ge_frombytes(R, sig))
        return false;

    // Step 5: decode S from signature
    uint8_t S[32];
    memcpy(S, sig + 32, 32);

    // Step 6: decode base point B
    ge_p3 B;
    if (!ge_frombytes(B, kBY))
        return false;

    // Step 7: compute check = [S]B - [k]A
    // Negate A: flip X and T, keep Y and Z
    ge_p3 negA;
    fe_copy(negA.X, A.X);
    fe_copy(negA.Y, A.Y);
    fe_copy(negA.Z, A.Z);
    fe_copy(negA.T, A.T);
    fe_neg(negA.X, negA.X);
    fe_neg(negA.T, negA.T);

    ge_p3 SB, kA;
    ge_scalarmult(SB, S, B);
    ge_scalarmult(kA, kh, negA);

    ge_p1p1 ch11;
    ge_add(ch11, SB, kA);
    ge_p3 ch;
    ge_p1p1_to_p3(ch, ch11);

    // Step 8: encode result and compare to R from signature
    ge_p2 ch2;
    ge_p3_to_p2(ch2, ch);
    fe iz;
    fe_invert(iz, ch2.Z);
    fe rx, ry;
    fe_mul(rx, ch2.X, iz);
    fe_mul(ry, ch2.Y, iz);
    uint8_t enc[32], rxb[32];
    fe_tobytes(enc, ry);
    fe_tobytes(rxb, rx);
    enc[31] |= (rxb[0] & 1) << 7;
    return memcmp(enc, sig, 32) == 0;
}

} // namespace exosnap::update
