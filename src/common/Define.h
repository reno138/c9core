/*
 * This file is part of the c9core Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef C9CORE_DEFINE_H
#define C9CORE_DEFINE_H

#include "CompilerDefs.h"
#include <cinttypes>
#include <climits>

#define C9CORE_LITTLEENDIAN 0
#define C9CORE_BIGENDIAN    1

#if !defined(C9CORE_ENDIAN)
#  if defined (BOOST_BIG_ENDIAN)
#    define C9CORE_ENDIAN C9CORE_BIGENDIAN
#  else
#    define C9CORE_ENDIAN C9CORE_LITTLEENDIAN
#  endif
#endif

#if C9_PLATFORM == C9_PLATFORM_WINDOWS
#  define C9CORE_PATH_MAX MAX_PATH
#  define _USE_MATH_DEFINES
#else //C9_PLATFORM != C9_PLATFORM_WINDOWS
#  define C9CORE_PATH_MAX PATH_MAX
#endif //C9_PLATFORM

#if !defined(COREDEBUG)
#  define C9CORE_INLINE inline
#else //COREDEBUG
#  if !defined(C9CORE_DEBUG)
#    define C9CORE_DEBUG
#  endif //C9CORE_DEBUG
#  define C9CORE_INLINE
#endif //!COREDEBUG

#if C9_COMPILER == C9_COMPILER_GNU
#  define ATTR_PRINTF(F, V) __attribute__ ((format (printf, F, V)))
#else //C9_COMPILER != C9_COMPILER_GNU
#  define ATTR_PRINTF(F, V)
#endif //C9_COMPILER == C9_COMPILER_GNU

#ifdef C9CORE_API_USE_DYNAMIC_LINKING
#  if C9_COMPILER == C9_COMPILER_MICROSOFT
#    define C9_API_EXPORT __declspec(dllexport)
#    define C9_API_IMPORT __declspec(dllimport)
#  elif C9_COMPILER == C9_COMPILER_GNU
#    define C9_API_EXPORT __attribute__((visibility("default")))
#    define C9_API_IMPORT
#  else
#    error compiler not supported!
#  endif
#else
#  define C9_API_EXPORT
#  define C9_API_IMPORT
#endif

#ifdef C9CORE_API_EXPORT_COMMON
#  define C9_COMMON_API C9_API_EXPORT
#else
#  define C9_COMMON_API C9_API_IMPORT
#endif

#ifdef C9CORE_API_EXPORT_DATABASE
#  define C9_DATABASE_API C9_API_EXPORT
#else
#  define C9_DATABASE_API C9_API_IMPORT
#endif

#ifdef C9CORE_API_EXPORT_SHARED
#  define C9_SHARED_API C9_API_EXPORT
#else
#  define C9_SHARED_API C9_API_IMPORT
#endif

#ifdef C9CORE_API_EXPORT_GAME
#  define C9_GAME_API C9_API_EXPORT
#else
#  define C9_GAME_API C9_API_IMPORT
#endif

#define UI64LIT(N) UINT64_C(N)
#define SI64LIT(N) INT64_C(N)

#define STRING_VIEW_FMT_ARG(str) static_cast<int>((str).length()), (str).data()

typedef std::int64_t int64;
typedef std::int32_t int32;
typedef std::int16_t int16;
typedef std::int8_t int8;
typedef std::uint64_t uint64;
typedef std::uint32_t uint32;
typedef std::uint16_t uint16;
typedef std::uint8_t uint8;

#endif //C9CORE_DEFINE_H
