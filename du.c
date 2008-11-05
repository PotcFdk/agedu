/*
 * du.c: implementation of du.h.
 */

#include "agedu.h" /* for config.h */

#ifdef HAVE_FEATURES_H
#define _GNU_SOURCE
#include <features.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "du.h"
#include "alloc.h"

#if !defined __linux__ || defined HAVE_FDOPENDIR

/*
 * Wrappers around POSIX opendir, readdir and closedir, which
 * permit me to replace them with different wrappers in special
 * circumstances.
 */

#include <dirent.h>
typedef DIR *dirhandle;

int open_dir(const char *path, dirhandle *dh)
{
#if defined O_NOATIME && defined HAVE_FDOPENDIR

    /*
     * On Linux, we have the O_NOATIME flag. This means we can
     * read the contents of directories without affecting their
     * atimes, which enables us to at least try to include them in
     * the age display rather than exempting them.
     *
     * Unfortunately, opendir() doesn't let us open a directory
     * with O_NOATIME. So instead, we have to open the directory
     * with vanilla open(), and then use fdopendir() to translate
     * the fd into a POSIX dir handle.
     */
    int fd;
    
    fd = open(path, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_LARGEFILE |
	      O_NOATIME | O_DIRECTORY);
    if (fd < 0) {
	/*
	 * Opening a file with O_NOATIME is not unconditionally
	 * permitted by the Linux kernel. As far as I can tell,
	 * it's permitted only for files on which the user would
	 * have been able to call utime(2): in other words, files
	 * for which the user could have deliberately set the
	 * atime back to its original value after finishing with
	 * it. Hence, O_NOATIME has no security implications; it's
	 * simply a cleaner, faster and more race-condition-free
	 * alternative to stat(), a normal open(), and a utimes()
	 * when finished.
	 *
	 * The upshot of all of which, for these purposes, is that
	 * we must be prepared to try again without O_NOATIME if
	 * we receive EPERM.
	 */
	if (errno == EPERM)
	    fd = open(path, O_RDONLY | O_NONBLOCK | O_NOCTTY |
		      O_LARGEFILE | O_DIRECTORY);
	if (fd < 0)
	    return -1;
    }

    *dh = fdopendir(fd);
#else
    *dh = opendir(path);
#endif

    if (!*dh)
	return -1;
    return 0;
}

const char *read_dir(dirhandle *dh)
{
    struct dirent *de = readdir(*dh);
    return de ? de->d_name : NULL;
}

void close_dir(dirhandle *dh)
{
    closedir(*dh);
}

#else /* defined __linux__ && !defined HAVE_FDOPENDIR */

/*
 * Earlier versions of glibc do not have fdopendir(). Therefore,
 * if we are on Linux and still wish to make use of O_NOATIME, we
 * have no option but to talk directly to the kernel system call
 * interface which underlies the POSIX opendir/readdir machinery.
 */

#define __KERNEL__
#include <unistd.h>
#include <fcntl.h>
#include <linux/types.h>
#include <linux/dirent.h>
#include <linux/unistd.h>

_syscall3(int, getdents, uint, fd, struct dirent *, dirp, uint, count)

typedef struct {
    int fd;
    struct dirent data[32];
    struct dirent *curr;
    int pos, endpos;
} dirhandle;

int open_dir(const char *path, dirhandle *dh)
{
    /*
     * As above, we try with O_NOATIME and then fall back to
     * trying without it.
     */
    dh->fd = open(path, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_LARGEFILE |
		  O_NOATIME | O_DIRECTORY);
    if (dh->fd < 0) {
	if (errno == EPERM)
	    dh->fd = open(path, O_RDONLY | O_NONBLOCK | O_NOCTTY |
			  O_LARGEFILE | O_DIRECTORY);
	if (dh->fd < 0)
	    return -1;
    }

    dh->pos = dh->endpos = 0;

    return 0;
}

const char *read_dir(dirhandle *dh)
{
    const char *ret;

    if (dh->pos >= dh->endpos) {
	dh->curr = dh->data;
	dh->pos = 0;
	dh->endpos = getdents(dh->fd, dh->data, sizeof(dh->data));
	if (dh->endpos <= 0)
	    return NULL;
    }

    ret = dh->curr->d_name;

    dh->pos += dh->curr->d_reclen;
    dh->curr = (struct dirent *)((char *)dh->data + dh->pos);

    return ret;
}

void close_dir(dirhandle *dh)
{
    close(dh->fd);
}

#endif /* !defined __linux__ || defined HAVE_FDOPENDIR */

static int str_cmp(const void *av, const void *bv)
{
    return strcmp(*(const char **)av, *(const char **)bv);
}

static void du_recurse(char **path, size_t pathlen, size_t *pathsize,
		       gotdata_fn_t gotdata, void *gotdata_ctx)
{
    const char *name;
    dirhandle d;
    STRUCT_STAT st;
    char **names;
    size_t i, nnames, namesize;

    if (LSTAT(*path, &st) < 0) {
	fprintf(stderr, "%s: lstat: %s\n", *path, strerror(errno));
	return;
    }

    if (!gotdata(gotdata_ctx, *path, &st))
	return;

    if (!S_ISDIR(st.st_mode))
	return;

    names = NULL;
    nnames = namesize = 0;

    if (open_dir(*path, &d) < 0) {
	fprintf(stderr, "%s: opendir: %s\n", *path, strerror(errno));
	return;
    }
    while ((name = read_dir(&d)) != NULL) {
	if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) {
	    /* do nothing - we skip "." and ".." */
	} else {
	    if (nnames >= namesize) {
		namesize = nnames * 3 / 2 + 64;
		names = sresize(names, namesize, char *);
	    }
	    names[nnames++] = dupstr(name);
	}
    }
    close_dir(&d);

    if (nnames == 0)
	return;

    qsort(names, nnames, sizeof(*names), str_cmp);

    for (i = 0; i < nnames; i++) {
	size_t newpathlen = pathlen + 1 + strlen(names[i]);
	if (*pathsize <= newpathlen) {
	    *pathsize = newpathlen * 3 / 2 + 256;
	    *path = sresize(*path, *pathsize, char);
	}
	/*
	 * Avoid duplicating a slash if we got a trailing one to
	 * begin with (i.e. if we're starting the scan in '/' itself).
	 */
	if (pathlen > 0 && (*path)[pathlen-1] == '/') {
	    strcpy(*path + pathlen, names[i]);
	    newpathlen--;
	} else {
	    sprintf(*path + pathlen, "/%s", names[i]);
	}

	du_recurse(path, newpathlen, pathsize, gotdata, gotdata_ctx);

	sfree(names[i]);
    }
    sfree(names);
}

void du(const char *inpath, gotdata_fn_t gotdata, void *gotdata_ctx)
{
    char *path;
    size_t pathlen, pathsize;

    pathlen = strlen(inpath);
    pathsize = pathlen + 256;
    path = snewn(pathsize, char);
    strcpy(path, inpath);

    du_recurse(&path, pathlen, &pathsize, gotdata, gotdata_ctx);
}
