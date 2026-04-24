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

if(CMAKE_SYSTEM_NAME MATCHES "Windows")
  if(USE_MYSQL_SOURCES)
      add_subdirectory(mysqllite)
  endif()
endif()

add_subdirectory(SFMT)
add_subdirectory(argon2)
add_subdirectory(boost)
add_subdirectory(fmt)
add_subdirectory(jemalloc)
add_subdirectory(openssl)
add_subdirectory(stdfs)
add_subdirectory(threads)
add_subdirectory(utf8cpp)

if ((APPS_BUILD AND (NOT APPS_BUILD STREQUAL "none")) OR BUILD_TOOLS_DB_IMPORT)
  add_subdirectory(mysql)
endif()

if (BUILD_APPLICATION_WORLDSERVER OR BUILD_TOOLS_MAPS)
  add_subdirectory(zlib)
  add_subdirectory(g3dlite)
  add_subdirectory(recastnavigation)
endif()

if (BUILD_APPLICATION_WORLDSERVER)
  add_subdirectory(readline)
  add_subdirectory(gsoap)
  add_subdirectory(gperftools)
endif()

if (BUILD_TOOLS_MAPS)
  add_subdirectory(bzip2)
  add_subdirectory(libmpq)
  add_subdirectory(fkYAML)
endif()

# ── NATS C client (cluster control-channel transport) ────────────────────────
# Vendored at deps/cnats (nats.c v3.8.0). Used by worldserver (NatsBus) and
# clustermgr (NatsMonitor). EXCLUDE_FROM_ALL suppresses upstream install rules
# so libnats_static.a and nats headers don't pollute ${CMAKE_INSTALL_PREFIX};
# the game library still pulls nats_static in via target_link_libraries.
if (BUILD_APPLICATION_WORLDSERVER)
  set(NATS_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
  set(NATS_BUILD_LIB_SHARED OFF CACHE BOOL "" FORCE)
  set(NATS_BUILD_LIB_STATIC ON  CACHE BOOL "" FORCE)
  set(NATS_BUILD_STREAMING  OFF CACHE BOOL "" FORCE)
  set(NATS_BUILD_WITH_TLS   OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING         OFF CACHE BOOL "" FORCE)
  add_subdirectory(cnats EXCLUDE_FROM_ALL)

  # Vendored nats.c forces -std=c99 in NATS_COMMON_C_FLAGS but uses C11
  # _Generic throughout the source. Override here so the compiler accepts
  # it without 60+ -Wc11-extensions warnings. Also silence a handful of
  # const-qualifier warnings in upstream code we don't maintain.
  if (TARGET nats_static)
    target_compile_options(nats_static PRIVATE
      -std=c11
      -Wno-incompatible-pointer-types-discards-qualifiers)
  endif()

  # NATS broker (Go source vendored at deps/nats-server, v2.10.24).
  # Built via the host Go toolchain; produces a static binary installed to
  # ${CMAKE_INSTALL_PREFIX}/bin/nats-server with a starter config in etc/.
  add_subdirectory(nats-server)
endif()
