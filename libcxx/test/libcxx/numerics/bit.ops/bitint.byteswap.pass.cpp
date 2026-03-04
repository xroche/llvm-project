//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Test std::byteswap for _BitInt(N) types, including wide types (N > 128).
// Wide _BitInt (N > 128) is only supported on x86 targets.

// UNSUPPORTED: c++03, c++11, c++14, c++17, c++20
// REQUIRES: target={{x86.*}}

#include <bit>
#include <cassert>

void test_standard_sizes() {
  assert(std::byteswap((unsigned _BitInt(8))0xAB) == (unsigned _BitInt(8))0xAB);
  assert(std::byteswap((unsigned _BitInt(16))0x0102) == (unsigned _BitInt(16))0x0201);
  assert(std::byteswap((unsigned _BitInt(32))0x01020304) == (unsigned _BitInt(32))0x04030201);
}

// Roundtrip: only valid for power-of-2 sizes where sizeof * 8 == N
void test_roundtrip() {
  unsigned _BitInt(64) v64 = 0xDEADBEEFCAFEBABEULL;
  assert(std::byteswap(std::byteswap(v64)) == v64);

  unsigned _BitInt(128) v128 = ((unsigned _BitInt(128))0xDEADBEEFULL << 64) | 0xCAFEBABEULL;
  assert(std::byteswap(std::byteswap(v128)) == v128);

  // 256-bit: uses generic loop (sizeof=32)
  unsigned _BitInt(256) v256 = ((unsigned _BitInt(256))0xDEADBEEFCAFEBABEULL << 128) | 0x1234567890ABCDEFULL;
  assert(std::byteswap(std::byteswap(v256)) == v256);
}

void test_wide_byteswap() {
  // Byte at position 0 moves to position 31 in a 256-bit value
  unsigned _BitInt(256) v = (unsigned _BitInt(256))0xAB;
  auto swapped = std::byteswap(v);
  assert((unsigned char)(swapped >> (31 * 8)) == 0xAB);

  // 4096-bit: first byte to last byte
  unsigned _BitInt(4096) big = (unsigned _BitInt(4096))0x42;
  auto big_swapped = std::byteswap(big);
  assert((unsigned char)(big_swapped >> ((sizeof(big) - 1) * 8)) == 0x42);
}

// Non-power-of-2: compiles and runs but roundtrip is NOT guaranteed
// because padding bits (sizeof*8 > N) cause truncation.
void test_non_power_of_2() {
  unsigned _BitInt(129) v = 42;
  auto swapped = std::byteswap(v);
  (void)swapped; // just verify no crash
}

int main(int, char**) {
  test_standard_sizes();
  test_roundtrip();
  test_wide_byteswap();
  test_non_power_of_2();
  return 0;
}
