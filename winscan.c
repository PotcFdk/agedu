#include <windows.h>

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define lenof(x) (sizeof((x))/sizeof(*(x)))

#define snew(type) \
    ( (type *) malloc (sizeof (type)) )
#define snewn(number, type) \
    ( (type *) malloc ((number) * sizeof (type)) )
#define sresize(array, number, type) \
    ( (void)sizeof((array)-(type *)0), \
      (type *) realloc ((array), (number) * sizeof (type)) )
#define sfree free
char *dupstr(const char *s) {
    char *r = malloc(1+strlen(s));
    strcpy(r,s);
    return r;
}

typedef struct {
    HANDLE hdl;
    WIN32_FIND_DATA fdata;
    bool got_one, eod;
} dirhandle;



int open_dir(char *path, dirhandle *dh)
{
    strcat(path, "\\*");
    dh->hdl = FindFirstFile(path, &dh->fdata);
    if (dh->hdl == INVALID_HANDLE_VALUE) {
	int err = GetLastError();
	if (err == ERROR_FILE_NOT_FOUND) {
	    dh->eod = true;
	    dh->got_one = false;
	    return 0;
	} else {
	    return -err;
	}
    } else {
	dh->eod = false;
	dh->got_one = true;
	return 0;
    }
}

const char *read_dir(dirhandle *dh)
{
    if (!dh->got_one) {
	if (dh->eod)
	    return NULL;

	if (FindNextFile(dh->hdl, &dh->fdata)) {
	    dh->got_one = true;
	} else {
	    dh->eod = true;
	    return NULL;
	}
    }

    dh->got_one = false;
    return dh->fdata.cFileName;
}

void close_dir(dirhandle *dh)
{
    CloseHandle(dh->hdl);
}

static int str_cmp(const void *av, const void *bv)
{
    return strcmp(*(const char **)av, *(const char **)bv);
}

typedef int (*gotdata_fn_t)(void *ctx, const char *pathname,
			    WIN32_FIND_DATA *dat);

static void format_error(char *buf, size_t size, int err)
{
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, err, 0, buf, size, NULL);
    buf[size-1] = '\0';
    if (buf[strlen(buf)-1] == '\n')
        buf[strlen(buf)-1] = '\0';
}

static void du_recurse(char **path, size_t pathlen, size_t *pathsize,
		       gotdata_fn_t gotdata, void *gotdata_ctx)
{
    const char *name;
    dirhandle d;
    char **names;
    int error;
    size_t i, nnames, namesize;
    WIN32_FIND_DATA dat;
    HANDLE h;

    if (*pathsize <= pathlen + 10) {
	*pathsize = pathlen * 3 / 2 + 256;
	*path = sresize(*path, *pathsize, char);
    }

    h = FindFirstFile(*path, &dat);
    if (h != INVALID_HANDLE_VALUE) {
        /* The normal case: we retrieved information about the file in
         * 'dat', and we also got a handle we can pass to
         * FindNextFile. We don't need the latter, so close it
         * immediately. */
	CloseHandle(h);
    } else if (pathlen > 0 && (*path)[pathlen-1] == '\\') {
        /* Special case: we're scanning a subdirectory such as c:\ .
         * In that case, take it to have zero size, and resort to
         * GetFileTime to find its times. */
	dat.nFileSizeHigh = dat.nFileSizeLow = 0;
	dat.ftLastWriteTime.dwHighDateTime = 0x19DB1DE;
	dat.ftLastWriteTime.dwLowDateTime = 0xD53E8000;
	dat.ftLastAccessTime.dwHighDateTime = 0x19DB1DE;
	dat.ftLastAccessTime.dwLowDateTime = 0xD53E8000;
	h = CreateFile(*path, GENERIC_READ, FILE_SHARE_WRITE, NULL,
		       OPEN_EXISTING, 0, NULL);
	if (h != INVALID_HANDLE_VALUE) {
	    GetFileTime(h, &dat.ftCreationTime, &dat.ftLastAccessTime,
			&dat.ftLastWriteTime);
	    CloseHandle(h);
	}
    } else {
        /* Total failure: FindFirstFile returned INVALID_HANDLE_VALUE
         * and it _wasn't_ because the file is a special directory. */
        char buf[4096];
        format_error(buf, lenof(buf), GetLastError());
	fprintf(stderr, "Unable to scan '%s': %s\n", *path, buf);
        return;
    }

    if (!gotdata(gotdata_ctx, *path, &dat))
	return;

    if (!(GetFileAttributes(*path) & FILE_ATTRIBUTE_DIRECTORY))
	return;

    names = NULL;
    nnames = namesize = 0;

    if ((error = open_dir(*path, &d)) < 0) {
        char buf[4096];
        format_error(buf, lenof(buf), -error);
	fprintf(stderr, "Unable to open directory '%s': %s\n", *path, buf);
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
	 * begin with (i.e. if we're starting the scan in '\\' itself).
	 */
	if (pathlen > 0 && (*path)[pathlen-1] == '\\') {
	    strcpy(*path + pathlen, names[i]);
	    newpathlen--;
	} else {
	    sprintf(*path + pathlen, "\\%s", names[i]);
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

int gd(void *ctx, const char *pathname, WIN32_FIND_DATA *dat)
{
    unsigned long long size, t;
    FILETIME ft;
    const char *p;

    size = dat->nFileSizeHigh;
    size = (size << 32) | dat->nFileSizeLow;
    printf("%llu ", size);

    if (dat->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
	ft = dat->ftLastWriteTime;
    else
	ft = dat->ftLastAccessTime;
    t = ft.dwHighDateTime;
    t = (t << 32) | ft.dwLowDateTime;
    t /= 10000000;
    /*
     * Correction factor: number of seconds between Windows's file
     * time epoch of 1 Jan 1601 and Unix's time_t epoch of 1 Jan 1970.
     *
     * That's 369 years, of which 92 were divisible by 4, but
     * three of those were century points.
     */
    t -= (369 * 365 + 92 - 3) * (unsigned long long)86400;
    printf("%llu ", t);

    for (p = pathname; *p; p++) {
	if (*p >= ' ' && *p < 127 && *p != '%')
	    putchar(*p);
	else
	    printf("%%%02x", (unsigned char)*p);
    }
    putchar('\n');
    return 1;
}

int main(int argc, char **argv)
{
    char *dir;
    int dirlen, dirsize;

    if (argc > 1)
	SetCurrentDirectory(argv[1]);

    dirsize = 512;
    dir = snewn(dirsize, char);
    while ((dirlen = GetCurrentDirectory(dirsize, dir)) >= dirsize) {
	dirsize = dirlen + 256;
	dir = sresize(dir, dirsize, char);
    }

    printf("agedu dump file. pathsep=5c\n");

    du(dir, gd, NULL);
}
