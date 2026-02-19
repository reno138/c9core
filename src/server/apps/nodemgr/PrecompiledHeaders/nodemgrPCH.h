/*
 * This file is part of the C9Core Project. See AUTHORS file for Copyright information
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

#ifndef nodemgrPCH_h__
#define nodemgrPCH_h__

// Common heavy headers that benefit from precompilation.
#include "Config.h"
#include "Define.h"
#include "Log.h"
#include "MessageBuffer.h"
#include "PskCrypt.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/process.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#endif // nodemgrPCH_h__
