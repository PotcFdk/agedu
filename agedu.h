/*
 * Central header file for agedu, defining various useful things.
 */

#include "cmake.h"

#if HAVE_FEATURES_H
#define _GNU_SOURCE
#include <features.h>
#endif

#if HAVE_STDIO_H
#  include <stdio.h>
#endif
#if HAVE_STDBOOL_H
#  include <stdbool.h>
#endif
#if HAVE_ERRNO_H
#  include <errno.h>
#endif
#if HAVE_TIME_H
#  include <time.h>
#endif
#if HAVE_ASSERT_H
#  include <assert.h>
#endif
#if HAVE_STRING_H
#  include <string.h>
#endif
#if HAVE_STDLIB_H
#  include <stdlib.h>
#endif
#if HAVE_STDARG_H
#  include <stdarg.h>
#endif
#if HAVE_STDINT_H
#  include <stdint.h>
#endif
#if HAVE_STDDEF_H
#  include <stddef.h>
#endif
#if HAVE_LIMITS_H
#  include <limits.h>
#endif
#if HAVE_CTYPE_H
#  include <ctype.h>
#endif

#if HAVE_SYS_TYPES_H
#  include <sys/types.h>
#endif
#if HAVE_SYS_STAT_H
#  include <sys/stat.h>
#endif
#if HAVE_UNISTD_H
#  include <unistd.h>
#endif
#if HAVE_FCNTL_H
#  include <fcntl.h>
#endif
#if HAVE_SYS_MMAN_H
#  include <sys/mman.h>
#endif
#if HAVE_TERMIOS_H
#  include <termios.h>
#endif
#if HAVE_SYS_IOCTL_H
#  include <sys/ioctl.h>
#endif
#if HAVE_FNMATCH_H
#  include <fnmatch.h>
#endif
#if HAVE_PWD_H
#  include <pwd.h>
#endif
#if HAVE_SYS_WAIT_H
#  include <sys/wait.h>
#endif
#if HAVE_SYS_SOCKET_H
#  include <sys/socket.h>
#endif
#if HAVE_ARPA_INET_H
#  include <arpa/inet.h>
#endif
#if HAVE_NETINET_IN_H
#  include <netinet/in.h>
#endif
#if HAVE_SYSLOG_H
#  include <syslog.h>
#endif
#if HAVE_SYS_SELECT_H
#  include <sys/select.h>
#endif
#if HAVE_NETDB_H
#  include <netdb.h>
#endif
    
#ifndef HOST_NAME_MAX
/* Reportedly at least one Solaris fails to comply with its POSIX
 * requirement to define this (see POSIX spec for gethostname) */
#define HOST_NAME_MAX 255 /* upper bound specified in SUS */
#endif

#define PNAME "agedu"

#define lenof(x) (sizeof((x))/sizeof(*(x)))

extern char pathsep;

#if HAVE_LSTAT64 && HAVE_STAT64
#define STRUCT_STAT struct stat64
#define LSTAT_FUNC lstat64
#define STAT_FUNC stat64
#else
#define STRUCT_STAT struct stat
#define LSTAT_FUNC lstat
#define STAT_FUNC stat
#endif

#define max(x,y) ( (x) > (y) ? (x) : (y) )
#define min(x,y) ( (x) < (y) ? (x) : (y) )
