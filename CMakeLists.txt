cmake_minimum_required(VERSION 3.7)
project(agedu LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME MATCHES "Windows")
  # On Unix, build the main agedu program.

  set(AGEDU_IPV6 ON
    CACHE BOOL "Build agedu with IPv6 support if possible")
  set(AGEDU_IPV4 ON
    CACHE BOOL "Build agedu with IPv4 support if possible")

  add_executable(agedu
    agedu.c du.c alloc.c trie.c index.c html.c httpd.c
    fgetline.c licence.c dumpfile.c)

  include(CheckIncludeFile)
  check_include_file(arpa/inet.h HAVE_ARPA_INET_H)
  check_include_file(assert.h HAVE_ASSERT_H)
  check_include_file(ctype.h HAVE_CTYPE_H)
  check_include_file(dirent.h HAVE_DIRENT_H)
  check_include_file(errno.h HAVE_ERRNO_H)
  check_include_file(fcntl.h HAVE_FCNTL_H)
  check_include_file(features.h HAVE_FEATURES_H)
  check_include_file(fnmatch.h HAVE_FNMATCH_H)
  check_include_file(limits.h HAVE_LIMITS_H)
  check_include_file(ndir.h HAVE_NDIR_H)
  check_include_file(netdb.h HAVE_NETDB_H)
  check_include_file(netinet/in.h HAVE_NETINET_IN_H)
  check_include_file(pwd.h HAVE_PWD_H)
  check_include_file(stdarg.h HAVE_STDARG_H)
  check_include_file(stdbool.h HAVE_STDBOOL_H)
  check_include_file(stddef.h HAVE_STDDEF_H)
  check_include_file(stdint.h HAVE_STDINT_H)
  check_include_file(stdio.h HAVE_STDIO_H)
  check_include_file(stdlib.h HAVE_STDLIB_H)
  check_include_file(string.h HAVE_STRING_H)
  check_include_file(sys/dir.h HAVE_SYS_DIR_H)
  check_include_file(sys/ioctl.h HAVE_SYS_IOCTL_H)
  check_include_file(sys/mman.h HAVE_SYS_MMAN_H)
  check_include_file(sys/ndir.h HAVE_SYS_NDIR_H)
  check_include_file(sys/select.h HAVE_SYS_SELECT_H)
  check_include_file(sys/socket.h HAVE_SYS_SOCKET_H)
  check_include_file(sys/stat.h HAVE_SYS_STAT_H)
  check_include_file(sys/types.h HAVE_SYS_TYPES_H)
  check_include_file(sys/wait.h HAVE_SYS_WAIT_H)
  check_include_file(syslog.h HAVE_SYSLOG_H)
  check_include_file(termios.h HAVE_TERMIOS_H)
  check_include_file(time.h HAVE_TIME_H)
  check_include_file(unistd.h HAVE_UNISTD_H)

  include(CheckSymbolExists)
  check_symbol_exists(fdopendir "sys/types.h;dirent.h" HAVE_FDOPENDIR)
  check_symbol_exists(getaddrinfo "sys/types.h;sys/socket.h;netdb.h"
    HAVE_GETADDRINFO)
  check_symbol_exists(gethostbyname "netdb.h" HAVE_GETHOSTBYNAME)
  check_symbol_exists(lstat64 "sys/types.h;sys/stat.h;unistd.h" HAVE_LSTAT64)
  check_symbol_exists(stat64 "sys/types.h;sys/stat.h;unistd.h" HAVE_STAT64)
  check_symbol_exists(strsignal "string.h" HAVE_STRSIGNAL)

  set(GENERATED_SOURCES_DIR ${CMAKE_CURRENT_BINARY_DIR}${CMAKE_FILES_DIRECTORY})
  include_directories(${GENERATED_SOURCES_DIR})
  configure_file(cmake.h.in ${GENERATED_SOURCES_DIR}/cmake.h)

  # If Halibut is available, build the docs too.
  find_program(HALIBUT halibut)
  if(HALIBUT)
    set(BUILD_MANPAGE ON)
    add_custom_command(OUTPUT agedu.1
      COMMAND ${HALIBUT} --man=agedu.1
        ${CMAKE_CURRENT_SOURCE_DIR}/agedu.but
      DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/agedu.but)
    add_custom_target(doc ALL DEPENDS agedu.1)
  else()
    set(BUILD_MANPAGE OFF)
  endif()

  # Installation
  include(GNUInstallDirs)
  if(CMAKE_VERSION VERSION_LESS 3.14)
    # CMake 3.13 and earlier required an explicit install destination.
    install(TARGETS agedu RUNTIME DESTINATION bin)
  else()
    # 3.14 and above selects a sensible default, which we should avoid
    # overriding here so that end users can override it using
    # CMAKE_INSTALL_BINDIR.
    install(TARGETS agedu)
  endif()

  if(BUILD_MANPAGE)
    install(FILES ${CMAKE_BINARY_DIR}/agedu.1
      DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
  elseif(EXISTS ${CMAKE_SOURCE_DIR}/agedu.1)
    # If we weren't able to build the man page from source, but we are
    # building from a source tarball in which a pre-built man page is
    # provided, we can install that.
    install(FILES ${CMAKE_SOURCE_DIR}/agedu.1
      DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
  endif()
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Windows")
  # On Windows, build ageduscan.exe.

  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
  add_executable(ageduscan winscan.c)
endif()

