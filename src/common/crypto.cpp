#include "common/crypto.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace acfa::crypto {
namespace {

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

void check(NTSTATUS st, const char* what) {
  if (!NT_SUCCESS(st)) throw std::runtime_error(strprintf("bcrypt %s failed: 0x%08lx", what, (unsigned long)st));
}

template <size_t N>
std::array<u8, N> hash_with(const wchar_t* algname, const void* data, size_t len) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  check(BCryptOpenAlgorithmProvider(&alg, algname, nullptr, 0), "open hash");
  std::array<u8, N> out{};
  NTSTATUS st = BCryptHash(alg, nullptr, 0, (PUCHAR)data, (ULONG)len, out.data(), (ULONG)N);
  BCryptCloseAlgorithmProvider(alg, 0);
  check(st, "hash");
  return out;
}

}  // namespace

std::array<u8, 20> sha1(const void* data, size_t len) { return hash_with<20>(BCRYPT_SHA1_ALGORITHM, data, len); }
std::array<u8, 32> sha256(const void* data, size_t len) { return hash_with<32>(BCRYPT_SHA256_ALGORITHM, data, len); }

std::string sha256_file_hex(const std::string& path) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE h = nullptr;
  check(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0), "open sha256");
  check(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0), "create hash");
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::vector<char> buf(1 << 20);
  while (f) {
    f.read(buf.data(), (std::streamsize)buf.size());
    std::streamsize n = f.gcount();
    if (n > 0) check(BCryptHashData(h, (PUCHAR)buf.data(), (ULONG)n, 0), "hash data");
  }
  u8 out[32];
  check(BCryptFinishHash(h, out, 32, 0), "finish hash");
  BCryptDestroyHash(h);
  BCryptCloseAlgorithmProvider(alg, 0);
  return hex(out, 32);
}

}  // namespace acfa::crypto

#else  // POSIX: self-contained SHA-1/SHA-256 (no OpenSSL/CNG dependency)

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace acfa::crypto {
namespace {

inline u32 rotl32(u32 x, int n) { return (x << n) | (x >> (32 - n)); }
inline u32 rotr32(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

// ---- SHA-1 ----
struct Sha1 {
  u32 h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
  u8 buf[64];
  size_t buf_len = 0;
  u64 total = 0;

  static void block(u32 h[5], const u8* p) {
    u32 w[80];
    for (int i = 0; i < 16; ++i)
      w[i] = (u32(p[4 * i]) << 24) | (u32(p[4 * i + 1]) << 16) | (u32(p[4 * i + 2]) << 8) | u32(p[4 * i + 3]);
    for (int i = 16; i < 80; ++i) w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      u32 f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5a827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ed9eba1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8f1bbcdcu;
      } else {
        f = b ^ c ^ d;
        k = 0xca62c1d6u;
      }
      u32 t = rotl32(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotl32(b, 30);
      b = a;
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }

  void update(const void* data, size_t n) {
    const u8* p = static_cast<const u8*>(data);
    total += n;
    while (n) {
      size_t take = 64 - buf_len;
      if (take > n) take = n;
      std::memcpy(buf + buf_len, p, take);
      buf_len += take;
      p += take;
      n -= take;
      if (buf_len == 64) {
        block(h, buf);
        buf_len = 0;
      }
    }
  }

  std::array<u8, 20> final() {
    u64 bits = total * 8;
    u8 pad = 0x80;
    update(&pad, 1);
    u8 zero = 0;
    while (buf_len != 56) update(&zero, 1);
    u8 len[8];
    for (int i = 0; i < 8; ++i) len[i] = u8(bits >> (56 - 8 * i));
    update(len, 8);
    std::array<u8, 20> out{};
    for (int i = 0; i < 5; ++i) {
      out[4 * i] = u8(h[i] >> 24);
      out[4 * i + 1] = u8(h[i] >> 16);
      out[4 * i + 2] = u8(h[i] >> 8);
      out[4 * i + 3] = u8(h[i]);
    }
    return out;
  }
};

// ---- SHA-256 ----
struct Sha256 {
  u32 h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  u8 buf[64];
  size_t buf_len = 0;
  u64 total = 0;

  static constexpr u32 k[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
      0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
      0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
      0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
      0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
      0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
      0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

  static void block(u32 h[8], const u8* p) {
    u32 w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (u32(p[4 * i]) << 24) | (u32(p[4 * i + 1]) << 16) | (u32(p[4 * i + 2]) << 8) | u32(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
      u32 s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
      u32 s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      u32 s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
      u32 ch = (e & f) ^ (~e & g);
      u32 t1 = hh + s1 + ch + k[i] + w[i];
      u32 s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
      u32 maj = (a & b) ^ (a & c) ^ (b & c);
      u32 t2 = s0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }

  void update(const void* data, size_t n) {
    const u8* p = static_cast<const u8*>(data);
    total += n;
    while (n) {
      size_t take = 64 - buf_len;
      if (take > n) take = n;
      std::memcpy(buf + buf_len, p, take);
      buf_len += take;
      p += take;
      n -= take;
      if (buf_len == 64) {
        block(h, buf);
        buf_len = 0;
      }
    }
  }

  std::array<u8, 32> final() {
    u64 bits = total * 8;
    u8 pad = 0x80;
    update(&pad, 1);
    u8 zero = 0;
    while (buf_len != 56) update(&zero, 1);
    u8 len[8];
    for (int i = 0; i < 8; ++i) len[i] = u8(bits >> (56 - 8 * i));
    update(len, 8);
    std::array<u8, 32> out{};
    for (int i = 0; i < 8; ++i) {
      out[4 * i] = u8(h[i] >> 24);
      out[4 * i + 1] = u8(h[i] >> 16);
      out[4 * i + 2] = u8(h[i] >> 8);
      out[4 * i + 3] = u8(h[i]);
    }
    return out;
  }
};

}  // namespace

std::array<u8, 20> sha1(const void* data, size_t len) {
  Sha1 s;
  s.update(data, len);
  return s.final();
}

std::array<u8, 32> sha256(const void* data, size_t len) {
  Sha256 s;
  s.update(data, len);
  return s.final();
}

std::string sha256_file_hex(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  Sha256 s;
  std::vector<char> buf(1 << 20);
  while (f) {
    f.read(buf.data(), (std::streamsize)buf.size());
    std::streamsize n = f.gcount();
    if (n > 0) s.update(buf.data(), size_t(n));
  }
  return hex(s.final().data(), 32);
}

}  // namespace acfa::crypto

#endif
