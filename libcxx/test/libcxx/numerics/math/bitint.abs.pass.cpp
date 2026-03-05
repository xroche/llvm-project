//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Test that std::abs works for _BitInt(N) and __int128 types via <cmath>.

// UNSUPPORTED: c++03, c++11, c++14, c++17

#include <cassert>
#include <cmath>

template <int N>
void test_signed_bitint() {
  using T = _BitInt(N);
  // abs(0) == 0
  assert(std::abs(T(0)) == T(0));
  // abs(positive) == positive
  assert(std::abs(T(1)) == T(1));
  assert(std::abs(T(42)) == T(42));
  // abs(negative) == positive
  assert(std::abs(T(-1)) == T(1));
  assert(std::abs(T(-42)) == T(42));
}

int main(int, char**) {
  // _BitInt(N) with N < sizeof(int)*8 are too small for std::abs — they
  // don't implicitly promote to int (unlike short/signed char), and our
  // template only covers sizeof(_Tp) >= sizeof(int).
  test_signed_bitint<32>();
  test_signed_bitint<64>();
  test_signed_bitint<128>();

  // Odd widths (>= 32 bits)
  test_signed_bitint<33>();
  test_signed_bitint<65>();

#if _LIBCPP_HAS_INT128
  // __int128 should also work
  assert(std::abs((__int128_t)0) == 0);
  assert(std::abs((__int128_t)42) == 42);
  assert(std::abs((__int128_t)-42) == 42);
  assert(std::abs((__int128_t)-1) == 1);
#endif

  // Wide _BitInt (N > 128) only on x86
#if __BITINT_MAXWIDTH__ >= 256
  test_signed_bitint<129>();
  test_signed_bitint<256>();
  test_signed_bitint<512>();

  // Large value test (Python-verified)
  {
    _BitInt(256) v = -(_BitInt(256))(1) << 200;
    _BitInt(256) expected = (_BitInt(256))(1) << 200;
    assert(std::abs(v) == expected);
  }
#endif

  return 0;
}
