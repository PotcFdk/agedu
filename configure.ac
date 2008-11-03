# autoconf input for agedu.

AC_INIT([agedu], [6.66], [anakin@pobox.com])
AC_CONFIG_SRCDIR([agedu.c])
AC_CONFIG_HEADER([config.h])

AM_INIT_AUTOMAKE

# Checks for programs.
AC_PROG_CC
AC_PROG_CC_C99
AC_PROG_INSTALL

# Checks for libraries.

# Checks for header files.
AC_HEADER_DIRENT
AC_HEADER_STDC
AC_HEADER_SYS_WAIT
AC_CHECK_HEADERS([arpa/inet.h fcntl.h limits.h netinet/in.h stddef.h stdint.h stdlib.h string.h sys/ioctl.h sys/socket.h syslog.h termios.h unistd.h])

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
AC_CHECK_FUNCS([ftruncate fdopendir inet_ntoa lstat64 memchr munmap select socket strcasecmp strchr strcspn strerror strrchr strspn strtoul strtoull])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
