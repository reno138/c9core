/*
 * This file is part of the C9Core Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License version 2 as published by the Free Software
 * Foundation. See <http://www.gnu.org/licenses/>.
 */

/**
 * @file BoostProcess.h
 * @brief Portable Boost.Process include.
 *
 * Boost 1.83+ (Arch Linux packaging) ships boost/process/v1.hpp for the legacy
 * API alongside the new v2 default. Ubuntu 24.04's Boost 1.83 package does not
 * split into v1/v2 — it exposes the legacy API directly via boost/process.hpp.
 * This header detects which variant is present and defines `namespace bp`
 * accordingly so callers use `bp::child`, `bp::std_out`, etc. portably.
 */

#pragma once

#if __has_include(<boost/process/v1.hpp>)
#  include <boost/process/v1.hpp>
namespace bp = boost::process::v1;
#else
#  include <boost/process.hpp>
namespace bp = boost::process;
#endif
