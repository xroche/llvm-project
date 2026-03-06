//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Test that std::to_chars and std::from_chars work for _BitInt(N > 128).
// Wide _BitInt is only supported on x86.

// UNSUPPORTED: c++03, c++11, c++14
// REQUIRES: target={{x86.*}}

#include <cassert>
#include <charconv>
#include <cstring>
#include <string_view>

// Helper: to_chars then from_chars roundtrip
template <class T>
void test_roundtrip(T value, int base = 10) {
  char buf[512];
  auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value, base);
  assert(ec == std::errc{});

  T result{};
  auto [ptr2, ec2] = std::from_chars(buf, ptr, result, base);
  assert(ec2 == std::errc{});
  assert(ptr2 == ptr);
  assert(result == value);
}

// Helper: to_chars (default base 10) roundtrip
template <class T>
void test_roundtrip_default(T value) {
  char buf[512];
  auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
  assert(ec == std::errc{});

  T result{};
  auto [ptr2, ec2] = std::from_chars(buf, ptr, result);
  assert(ec2 == std::errc{});
  assert(ptr2 == ptr);
  assert(result == value);
}

// Helper: to_chars and verify against expected string
template <class T>
void test_to_chars(T value, const char* expected, int base = 10) {
  char buf[512];
  auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value, base);
  assert(ec == std::errc{});
  assert(std::string_view(buf, ptr - buf) == expected);
}

#if __BITINT_MAXWIDTH__ >= 256

void test_256bit() {
  using U256 = unsigned _BitInt(256);
  using S256 = _BitInt(256);

  // Basic values
  test_roundtrip(U256(0));
  test_roundtrip(U256(1));
  test_roundtrip(U256(42));
  test_roundtrip(U256(1000000000));

  test_roundtrip_default(U256(0));
  test_roundtrip_default(U256(42));
  test_roundtrip_default(U256(1000000000));

  // Signed
  test_roundtrip(S256(0));
  test_roundtrip(S256(42));
  test_roundtrip(S256(-42));
  test_roundtrip(S256(-1));

  test_roundtrip_default(S256(-42));

  // Large value: 2^200
  // Python: 2**200 = 1606938044258990275541962092341162602522202993782792835301376
  {
    U256 v = U256(1) << 200;
    test_to_chars(v, "1606938044258990275541962092341162602522202993782792835301376");
    test_roundtrip(v);
  }

  // Max unsigned 256-bit:
  // Python: 2**256 - 1 =
  // 115792089237316195423570985008687907853269984665640564039457584007913129639935
  {
    U256 v = ~U256(0);
    test_to_chars(v, "115792089237316195423570985008687907853269984665640564039457584007913129639935");
    test_roundtrip(v);
  }

  // Non-base-10
  test_roundtrip(U256(0xFF), 16);
  test_roundtrip(U256(0777), 8);
  test_roundtrip(U256(0b1010), 2);

  // Large hex value
  {
    U256 v = U256(1) << 200;
    test_roundtrip(v, 16);
    test_roundtrip(v, 2);
    test_roundtrip(v, 8);
    test_roundtrip(v, 36);
  }

  // Signed with non-base-10
  test_roundtrip(S256(-42), 16);
  test_roundtrip(S256(-42), 2);

  // Buffer too small
  {
    char buf[5];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), U256(1000000));
    assert(ec == std::errc::value_too_large);
  }
}

#endif // __BITINT_MAXWIDTH__ >= 256

#if __BITINT_MAXWIDTH__ >= 512

void test_512bit() {
  using U512 = unsigned _BitInt(512);

  test_roundtrip(U512(0));
  test_roundtrip(U512(42));
  test_roundtrip_default(U512(42));

  // Large value roundtrip
  U512 v = U512(1) << 400;
  test_roundtrip(v);
  test_roundtrip(v, 16);
}

#endif // __BITINT_MAXWIDTH__ >= 512

// Also test that narrow _BitInt still works through existing paths
void test_narrow_bitint() {
  using U32 = unsigned _BitInt(32);
  using U64 = unsigned _BitInt(64);
  using S32 = _BitInt(32);
  using S64 = _BitInt(64);

  test_roundtrip(U32(42));
  test_roundtrip(U64(1000000));
  test_roundtrip(S32(-42));
  test_roundtrip(S64(-42));

  test_roundtrip(U32(42), 16);
  test_roundtrip(U64(42), 2);

  // _BitInt(128) goes through __make_32_64_or_128_bit_t which requires __int128
#if _LIBCPP_HAS_INT128
  using U128 = unsigned _BitInt(128);
  using S128 = _BitInt(128);

  test_roundtrip(U128(42));
  test_roundtrip(S128(-42));
  test_roundtrip(U128(42), 8);
#endif
}

int main(int, char**) {
  test_narrow_bitint();

#if __BITINT_MAXWIDTH__ >= 256
  test_256bit();
#endif

#if __BITINT_MAXWIDTH__ >= 512
  test_512bit();
#endif

  return 0;
}
