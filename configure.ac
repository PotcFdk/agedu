# autoconf input for agedu.

AC_INIT([agedu], [6.66], [anakin@pobox.com])
AC_CONFIG_SRCDIR([agedu.c])
AC_CONFIG_HEADER([config.h])

AM_INIT_AUTOMAKE

# Checks for programs.
AC_PROG_CC
AC_PROG_CC_C99
AC_PROG_INSTALL
AC_CHECK_PROG([HALIBUT],[halibut],[yes],[no])
AM_CONDITIONAL([HAVE_HALIBUT],[test "x$HALIBUT" = "xyes"])

# Checks for libraries.

# Checks for header files.
AC_HEADER_DIRENT
AC_HEADER_STDC
AC_HEADER_SYS_WAIT
AC_CHECK_HEADERS([assert.h arpa/inet.h ctype.h errno.h fcntl.h features.h fnmatch.h limits.h netinet/in.h pwd.h stdarg.h stddef.h stdint.h stdio.h stdlib.h string.h sys/ioctl.h sys/mman.h sys/socket.h syslog.h termios.h time.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_OFF_T
AC_TYPE_SIZE_T
AC_STRUCT_TM

# Checks for library functions.
AC_FUNC_CLOSEDIR_VOID
AC_PROG_GCC_TRADITIONAL
AC_FUNC_MMAP
AC_FUNC_SELECT_ARGTYPES
AC_FUNC_STRFTIME
AC_FUNC_VPRINTF

AC_SEARCH_LIBS(connect, socket nsl)
AC_SEARCH_LIBS(inet_ntoa, socket nsl)
AC_SEARCH_LIBS(inet_addr, socket nsl)
# AC_SEARCH_LIBS(gethostbyname, socket nsl resolv) # (if we used gethostbyname)

AC_CHECK_FUNCS([ftruncate fdopendir lstat64 stat64 memchr munmap select socket strcasecmp strchr strcspn strerror strrchr strspn strtoul strtoull connect inet_ntoa inet_addr])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
