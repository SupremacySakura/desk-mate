#include "deskmate/platform/update_crypto.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace deskmate::platform {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

constexpr std::array<std::uint64_t, 80> kSha512K = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

inline std::uint32_t rotr32(std::uint32_t x, std::uint32_t n) noexcept {
  return (x >> n) | (x << (32u - n));
}

inline std::uint64_t rotr64(std::uint64_t x, std::uint64_t n) noexcept {
  return (x >> n) | (x << (64u - n));
}

inline std::uint32_t load_be32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) |
         (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) |
         static_cast<std::uint32_t>(p[3]);
}

inline std::uint64_t load_be64(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint64_t>(p[0]) << 56) |
         (static_cast<std::uint64_t>(p[1]) << 48) |
         (static_cast<std::uint64_t>(p[2]) << 40) |
         (static_cast<std::uint64_t>(p[3]) << 32) |
         (static_cast<std::uint64_t>(p[4]) << 24) |
         (static_cast<std::uint64_t>(p[5]) << 16) |
         (static_cast<std::uint64_t>(p[6]) << 8) |
         static_cast<std::uint64_t>(p[7]);
}

// --- Portable TweetNaCl-style Ed25519 verification (no __int128) ---

using gf = std::int64_t[16];

constexpr gf kGfZero = {0};
constexpr gf kGfOne = {1};
constexpr gf kCurveD = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203,
};
constexpr gf kCurveD2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406,
};
constexpr gf kCurveX = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169,
};
constexpr gf kCurveY = {
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
};
constexpr gf kSqrtM1 = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83,
};

void gf_set(gf o, const gf a) noexcept {
  for (int i = 0; i < 16; ++i) {
    o[i] = a[i];
  }
}

void gf_add(gf o, const gf a, const gf b) noexcept {
  for (int i = 0; i < 16; ++i) {
    o[i] = a[i] + b[i];
  }
}

void gf_sub(gf o, const gf a, const gf b) noexcept {
  for (int i = 0; i < 16; ++i) {
    o[i] = a[i] - b[i];
  }
}

void gf_carry(gf o) noexcept {
  for (int i = 0; i < 16; ++i) {
    o[i] += (1LL << 16);
    const std::int64_t c = o[i] >> 16;
    o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
    o[i] -= c << 16;
  }
}

void gf_sel(gf p, gf q, int b) noexcept {
  const std::int64_t c = ~(static_cast<std::int64_t>(b) - 1);
  for (int i = 0; i < 16; ++i) {
    const std::int64_t t = c & (p[i] ^ q[i]);
    p[i] ^= t;
    q[i] ^= t;
  }
}

void gf_mul(gf o, const gf a, const gf b) noexcept {
  std::int64_t t[31]{};
  for (int i = 0; i < 16; ++i) {
    for (int j = 0; j < 16; ++j) {
      t[i + j] += a[i] * b[j];
    }
  }
  for (int i = 0; i < 15; ++i) {
    t[i] += 38 * t[i + 16];
  }
  for (int i = 0; i < 16; ++i) {
    o[i] = t[i];
  }
  gf_carry(o);
  gf_carry(o);
}

void gf_sq(gf o, const gf a) noexcept { gf_mul(o, a, a); }

void gf_inv(gf o, const gf i_in) noexcept {
  gf c;
  gf_set(c, i_in);
  for (int a = 253; a >= 0; --a) {
    gf_sq(c, c);
    if (a != 2 && a != 4) {
      gf_mul(c, c, i_in);
    }
  }
  gf_set(o, c);
}

void gf_pow2523(gf o, const gf i_in) noexcept {
  gf c;
  gf_set(c, i_in);
  for (int a = 250; a >= 0; --a) {
    gf_sq(c, c);
    if (a != 1) {
      gf_mul(c, c, i_in);
    }
  }
  gf_set(o, c);
}

void gf_pack(std::uint8_t* o, const gf n) noexcept {
  gf m, t;
  gf_set(t, n);
  gf_carry(t);
  gf_carry(t);
  gf_carry(t);
  for (int j = 0; j < 2; ++j) {
    m[0] = t[0] - 0xffed;
    for (int i = 1; i < 15; ++i) {
      m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
      m[i - 1] &= 0xffff;
    }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    const int b = static_cast<int>((m[15] >> 16) & 1);
    m[14] &= 0xffff;
    gf_sel(t, m, 1 - b);
  }
  for (int i = 0; i < 16; ++i) {
    o[2 * i] = static_cast<std::uint8_t>(t[i] & 0xff);
    o[2 * i + 1] = static_cast<std::uint8_t>((t[i] >> 8) & 0xff);
  }
}

void gf_unpack(gf o, const std::uint8_t* n) noexcept {
  for (int i = 0; i < 16; ++i) {
    o[i] = static_cast<std::int64_t>(n[2 * i]) +
           (static_cast<std::int64_t>(n[2 * i + 1]) << 8);
  }
  o[15] &= 0x7fff;
}

int gf_neq(const gf a, const gf b) noexcept {
  std::uint8_t c[32]{};
  std::uint8_t d[32]{};
  gf_pack(c, a);
  gf_pack(d, b);
  std::uint8_t diff = 0;
  for (int i = 0; i < 32; ++i) {
    diff |= static_cast<std::uint8_t>(c[i] ^ d[i]);
  }
  return diff != 0 ? 1 : 0;
}

int gf_parity(const gf a) noexcept {
  std::uint8_t d[32]{};
  gf_pack(d, a);
  return d[0] & 1;
}

void point_add(gf p[4], const gf q[4]) noexcept {
  gf a, b, c, d, t, e, f, g, h;
  gf_sub(a, p[1], p[0]);
  gf_sub(t, q[1], q[0]);
  gf_mul(a, a, t);
  gf_add(b, p[0], p[1]);
  gf_add(t, q[0], q[1]);
  gf_mul(b, b, t);
  gf_mul(c, p[3], q[3]);
  gf_mul(c, c, kCurveD2);
  gf_mul(d, p[2], q[2]);
  gf_add(d, d, d);
  gf_sub(e, b, a);
  gf_sub(f, d, c);
  gf_add(g, d, c);
  gf_add(h, b, a);

  gf_mul(p[0], e, f);
  gf_mul(p[1], h, g);
  gf_mul(p[2], g, f);
  gf_mul(p[3], e, h);
}

void point_cswap(gf p[4], gf q[4], int b) noexcept {
  for (int i = 0; i < 4; ++i) {
    gf_sel(p[i], q[i], b);
  }
}

void point_pack(std::uint8_t* r, const gf p[4]) noexcept {
  gf tx, ty, zi;
  gf_inv(zi, p[2]);
  gf_mul(tx, p[0], zi);
  gf_mul(ty, p[1], zi);
  gf_pack(r, ty);
  r[31] ^= static_cast<std::uint8_t>(gf_parity(tx) << 7);
}

void point_scalarmult(gf p[4], const gf q[4], const std::uint8_t* s) noexcept {
  gf q_copy[4];
  for (int i = 0; i < 4; ++i) {
    gf_set(q_copy[i], q[i]);
  }
  gf_set(p[0], kGfZero);
  gf_set(p[1], kGfOne);
  gf_set(p[2], kGfOne);
  gf_set(p[3], kGfZero);
  for (int i = 255; i >= 0; --i) {
    const int b = (s[i / 8] >> (i & 7)) & 1;
    point_cswap(p, q_copy, b);
    point_add(q_copy, p);
    point_add(p, p);
    point_cswap(p, q_copy, b);
  }
}

void point_scalarbase(gf p[4], const std::uint8_t* s) noexcept {
  gf q[4];
  gf_set(q[0], kCurveX);
  gf_set(q[1], kCurveY);
  gf_set(q[2], kGfOne);
  gf_mul(q[3], kCurveX, kCurveY);
  point_scalarmult(p, q, s);
}

// RFC 8032 canonical y < 2^255 - 19 check on 32-byte little-endian point encoding.
bool encoded_y_is_canonical(const std::uint8_t* enc) noexcept {
  std::uint8_t y[32]{};
  std::memcpy(y, enc, 32);
  y[31] &= 0x7fu;
  // p = 2^255 - 19: in LE bytes, [0]=0xed, [1..30]=0xff, [31]=0x7f
  static constexpr std::array<std::uint8_t, 32> kPrimeLE = {
      0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
  };
  for (int i = 31; i >= 0; --i) {
    if (y[i] < kPrimeLE[i]) {
      return true;
    }
    if (y[i] > kPrimeLE[i]) {
      return false;
    }
  }
  return false;  // equal to p
}

// Unpacks encoded point into r = -A (negated x) per TweetNaCl verification.
bool point_unpackneg(gf r[4], const std::uint8_t* p) noexcept {
  if (!encoded_y_is_canonical(p)) {
    return false;
  }
  gf t, chk, num, den, den2, den4, den6;
  gf_set(r[2], kGfOne);
  gf_unpack(r[1], p);
  gf_sq(num, r[1]);
  gf_mul(den, num, kCurveD);
  gf_sub(num, num, r[2]);
  gf_add(den, r[2], den);

  gf_sq(den2, den);
  gf_sq(den4, den2);
  gf_mul(den6, den4, den2);
  gf_mul(t, den6, den);
  gf_mul(t, t, num);

  gf_pow2523(t, t);
  gf_mul(t, t, num);
  gf_mul(t, t, den);
  gf_mul(t, t, den);
  gf_mul(r[0], t, den);

  gf_sq(chk, r[0]);
  gf_mul(chk, chk, den);
  if (gf_neq(chk, num) != 0) {
    gf_mul(r[0], r[0], kSqrtM1);
  }

  gf_sq(chk, r[0]);
  gf_mul(chk, chk, den);
  if (gf_neq(chk, num) != 0) {
    return false;
  }

  const int sign = p[31] >> 7;
  if (gf_neq(r[0], kGfZero) == 0 && sign == 1) {
    return false;
  }
  if (gf_parity(r[0]) == sign) {
    gf_sub(r[0], kGfZero, r[0]);
  }
  gf_mul(r[3], r[0], r[1]);
  return true;
}

bool scalar_is_canonical(const std::uint8_t* s) noexcept {
  // L = 2^252 + 27742317777372353535851937790883648493 in little-endian:
  static constexpr std::array<std::uint8_t, 32> kOrderLE = {
      0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
      0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
  };
  for (int i = 31; i >= 0; --i) {
    if (s[i] < kOrderLE[i]) {
      return true;
    }
    if (s[i] > kOrderLE[i]) {
      return false;
    }
  }
  return false;  // equal to L
}

void reduce_mod_l(std::uint8_t* r, std::int64_t x[64]) noexcept {
  static constexpr std::uint8_t kL[32] = {
      0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
      0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
  };
  for (int i = 63; i >= 32; --i) {
    std::int64_t carry = 0;
    const int j_end = i - 12;
    for (int j = i - 32; j < j_end; ++j) {
      x[j] += carry - 16 * x[i] * static_cast<std::int64_t>(kL[j - (i - 32)]);
      carry = (x[j] + 128) >> 8;
      x[j] -= carry * 256;
    }
    x[j_end] += carry;
    x[i] = 0;
  }
  std::int64_t carry = 0;
  for (int j = 0; j < 32; ++j) {
    x[j] += carry - (x[31] >> 4) * static_cast<std::int64_t>(kL[j]);
    carry = x[j] >> 8;
    x[j] &= 255;
  }
  for (int j = 0; j < 32; ++j) {
    x[j] -= carry * static_cast<std::int64_t>(kL[j]);
  }
  for (int i = 0; i < 32; ++i) {
    x[i + 1] += x[i] >> 8;
    r[i] = static_cast<std::uint8_t>(x[i] & 255);
  }
}

void mod_l(std::uint8_t* r, const std::uint8_t* h64) noexcept {
  std::int64_t x[64]{};
  for (int i = 0; i < 64; ++i) {
    x[i] = static_cast<std::int64_t>(h64[i]);
  }
  reduce_mod_l(r, x);
}

int hex_nibble(char ch) noexcept {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

}  // namespace

Sha256Hasher::Sha256Hasher() noexcept
    : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u} {}

void Sha256Hasher::process_block(const std::uint8_t* block) noexcept {
  std::uint32_t w[64]{};
  for (int i = 0; i < 16; ++i) {
    w[i] = load_be32(block + i * 4);
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 =
        rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 =
        rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (int i = 0; i < 64; ++i) {
    const std::uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + S1 + ch + kSha256K[i] + w[i];
    const std::uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = S0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256Hasher::update(std::span<const std::uint8_t> data) noexcept {
  const std::uint8_t* ptr = data.data();
  std::size_t rem = data.size();
  total_bytes_ += static_cast<std::uint64_t>(rem);

  if (buffer_used_ > 0) {
    const std::size_t take = std::min<std::size_t>(rem, 64 - buffer_used_);
    std::memcpy(buffer_.data() + buffer_used_, ptr, take);
    buffer_used_ += take;
    ptr += take;
    rem -= take;
    if (buffer_used_ == 64) {
      process_block(buffer_.data());
      buffer_used_ = 0;
    }
  }
  while (rem >= 64) {
    process_block(ptr);
    ptr += 64;
    rem -= 64;
  }
  if (rem > 0) {
    std::memcpy(buffer_.data(), ptr, rem);
    buffer_used_ = rem;
  }
}

std::array<std::uint8_t, 32> Sha256Hasher::finalize() noexcept {
  const std::uint64_t bit_len = total_bytes_ * 8ULL;
  std::uint8_t pad = 0x80u;
  buffer_[buffer_used_++] = pad;
  if (buffer_used_ > 56) {
    std::memset(buffer_.data() + buffer_used_, 0, 64 - buffer_used_);
    process_block(buffer_.data());
    buffer_used_ = 0;
  }
  std::memset(buffer_.data() + buffer_used_, 0, 56 - buffer_used_);
  for (int i = 0; i < 8; ++i) {
    buffer_[56 + i] =
        static_cast<std::uint8_t>((bit_len >> (56 - i * 8)) & 0xffu);
  }
  process_block(buffer_.data());

  std::array<std::uint8_t, 32> digest{};
  for (int i = 0; i < 8; ++i) {
    digest[i * 4 + 0] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xffu);
    digest[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xffu);
    digest[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xffu);
    digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xffu);
  }
  return digest;
}

std::array<std::uint8_t, 32> sha256(
    std::span<const std::uint8_t> data) noexcept {
  Sha256Hasher hasher;
  hasher.update(data);
  return hasher.finalize();
}

bool sha256_file(const std::string& path,
                 std::array<std::uint8_t, 32>& out_hash,
                 std::uint64_t& out_size) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return false;
  }
  Sha256Hasher hasher;
  out_size = 0;
  std::array<std::uint8_t, 65536> buf{};
  while (in) {
    in.read(reinterpret_cast<char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
    const std::streamsize got = in.gcount();
    if (got > 0) {
      out_size += static_cast<std::uint64_t>(got);
      hasher.update(std::span<const std::uint8_t>(
          buf.data(), static_cast<std::size_t>(got)));
    }
  }
  out_hash = hasher.finalize();
  return true;
}

std::array<std::uint8_t, 64> sha512(
    std::span<const std::uint8_t> data) noexcept {
  std::array<std::uint64_t, 8> state = {
      0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
      0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
      0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
  };

  const auto compress = [&](const std::uint8_t* block) {
    std::uint64_t w[80]{};
    for (int i = 0; i < 16; ++i) {
      w[i] = load_be64(block + i * 8);
    }
    for (int i = 16; i < 80; ++i) {
      const std::uint64_t s0 =
          rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
      const std::uint64_t s1 =
          rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint64_t a = state[0];
    std::uint64_t b = state[1];
    std::uint64_t c = state[2];
    std::uint64_t d = state[3];
    std::uint64_t e = state[4];
    std::uint64_t f = state[5];
    std::uint64_t g = state[6];
    std::uint64_t h = state[7];

    for (int i = 0; i < 80; ++i) {
      const std::uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
      const std::uint64_t ch = (e & f) ^ ((~e) & g);
      const std::uint64_t temp1 = h + S1 + ch + kSha512K[i] + w[i];
      const std::uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
      const std::uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint64_t temp2 = S0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  };

  std::size_t pos = 0;
  while (data.size() - pos >= 128) {
    compress(data.data() + pos);
    pos += 128;
  }

  std::array<std::uint8_t, 256> tail{};
  const std::size_t rem = data.size() - pos;
  if (rem > 0) {
    std::memcpy(tail.data(), data.data() + pos, rem);
  }
  tail[rem] = 0x80u;
  const std::size_t pad_blocks = (rem + 1 + 16 <= 128) ? 1 : 2;
  const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8ULL;
  const std::size_t len_off = pad_blocks * 128 - 8;
  for (int i = 0; i < 8; ++i) {
    tail[len_off + i] =
        static_cast<std::uint8_t>((bit_len >> (56 - i * 8)) & 0xffu);
  }
  compress(tail.data());
  if (pad_blocks == 2) {
    compress(tail.data() + 128);
  }

  std::array<std::uint8_t, 64> digest{};
  for (int i = 0; i < 8; ++i) {
    for (int j = 0; j < 8; ++j) {
      digest[i * 8 + j] =
          static_cast<std::uint8_t>((state[i] >> (56 - j * 8)) & 0xffu);
    }
  }
  return digest;
}

bool ed25519_verify(std::span<const std::uint8_t, 32> pubkey,
                    std::span<const std::uint8_t> message,
                    std::span<const std::uint8_t, 64> signature) noexcept {
  if (!scalar_is_canonical(signature.data() + 32)) {
    return false;
  }
  if (!encoded_y_is_canonical(signature.data())) {
    return false;
  }

  gf p[4], q[4];
  if (!point_unpackneg(q, pubkey.data())) {
    return false;
  }

  std::vector<std::uint8_t> h_input;
  h_input.reserve(64 + message.size());
  h_input.insert(h_input.end(), signature.data(), signature.data() + 32);
  h_input.insert(h_input.end(), pubkey.data(), pubkey.data() + 32);
  h_input.insert(h_input.end(), message.begin(), message.end());
  const auto h_digest = sha512(h_input);

  std::uint8_t h_mod_l[32]{};
  mod_l(h_mod_l, h_digest.data());

  point_scalarmult(p, q, h_mod_l);
  point_scalarbase(q, signature.data() + 32);
  point_add(p, q);

  std::uint8_t r_check[32]{};
  point_pack(r_check, p);
  std::uint8_t diff = 0;
  for (int i = 0; i < 32; ++i) {
    diff |= static_cast<std::uint8_t>(r_check[i] ^ signature[i]);
  }
  return diff == 0;
}

std::string hex_encode(std::span<const std::uint8_t> bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::uint8_t b : bytes) {
    out.push_back(kHex[(b >> 4) & 0x0f]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

bool hex_decode(std::string_view hex, std::vector<std::uint8_t>& out) {
  if ((hex.size() % 2) != 0) {
    return false;
  }
  out.clear();
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = hex_nibble(hex[i]);
    const int lo = hex_nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      out.clear();
      return false;
    }
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return true;
}

}  // namespace deskmate::platform
