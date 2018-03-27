/*
 * Central header file for agedu, defining various useful things.
 */

#include "config.h"

#ifdef HAVE_FEATURES_H
#define _GNU_SOURCE
#include <features.h>
#endif

#ifdef HAVE_STDIO_H
#  include <stdio.h>
#endif
#ifdef HAVE_ERRNO_H
#  include <errno.h>
#endif
#ifdef HAVE_TIME_H
#  include <time.h>
#endif
#ifdef HAVE_ASSERT_H
#  include <assert.h>
#endif
#ifdef HAVE_STRING_H
#  include <string.h>
#endif
#ifdef HAVE_STDLIB_H
#  include <stdlib.h>
#endif
#ifdef HAVE_STDARG_H
#  include <stdarg.h>
#endif
#ifdef HAVE_STDINT_H
#  include <stdint.h>
#endif
#ifdef HAVE_STDDEF_H
#  include <stddef.h>
#endif
#ifdef HAVE_LIMITS_H
#  include <limits.h>
#endif
#ifdef HAVE_CTYPE_H
#  include <ctype.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#  include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#  include <sys/stat.h>
#endif
#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#  include <fcntl.h>
#endif
#ifdef HAVE_SYS_MMAN_H
#  include <sys/mman.h>
#endif
#ifdef HAVE_TERMIOS_H
#  include <termios.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
#  include <sys/ioctl.h>
#endif
#ifdef HAVE_FNMATCH_H
#  include <fnmatch.h>
#endif
#ifdef HAVE_PWD_H
#  include <pwd.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#  include <sys/wait.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#  include <sys/socket.h>
#endif
#ifdef HAVE_ARPA_INET_H
#  include <arpa/inet.h>
#endif
#ifdef HAVE_NETINET_IN_H
#  include <netinet/in.h>
#endif
#ifdef HAVE_SYSLOG_H
#  include <syslog.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#  include <sys/select.h>
#endif
#ifdef HAVE_NETDB_H
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

#if defined HAVE_LSTAT64 && HAVE_STAT64
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
