#
# This file is part of the c9core Project. See AUTHORS file for Copyright information
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
#

# Build the vendored nats-server (Go source, v2.10.24) using the host Go
# toolchain. Produces a fully static linux-amd64 binary (CGO_ENABLED=0)
# matching the official upstream release artifact, and installs it to
# ${CMAKE_INSTALL_PREFIX}/bin alongside worldserver/authserver.
#
# Go modules are vendored under deps/nats-server/vendor so the build does
# not require network access. Dependencies are pinned via go.sum.

find_program(GO_EXECUTABLE go)
if (NOT GO_EXECUTABLE)
  message(FATAL_ERROR
    "Go toolchain not found. nats-server is built from vendored Go source. "
    "Install Go (e.g. 'pacman -S go', 'apt install golang-go') or pass "
    "-DGO_EXECUTABLE=/path/to/go to cmake.")
endif()

execute_process(
  COMMAND ${GO_EXECUTABLE} version
  OUTPUT_VARIABLE GO_VERSION_OUTPUT
  OUTPUT_STRIP_TRAILING_WHITESPACE)
message(STATUS "nats-server: building with ${GO_VERSION_OUTPUT}")

set(NATS_SERVER_BINARY "${CMAKE_CURRENT_BINARY_DIR}/nats-server")

# Collect all .go files under the vendored tree as build dependencies so
# the custom command re-runs only when source actually changes.
file(GLOB_RECURSE NATS_SERVER_GO_SOURCES
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.go")

add_custom_command(
  OUTPUT  ${NATS_SERVER_BINARY}
  COMMAND ${CMAKE_COMMAND} -E env
            CGO_ENABLED=0
            GOFLAGS=-mod=vendor
          ${GO_EXECUTABLE} build
            -trimpath
            -ldflags "-s -w"
            -o ${NATS_SERVER_BINARY}
            .
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS ${NATS_SERVER_GO_SOURCES}
          ${CMAKE_CURRENT_SOURCE_DIR}/go.mod
          ${CMAKE_CURRENT_SOURCE_DIR}/go.sum
  COMMENT "Building nats-server (vendored Go source)"
  VERBATIM)

add_custom_target(nats-server ALL DEPENDS ${NATS_SERVER_BINARY})

install(PROGRAMS ${NATS_SERVER_BINARY}
        DESTINATION bin)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/nats-server.conf.dist
        DESTINATION etc)
