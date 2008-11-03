/*
 * Central header file for agedu, defining various useful things.
 */

#include "config.h"

#define PNAME "agedu"

#define DUMPHDR "agedu dump file. pathsep="

#define lenof(x) (sizeof((x))/sizeof(*(x)))

extern char pathsep;

#ifdef HAVE_LSTAT64
#define STRUCT_STAT struct stat64
#define LSTAT lstat64
#else
#define STRUCT_STAT struct stat
#define LSTAT lstat
#endif
