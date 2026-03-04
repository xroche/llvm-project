//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Test std::format support for _BitInt(N) types.

// UNSUPPORTED: c++03, c++11, c++14, c++17

#include <cassert>
#include <format>
#include <string>

// ===== Basic decimal formatting =====

void test_decimal() {
  // Small widths
  assert(std::format("{}", (_BitInt(8))0) == "0");
  assert(std::format("{}", (_BitInt(8))42) == "42");
  assert(std::format("{}", (_BitInt(8))-42) == "-42");
  assert(std::format("{}", (_BitInt(8))127) == "127");
  assert(std::format("{}", (_BitInt(8))-128) == "-128");

  // Standard widths
  assert(std::format("{}", (_BitInt(16))32767) == "32767");
  assert(std::format("{}", (_BitInt(32))2147483647) == "2147483647");
  assert(std::format("{}", (_BitInt(64))-999999999999LL) == "-999999999999");

  // 128-bit requires __int128 for __make_32_64_or_128_bit_t (Python-verified)
#if _LIBCPP_HAS_INT128
  assert(std::format("{}", (_BitInt(128))1234567890123456LL) == "1234567890123456");
#endif

  // Unsigned variants
  assert(std::format("{}", (unsigned _BitInt(8))255) == "255");
  assert(std::format("{}", (unsigned _BitInt(64))0xDEADBEEFCAFEBABEULL) == "16045690984503098046");

  // Odd widths
  assert(std::format("{}", (_BitInt(7))63) == "63");
  assert(std::format("{}", (_BitInt(7))-64) == "-64");
  assert(std::format("{}", (unsigned _BitInt(1))1) == "1");
  assert(std::format("{}", (unsigned _BitInt(1))0) == "0");
}

// ===== Hex formatting =====

void test_hex() {
  assert(std::format("{:x}", (unsigned _BitInt(32))0xDEAD) == "dead");
  assert(std::format("{:X}", (unsigned _BitInt(32))0xDEAD) == "DEAD");
  assert(std::format("{:#x}", (unsigned _BitInt(32))0xDEAD) == "0xdead");
  assert(std::format("{:#X}", (unsigned _BitInt(32))0xDEAD) == "0XDEAD");

  // 64-bit hex (Python-verified)
  assert(std::format("{:#x}", (unsigned _BitInt(64))0xDEADBEEFCAFEBABEULL) == "0xdeadbeefcafebabe");

  // 128-bit hex
#if _LIBCPP_HAS_INT128
  assert(std::format("{:#x}", (unsigned _BitInt(128))0xCAFEBABE) == "0xcafebabe");
#endif
}

// ===== Octal formatting =====

void test_octal() {
  assert(std::format("{:o}", (unsigned _BitInt(32))0777) == "777");
  assert(std::format("{:#o}", (unsigned _BitInt(32))0777) == "0777");
  assert(std::format("{:o}", (unsigned _BitInt(32))0) == "0");
}

// ===== Binary formatting =====

void test_binary() {
  assert(std::format("{:b}", (unsigned _BitInt(8))0xFF) == "11111111");
  assert(std::format("{:#b}", (unsigned _BitInt(8))0xFF) == "0b11111111");
  assert(std::format("{:#B}", (unsigned _BitInt(8))0xFF) == "0B11111111");
  assert(std::format("{:b}", (unsigned _BitInt(8))0) == "0");
}

// ===== Width and fill =====

void test_width_fill() {
  // Right-aligned (default for integers)
  assert(std::format("{:>20}", (_BitInt(32))42) == "                  42");
  assert(std::format("{:>20}", (_BitInt(32))-42) == "                 -42");

  // Left-aligned
  assert(std::format("{:<20}", (_BitInt(32))42) == "42                  ");

  // Center-aligned
  assert(std::format("{:^20}", (_BitInt(32))42) == "         42         ");

  // Zero-padded
  assert(std::format("{:020}", (_BitInt(64))42) == "00000000000000000042");
  assert(std::format("{:020}", (_BitInt(64))-42) == "-0000000000000000042");

  // Custom fill character
  assert(std::format("{:*>10}", (_BitInt(32))42) == "********42");
}

// ===== Sign formatting =====

void test_sign() {
  // Plus sign
  assert(std::format("{:+}", (_BitInt(32))42) == "+42");
  assert(std::format("{:+}", (_BitInt(32))-42) == "-42");

  // Space sign
  assert(std::format("{: }", (_BitInt(32))42) == " 42");
  assert(std::format("{: }", (_BitInt(32))-42) == "-42");
}

// ===== Explicit decimal specifier =====

void test_explicit_decimal() {
  assert(std::format("{:d}", (_BitInt(32))42) == "42");
#if _LIBCPP_HAS_INT128
  assert(std::format("{:d}", (_BitInt(128))42) == "42");
#endif
}

// ===== Edge values =====

void test_edge_values() {
  // Zero
  assert(std::format("{}", (_BitInt(8))0) == "0");
#if _LIBCPP_HAS_INT128
  assert(std::format("{}", (_BitInt(128))0) == "0");
#endif
  assert(std::format("{:#x}", (unsigned _BitInt(64))0) == "0x0");

  // _BitInt(2): minimum signed width, range [-2, 1]
  assert(std::format("{}", (_BitInt(2))1) == "1");
  assert(std::format("{}", (_BitInt(2))-1) == "-1");
  assert(std::format("{}", (_BitInt(2))-2) == "-2");
}

int main(int, char**) {
  test_decimal();
  test_hex();
  test_octal();
  test_binary();
  test_width_fill();
  test_sign();
  test_explicit_decimal();
  test_edge_values();
  return 0;
}
