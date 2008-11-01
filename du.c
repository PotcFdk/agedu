/*
 * du.c: implementation of du.h.
 */

#define _GNU_SOURCE
#include <features.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "du.h"
#include "malloc.h"

#ifdef __linux__
#define __KERNEL__
#include <unistd.h>
#include <fcntl.h>
#include <linux/types.h>
#include <linux/dirent.h>
#include <linux/unistd.h>
typedef int dirhandle;
typedef struct {
    struct dirent data[32];
    struct dirent *curr;
    int pos, endpos;
} direntry;
_syscall3(int, getdents, uint, fd, struct dirent *, dirp, uint, count)
#define OPENDIR(f) open(f, O_RDONLY | O_NOATIME | O_DIRECTORY)
#define DIRVALID(dh) ((dh) >= 0)
#define READDIR(dh,de) ((de).curr = (de).data, (de).pos = 0, \
    ((de).endpos = getdents((dh), (de).data, sizeof((de).data))) > 0)
#define DENAME(de) ((de).curr->d_name)
#define DEDONE(de) ((de).pos >= (de).endpos)
#define DEADVANCE(de) ((de).pos += (de).curr->d_reclen, \
    (de).curr = (struct dirent *)((char *)(de).data + (de).pos))
#define CLOSEDIR(dh) close(dh)
#else
#include <dirent.h>
typedef DIR *dirhandle;
typedef struct dirent *direntry;
#define OPENDIR(f) opendir(f)
#define DIRVALID(dh) ((dh) != NULL)
#define READDIR(dh,de) (((de) = readdir(dh)) ? 1 : 0)
#define DENAME(de) ((de)->d_name)
#define DEDONE(de) ((de) == NULL)
#define DEADVANCE(de) ((de) = NULL)
#define CLOSEDIR(dh) closedir(dh)
#endif

static int str_cmp(const void *av, const void *bv)
{
    return strcmp(*(const char **)av, *(const char **)bv);
}

static void du_recurse(char **path, size_t pathlen, size_t *pathsize,
		       gotdata_fn_t gotdata, void *gotdata_ctx)
{
    direntry de;
    dirhandle d;
    struct stat64 st;
    char **names;
    size_t i, nnames, namesize;

    if (lstat64(*path, &st) < 0) {
	fprintf(stderr, "%s: lstat: %s\n", *path, strerror(errno));
	return;
    }

    if (!gotdata(gotdata_ctx, *path, &st))
	return;

    if (!S_ISDIR(st.st_mode))
	return;

    names = NULL;
    nnames = namesize = 0;

    d = OPENDIR(*path);
    if (!DIRVALID(d)) {
	fprintf(stderr, "%s: opendir: %s\n", *path, strerror(errno));
	return;
    }
    while (READDIR(d, de)) {
	do {
	    const char *name = DENAME(de);
	    if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) {
		/* do nothing - we skip "." and ".." */
	    } else {
		if (nnames >= namesize) {
		    namesize = nnames * 3 / 2 + 64;
		    names = sresize(names, namesize, char *);
		}
		names[nnames++] = dupstr(name);
	    }
	    DEADVANCE(de);
	} while (!DEDONE(de));
    }
    CLOSEDIR(d);

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

void du(char *inpath, gotdata_fn_t gotdata, void *gotdata_ctx)
{
    char *path;
    size_t pathlen, pathsize;

    pathlen = strlen(inpath);
    pathsize = pathlen + 256;
    path = snewn(pathsize, char);
    strcpy(path, inpath);

    du_recurse(&path, pathlen, &pathsize, gotdata, gotdata_ctx);
}
