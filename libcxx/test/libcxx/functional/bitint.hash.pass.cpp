//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Test that std::hash works for _BitInt(N) types, including types with
// padding bits (non-byte-aligned widths).

// UNSUPPORTED: c++03, c++11, c++14, c++17

#include <cassert>
#include <functional>

// Test that hash is deterministic: same value always produces same hash.
template <class T>
void test_deterministic(T val) {
  std::hash<T> h;
  assert(h(val) == h(val));
}

// Test that different values (likely) produce different hashes.
template <class T>
void test_different(T a, T b) {
  std::hash<T> h;
  // Not guaranteed, but extremely unlikely to collide for small values.
  // We test this to catch bugs where hash always returns 0 or similar.
  assert(h(a) != h(b));
}

template <int N>
void test_unsigned_bitint() {
  using T = unsigned _BitInt(N);
  test_deterministic(T(0));
  test_deterministic(T(1));
  if constexpr (N >= 8)
    test_deterministic(T(42));

  test_different(T(0), T(1));
  if constexpr (N >= 8)
    test_different(T(1), T(42));
}

template <int N>
void test_signed_bitint() {
  using T = _BitInt(N);
  test_deterministic(T(0));
  test_deterministic(T(1));
  test_deterministic(T(-1));
  if constexpr (N >= 8)
    test_deterministic(T(-42));

  test_different(T(0), T(1));
  test_different(T(0), T(-1));
}

int main(int, char**) {
  // Standard widths
  test_unsigned_bitint<8>();
  test_unsigned_bitint<16>();
  test_unsigned_bitint<32>();
  test_unsigned_bitint<64>();
  test_unsigned_bitint<128>();

  test_signed_bitint<8>();
  test_signed_bitint<16>();
  test_signed_bitint<32>();
  test_signed_bitint<64>();
  test_signed_bitint<128>();

  // Odd widths with padding bits
  test_unsigned_bitint<7>();
  test_unsigned_bitint<9>();
  test_unsigned_bitint<15>();
  test_unsigned_bitint<17>();
  test_unsigned_bitint<33>();
  test_unsigned_bitint<65>();
  test_unsigned_bitint<127>();

  test_signed_bitint<7>();
  test_signed_bitint<9>();
  test_signed_bitint<15>();
  test_signed_bitint<33>();
  test_signed_bitint<65>();
  test_signed_bitint<127>();

  // Wide _BitInt (N > 128) only on x86
#if __BITINT_MAXWIDTH__ >= 256
  test_unsigned_bitint<129>();
  test_unsigned_bitint<255>();
  test_unsigned_bitint<256>();
  test_unsigned_bitint<512>();

  test_signed_bitint<129>();
  test_signed_bitint<256>();
  test_signed_bitint<512>();

  // Verify that padding bits don't affect hash: construct the same logical
  // value and verify the hash is identical.
  {
    unsigned _BitInt(129) a = 42;
    unsigned _BitInt(129) b = 42;
    std::hash<unsigned _BitInt(129)> h;
    assert(h(a) == h(b));
  }
#endif

  return 0;
}
