/*
 * Main program for agedu.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <fnmatch.h>

#include "du.h"
#include "trie.h"
#include "index.h"
#include "malloc.h"
#include "html.h"
#include "httpd.h"

#define PNAME "agedu"

void fatal(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", PNAME);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

struct inclusion_exclusion {
    int include;
    const char *wildcard;
    int path;
};

struct ctx {
    triebuild *tb;
    dev_t datafile_dev, filesystem_dev;
    ino_t datafile_ino;
    time_t last_output_update;
    int progress, progwidth;
    struct inclusion_exclusion *inex;
    int ninex;
    int crossfs;
};

static int gotdata(void *vctx, const char *pathname, const struct stat64 *st)
{
    struct ctx *ctx = (struct ctx *)vctx;
    struct trie_file file;
    time_t t;
    int i, include;
    const char *filename;

    /*
     * Filter out our own data file.
     */
    if (st->st_dev == ctx->datafile_dev && st->st_ino == ctx->datafile_ino)
	return 0;

    /*
     * Don't cross the streams^W^Wany file system boundary.
     */
    if (!ctx->crossfs && st->st_dev != ctx->filesystem_dev)
	return 0;

    /*
     * Filter based on wildcards.
     */
    include = 1;
    filename = strrchr(pathname, '/');
    if (!filename)
	filename = pathname;
    else
	filename++;
    for (i = 0; i < ctx->ninex; i++) {
	if (fnmatch(ctx->inex[i].wildcard,
		    ctx->inex[i].path ? pathname : filename,
		    FNM_PATHNAME) == 0)
	    include = ctx->inex[i].include;
    }
    if (!include)
	return 1;		       /* filter, but don't prune */

    file.blocks = st->st_blocks;
    file.atime = st->st_atime;
    triebuild_add(ctx->tb, pathname, &file);

    t = time(NULL);
    if (t != ctx->last_output_update) {
	if (ctx->progress) {
	    fprintf(stderr, "%-*.*s\r", ctx->progwidth, ctx->progwidth,
		    pathname);
	    fflush(stderr);
	}
	ctx->last_output_update = t;
    }

    return 1;
}

static void run_query(const void *mappedfile, const char *rootdir,
		      time_t t, int depth)
{
    size_t maxpathlen;
    char *pathbuf;
    unsigned long xi1, xi2;
    unsigned long long s1, s2;

    maxpathlen = trie_maxpathlen(mappedfile);
    pathbuf = snewn(maxpathlen + 1, char);

    /*
     * We want to query everything between the supplied filename
     * (inclusive) and that filename with a ^A on the end
     * (exclusive). So find the x indices for each.
     */
    sprintf(pathbuf, "%s\001", rootdir);
    xi1 = trie_before(mappedfile, rootdir);
    xi2 = trie_before(mappedfile, pathbuf);

    /*
     * Now do the lookups in the age index.
     */
    s1 = index_query(mappedfile, xi1, t);
    s2 = index_query(mappedfile, xi2, t);

    /* Display in units of 2 512-byte blocks = 1Kb */
    printf("%-11llu %s\n", (s2 - s1) / 2, rootdir);

    if (depth > 0) {
	/*
	 * Now scan for first-level subdirectories and report
	 * those too.
	 */
	xi1++;
	while (xi1 < xi2) {
	    trie_getpath(mappedfile, xi1, pathbuf);
	    run_query(mappedfile, pathbuf, t, depth-1);
	    strcat(pathbuf, "\001");
	    xi1 = trie_before(mappedfile, pathbuf);
	}
    }
}

int main(int argc, char **argv)
{
    int fd, count;
    struct ctx actx, *ctx = &actx;
    struct stat st;
    off_t totalsize, realsize;
    void *mappedfile;
    triewalk *tw;
    indexbuild *ib;
    const struct trie_file *tf;
    char *filename = "agedu.dat";
    char *rootdir = NULL;
    int doing_opts = 1;
    enum { QUERY, HTML, SCAN, DUMP, HTTPD } mode = QUERY;
    char *minage = "0d";
    int auth = HTTPD_AUTH_MAGIC | HTTPD_AUTH_BASIC;
    int progress = 1;
    struct inclusion_exclusion *inex = NULL;
    int ninex = 0, inexsize = 0;
    int crossfs = 0;

    while (--argc > 0) {
        char *p = *++argv;
        char *optval;

        if (doing_opts && *p == '-') {
            if (!strcmp(p, "--")) {
                doing_opts = 0;
            } else if (p[1] == '-') {
		char *optval = strchr(p, '=');
		if (optval)
		    *optval++ = '\0';
		if (!strcmp(p, "--help")) {
		    printf("FIXME: usage();\n");
		    return 0;
		} else if (!strcmp(p, "--version")) {
		    printf("FIXME: version();\n");
		    return 0;
		} else if (!strcmp(p, "--licence") ||
			   !strcmp(p, "--license")) {
		    printf("FIXME: licence();\n");
		    return 0;
		} else if (!strcmp(p, "--scan")) {
		    mode = SCAN;
		} else if (!strcmp(p, "--dump")) {
		    mode = DUMP;
		} else if (!strcmp(p, "--html")) {
		    mode = HTML;
		} else if (!strcmp(p, "--httpd") ||
			   !strcmp(p, "--server")) {
		    mode = HTTPD;
		} else if (!strcmp(p, "--progress") ||
			   !strcmp(p, "--scan-progress")) {
		    progress = 2;
		} else if (!strcmp(p, "--no-progress") ||
			   !strcmp(p, "--no-scan-progress")) {
		    progress = 0;
		} else if (!strcmp(p, "--tty-progress") ||
			   !strcmp(p, "--tty-scan-progress") ||
			   !strcmp(p, "--progress-tty") ||
			   !strcmp(p, "--scan-progress-tty")) {
		    progress = 1;
		} else if (!strcmp(p, "--crossfs")) {
		    crossfs = 1;
		} else if (!strcmp(p, "--no-crossfs")) {
		    crossfs = 0;
		} else if (!strcmp(p, "--file") ||
			   !strcmp(p, "--auth") ||
			   !strcmp(p, "--http-auth") ||
			   !strcmp(p, "--httpd-auth") ||
			   !strcmp(p, "--server-auth") ||
			   !strcmp(p, "--minimum-age") ||
			   !strcmp(p, "--min-age") ||
			   !strcmp(p, "--age") ||
			   !strcmp(p, "--include") ||
			   !strcmp(p, "--include-path") ||
			   !strcmp(p, "--exclude") ||
			   !strcmp(p, "--exclude-path")) {
		    /*
		     * Long options requiring values.
		     */
		    if (!optval) {
			if (--argc > 0) {
			    optval = *++argv;
			} else {
			    fprintf(stderr, "%s: option '%s' requires"
				    " an argument\n", PNAME, p);
			    return 1;
			}
		    }
		    if (!strcmp(p, "--file")) {
			filename = optval;
		    } else if (!strcmp(p, "--minimum-age") ||
			       !strcmp(p, "--min-age") ||
			       !strcmp(p, "--age")) {
			minage = optval;
		    } else if (!strcmp(p, "--auth") ||
			       !strcmp(p, "--http-auth") ||
			       !strcmp(p, "--httpd-auth") ||
			       !strcmp(p, "--server-auth")) {
			if (!strcmp(optval, "magic"))
			    auth = HTTPD_AUTH_MAGIC;
			else if (!strcmp(optval, "basic"))
			    auth = HTTPD_AUTH_BASIC;
			else if (!strcmp(optval, "none"))
			    auth = HTTPD_AUTH_NONE;
			else if (!strcmp(optval, "default"))
			    auth = HTTPD_AUTH_MAGIC | HTTPD_AUTH_BASIC;
			else {
			    fprintf(stderr, "%s: unrecognised authentication"
				    " type '%s'\n%*s  options are 'magic',"
				    " 'basic', 'none', 'default'\n",
				    PNAME, optval, (int)strlen(PNAME), "");
			    return 1;
			}
		    } else if (!strcmp(p, "--include") ||
			       !strcmp(p, "--include-path") ||
			       !strcmp(p, "--exclude") ||
			       !strcmp(p, "--exclude-path")) {
			if (ninex >= inexsize) {
			    inexsize = ninex * 3 / 2 + 16;
			    inex = sresize(inex, inexsize,
					   struct inclusion_exclusion);
			}
			inex[ninex].path = (!strcmp(p, "--include-path") ||
					    !strcmp(p, "--exclude-path"));
			inex[ninex].include = (!strcmp(p, "--include") ||
					       !strcmp(p, "--include-path"));
			inex[ninex].wildcard = optval;
			ninex++;
		    }
		} else {
		    fprintf(stderr, "%s: unrecognised option '%s'\n",
			    PNAME, p);
		    return 1;
		}
            } else {
                p++;
                while (*p) {
                    char c = *p++;

                    switch (c) {
                        /* Options requiring arguments. */
		      case 'f':
		      case 'a':
                        if (*p) {
                            optval = p;
                            p += strlen(p);
                        } else if (--argc > 0) {
                            optval = *++argv;
                        } else {
                            fprintf(stderr, "%s: option '-%c' requires"
                                    " an argument\n", PNAME, c);
                            return 1;
                        }
                        switch (c) {
			  case 'f':    /* data file name */
			    filename = optval;
			    break;
			  case 'a':    /* maximum age */
			    minage = optval;
			    break;
                        }
                        break;
		      case 's':
			mode = SCAN;
			break;
                      default:
                        fprintf(stderr, "%s: unrecognised option '-%c'\n",
                                PNAME, c);
                        return 1;
                    }
                }
            }
        } else {
	    if (!rootdir) {
		rootdir = p;
	    } else {
		fprintf(stderr, "%s: unexpected argument '%s'\n", PNAME, p);
		return 1;
	    }
        }
    }

    if (!rootdir)
	rootdir = ".";

    if (mode == SCAN) {

	fd = open(filename, O_RDWR | O_TRUNC | O_CREAT, S_IRWXU);
	if (fd < 0) {
	    fprintf(stderr, "%s: %s: open: %s\n", PNAME, filename,
		    strerror(errno));
	    return 1;
	}

	if (stat(rootdir, &st) < 0) {
	    fprintf(stderr, "%s: %s: stat: %s\n", PNAME, rootdir,
		    strerror(errno));
	    return 1;
	}
	ctx->filesystem_dev = crossfs ? 0 : st.st_dev;

	if (fstat(fd, &st) < 0) {
	    perror("agedu: fstat");
	    return 1;
	}
	ctx->datafile_dev = st.st_dev;
	ctx->datafile_ino = st.st_ino;
	ctx->inex = inex;
	ctx->ninex = ninex;
	ctx->crossfs = crossfs;

	ctx->last_output_update = time(NULL);

	/* progress==1 means report progress only if stderr is a tty */
	if (progress == 1)
	    progress = isatty(2) ? 2 : 0;
	ctx->progress = progress;
	{
	    struct winsize ws;
	    if (progress && ioctl(2, TIOCGWINSZ, &ws) == 0)
		ctx->progwidth = ws.ws_col - 1;
	    else
		ctx->progwidth = 79;
	}

	/*
	 * Scan the directory tree, and write out the trie component
	 * of the data file.
	 */
	ctx->tb = triebuild_new(fd);
	du(rootdir, gotdata, ctx);
	count = triebuild_finish(ctx->tb);
	triebuild_free(ctx->tb);

	if (ctx->progress) {
	    fprintf(stderr, "%-*s\r", ctx->progwidth, "");
	    fflush(stderr);
	}

	/*
	 * Work out how much space the cumulative index trees will
	 * take; enlarge the file, and memory-map it.
	 */
	if (fstat(fd, &st) < 0) {
	    perror("agedu: fstat");
	    return 1;
	}

	printf("Built pathname index, %d entries, %ju bytes\n", count,
	       (intmax_t)st.st_size);

	totalsize = index_compute_size(st.st_size, count);

	if (lseek(fd, totalsize-1, SEEK_SET) < 0) {
	    perror("agedu: lseek");
	    return 1;
	}
	if (write(fd, "\0", 1) < 1) {
	    perror("agedu: write");
	    return 1;
	}

	printf("Upper bound on index file size = %ju bytes\n",
	       (intmax_t)totalsize);

	mappedfile = mmap(NULL, totalsize, PROT_READ|PROT_WRITE,MAP_SHARED, fd, 0);
	if (!mappedfile) {
	    perror("agedu: mmap");
	    return 1;
	}

	ib = indexbuild_new(mappedfile, st.st_size, count);
	tw = triewalk_new(mappedfile);
	while ((tf = triewalk_next(tw, NULL)) != NULL)
	    indexbuild_add(ib, tf);
	triewalk_free(tw);
	realsize = indexbuild_realsize(ib);
	indexbuild_free(ib);

	munmap(mappedfile, totalsize);
	ftruncate(fd, realsize);
	close(fd);
	printf("Actual index file size = %ju bytes\n", (intmax_t)realsize);
    } else if (mode == QUERY) {
	time_t t;
	struct tm tm;
	int nunits;
	char unit[2];
	size_t pathlen;

	t = time(NULL);

	if (2 != sscanf(minage, "%d%1[DdWwMmYy]", &nunits, unit)) {
	    fprintf(stderr, "%s: minimum age should be a number followed by"
		    " one of d,w,m,y\n", PNAME);
	    return 1;
	}

	if (unit[0] == 'd') {
	    t -= 86400 * nunits;
	} else if (unit[0] == 'w') {
	    t -= 86400 * 7 * nunits;
	} else {
	    int ym;

	    tm = *localtime(&t);
	    ym = tm.tm_year * 12 + tm.tm_mon;

	    if (unit[0] == 'm')
		ym -= nunits;
	    else
		ym -= 12 * nunits;

	    tm.tm_year = ym / 12;
	    tm.tm_mon = ym % 12;

	    t = mktime(&tm);
	}

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
	    fprintf(stderr, "%s: %s: open: %s\n", PNAME, filename,
		    strerror(errno));
	    return 1;
	}
	if (fstat(fd, &st) < 0) {
	    perror("agedu: fstat");
	    return 1;
	}
	totalsize = st.st_size;
	mappedfile = mmap(NULL, totalsize, PROT_READ, MAP_SHARED, fd, 0);
	if (!mappedfile) {
	    perror("agedu: mmap");
	    return 1;
	}

	/*
	 * Trim trailing slash, just in case.
	 */
	pathlen = strlen(rootdir);
	if (pathlen > 0 && rootdir[pathlen-1] == '/')
	    rootdir[--pathlen] = '\0';

	run_query(mappedfile, rootdir, t, 1);
    } else if (mode == HTML) {
	size_t pathlen;
	unsigned long xi;
	char *html;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
	    fprintf(stderr, "%s: %s: open: %s\n", PNAME, filename,
		    strerror(errno));
	    return 1;
	}
	if (fstat(fd, &st) < 0) {
	    perror("agedu: fstat");
	    return 1;
	}
	totalsize = st.st_size;
	mappedfile = mmap(NULL, totalsize, PROT_READ, MAP_SHARED, fd, 0);
	if (!mappedfile) {
	    perror("agedu: mmap");
	    return 1;
	}

	/*
	 * Trim trailing slash, just in case.
	 */
	pathlen = strlen(rootdir);
	if (pathlen > 0 && rootdir[pathlen-1] == '/')
	    rootdir[--pathlen] = '\0';

	xi = trie_before(mappedfile, rootdir);
	html = html_query(mappedfile, xi, NULL);
	fputs(html, stdout);
    } else if (mode == DUMP) {
	size_t maxpathlen;
	char *buf;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
	    fprintf(stderr, "%s: %s: open: %s\n", PNAME, filename,
		    strerror(errno));
	    return 1;
	}
	if (fstat(fd, &st) < 0) {
	    perror("agedu: fstat");
	    return 1;
	}
	totalsize = st.st_size;
	mappedfile = mmap(NULL, totalsize, PROT_READ, MAP_SHARED, fd, 0);
	if (!mappedfile) {
	    perror("agedu: mmap");
	    return 1;
	}

	maxpathlen = trie_maxpathlen(mappedfile);
	buf = snewn(maxpathlen, char);

	tw = triewalk_new(mappedfile);
	while ((tf = triewalk_next(tw, buf)) != NULL) {
	    printf("%s: %llu %llu\n", buf, tf->blocks, tf->atime);
	}
	triewalk_free(tw);
    } else if (mode == HTTPD) {
	fd = open(filename, O_RDONLY);
	if (fd < 0) {
	    fprintf(stderr, "%s: %s: open: %s\n", PNAME, filename,
		    strerror(errno));
	    return 1;
	}
	if (fstat(fd, &st) < 0) {
	    perror("agedu: fstat");
	    return 1;
	}
	totalsize = st.st_size;
	mappedfile = mmap(NULL, totalsize, PROT_READ, MAP_SHARED, fd, 0);
	if (!mappedfile) {
	    perror("agedu: mmap");
	    return 1;
	}

	run_httpd(mappedfile, auth);
    }

    return 0;
}
