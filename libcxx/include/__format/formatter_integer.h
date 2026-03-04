// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___FORMAT_FORMATTER_INTEGER_H
#define _LIBCPP___FORMAT_FORMATTER_INTEGER_H

#include <__concepts/arithmetic.h>
#include <__config>
#include <__format/concepts.h>
#include <__format/format_parse_context.h>
#include <__format/formatter.h>
#include <__format/formatter_integral.h>
#include <__format/formatter_output.h>
#include <__format/parser_std_format_spec.h>
#include <__type_traits/integer_traits.h>
#include <__type_traits/is_void.h>
#include <__type_traits/make_32_64_or_128_bit.h>

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#  pragma GCC system_header
#endif

_LIBCPP_BEGIN_NAMESPACE_STD

#if _LIBCPP_STD_VER >= 20

template <__fmt_char_type _CharT>
struct __formatter_integer {
public:
  template <class _ParseContext>
  _LIBCPP_HIDE_FROM_ABI constexpr typename _ParseContext::iterator parse(_ParseContext& __ctx) {
    typename _ParseContext::iterator __result = __parser_.__parse(__ctx, __format_spec::__fields_integral);
    __format_spec::__process_parsed_integer(__parser_, "an integer");
    return __result;
  }

  template <integral _Tp, class _FormatContext>
  _LIBCPP_HIDE_FROM_ABI typename _FormatContext::iterator format(_Tp __value, _FormatContext& __ctx) const {
    __format_spec::__parsed_specifications<_CharT> __specs = __parser_.__get_parsed_std_specifications(__ctx);

    if (__specs.__std_.__type_ == __format_spec::__type::__char)
      return __formatter::__format_char(__value, __ctx.out(), __specs);

    using _Type = __make_32_64_or_128_bit_t<_Tp>;
    static_assert(!is_void<_Type>::value, "unsupported integral type used in __formatter_integer::__format");

    // Reduce the number of instantiation of the integer formatter
    return __formatter::__format_integer(static_cast<_Type>(__value), __ctx, __specs);
  }

  __format_spec::__parser<_CharT> __parser_;
};

// Signed integral types.
template <__fmt_char_type _CharT>
struct formatter<signed char, _CharT> : public __formatter_integer<_CharT> {};
template <__fmt_char_type _CharT>
struct formatter<short, _CharT> : public __formatter_integer<_CharT> {};
template <__fmt_char_type _CharT>
struct formatter<int, _CharT> : public __formatter_integer<_CharT> {};
template <__fmt_char_type _CharT>
struct formatter<long, _CharT> : public __formatter_integer<_CharT> {};
template <__fmt_char_type _CharT>
struct formatter<long long, _CharT> : public __formatter_integer<_CharT> {};
#  if _LIBCPP_HAS_INT128
template <__fmt_char_type _CharT>
struct formatter<__int128_t, _CharT> : public __formatter_integer<_CharT> {};
#  endif

// Unsigned integral types.
template <__fmt_char_type _CharT>
struct formatter<unsigned char, _CharT> : public __formatter_integer<_CharT> {};
template <__fmt_char_type _CharT>
struct formatter<unsigned short, _CharT> : public __formatter_integer<_CharT> {};
template <__fmt_char_type _CharT>
struct formatter<unsigned, _CharT> : public __formatter_integer<_CharT> {};
template <__fmt_char_type _CharT>
struct formatter<unsigned long, _CharT> : public __formatter_integer<_CharT> {};
template <__fmt_char_type _CharT>
struct formatter<unsigned long long, _CharT> : public __formatter_integer<_CharT> {};
#  if _LIBCPP_HAS_INT128
template <__fmt_char_type _CharT>
struct formatter<__uint128_t, _CharT> : public __formatter_integer<_CharT> {};
#  endif

// _BitInt(N) types that fit within 128 bits use the standard integer formatter
// via __make_32_64_or_128_bit_t. Wider _BitInt types use the handle path and
// are formatted via to_chars (see format_arg_store.h).
// This concept matches _BitInt types that are NOT already covered by explicit
// specializations above (i.e., not standard or __int128 types).
// Matches _BitInt(N) types that can be formatted via __make_32_64_or_128_bit_t.
// Excludes all types that already have explicit formatter specializations above.
template <class _Tp>
concept __formattable_bitint =
    __signed_or_unsigned_integer<_Tp> && !is_void_v<__make_32_64_or_128_bit_t<_Tp>> && !is_same_v<_Tp, signed char> &&
    !is_same_v<_Tp, unsigned char> && !is_same_v<_Tp, short> && !is_same_v<_Tp, unsigned short> &&
    !is_same_v<_Tp, int> && !is_same_v<_Tp, unsigned int> && !is_same_v<_Tp, long> && !is_same_v<_Tp, unsigned long> &&
    !is_same_v<_Tp, long long> && !is_same_v<_Tp, unsigned long long>
#  if _LIBCPP_HAS_INT128
    && !is_same_v<_Tp, __int128_t> && !is_same_v<_Tp, __uint128_t>
#  endif
    ;

template <__formattable_bitint _Tp, __fmt_char_type _CharT>
struct formatter<_Tp, _CharT> : public __formatter_integer<_CharT> {};

#  if _LIBCPP_STD_VER >= 23
template <>
inline constexpr bool enable_nonlocking_formatter_optimization<signed char> = true;
template <>
inline constexpr bool enable_nonlocking_formatter_optimization<short> = true;
template <>
inline constexpr bool enable_nonlocking_formatter_optimization<int> = true;
template <>
inline constexpr bool enable_nonlocking_formatter_optimization<long> = true;
template <>
inline constexpr bool enable_nonlocking_formatter_optimization<long long> = true;
#    if _LIBCPP_HAS_INT128
template <>
inline constexpr bool enable_nonlocking_formatter_optimization<__int128_t> = true;
#    endif

template <>
inline constexpr bool enable_nonlocking_formatter_optimization<unsigned char> = true;
template <>
inline constexpr bool enable_nonlocking_formatter_optimization<unsigned short> = true;
template <>
inline constexpr bool enable_nonlocking_formatter_optimization<unsigned> = true;
template <>
inline constexpr bool enable_nonlocking_formatter_optimization<unsigned long> = true;
template <>
inline constexpr bool enable_nonlocking_formatter_optimization<unsigned long long> = true;
#    if _LIBCPP_HAS_INT128
template <>
inline constexpr bool enable_nonlocking_formatter_optimization<__uint128_t> = true;
#    endif
#  endif // _LIBCPP_STD_VER >= 23
#endif   // _LIBCPP_STD_VER >= 20

_LIBCPP_END_NAMESPACE_STD

#endif // _LIBCPP___FORMAT_FORMATTER_INTEGER_H
