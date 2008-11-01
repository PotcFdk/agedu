/*
 * du.h: the function which actually performs the disk scan.
 */

#include <sys/types.h>
#include <sys/stat.h>

/*
 * Function called to report a file or directory, its size and its
 * last-access time.
 *
 * Returns non-zero if recursion should proceed into this file's
 * contents (if it's a directory); zero if it should not. If the
 * file isn't a directory, the return value is ignored.
 */
typedef int (*gotdata_fn_t)(void *ctx,
			    const char *pathname,
			    const struct stat64 *st);

/*
 * Recursively scan a directory tree and report every
 * space-consuming item in it to gotdata().
 */
void du(const char *path, gotdata_fn_t gotdata, void *gotdata_ctx);
