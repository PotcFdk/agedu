/*
 * html.c: implementation of html.h.
 */

#include "agedu.h"
#include "html.h"
#include "alloc.h"
#include "trie.h"
#include "index.h"

#define MAXCOLOUR 511

struct html {
    char *buf;
    size_t buflen, bufsize;
    const void *t;
    unsigned long long totalsize, oldest, newest;
    char *path2;
    char *href;
    size_t hreflen;
    const char *format, *rootpage;
    unsigned long long thresholds[MAXCOLOUR];
    char *titletexts[MAXCOLOUR+1];
    time_t now;
};

static void vhtprintf(struct html *ctx, const char *fmt, va_list ap)
{
    va_list ap2;
    int size, size2;
    char testbuf[2];

    va_copy(ap2, ap);
    /*
     * Some C libraries (Solaris, I'm looking at you) don't like
     * an output buffer size of zero in vsnprintf, but will return
     * sensible values given any non-zero buffer size. Hence, we
     * use testbuf to gauge the length of the string.
     */
    size = vsnprintf(testbuf, 1, fmt, ap2);
    va_end(ap2);

    if (ctx->buflen + size >= ctx->bufsize) {
	ctx->bufsize = (ctx->buflen + size) * 3 / 2 + 1024;
	ctx->buf = sresize(ctx->buf, ctx->bufsize, char);
    }
    size2 = vsnprintf(ctx->buf + ctx->buflen, ctx->bufsize - ctx->buflen,
		      fmt, ap);
    assert(size == size2);
    ctx->buflen += size;
}

static void htprintf(struct html *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vhtprintf(ctx, fmt, ap);
    va_end(ap);
}

static unsigned long long round_and_format_age(struct html *ctx,
					       unsigned long long age,
					       char *buf, int direction)
{
    struct tm tm, tm2;
    char newbuf[80];
    unsigned long long ret, newret;
    int i;
    int ym;
    static const int minutes[] = { 5, 10, 15, 30, 45 };

    tm = *localtime(&ctx->now);
    ym = tm.tm_year * 12 + tm.tm_mon;

    ret = ctx->now;
    strcpy(buf, "Now");

    for (i = 0; i < lenof(minutes); i++) {
	newret = ctx->now - minutes[i] * 60;
	sprintf(newbuf, "%d minutes", minutes[i]);
	if (newret < age)
	    goto finish;
	strcpy(buf, newbuf);
	ret = newret;
    }

    for (i = 1; i < 24; i++) {
	newret = ctx->now - i * (60*60);
	sprintf(newbuf, "%d hour%s", i, i==1 ? "" : "s");
	if (newret < age)
	    goto finish;
	strcpy(buf, newbuf);
	ret = newret;
    }

    for (i = 1; i < 7; i++) {
	newret = ctx->now - i * (24*60*60);
	sprintf(newbuf, "%d day%s", i, i==1 ? "" : "s");
	if (newret < age)
	    goto finish;
	strcpy(buf, newbuf);
	ret = newret;
    }

    for (i = 1; i < 4; i++) {
	newret = ctx->now - i * (7*24*60*60);
	sprintf(newbuf, "%d week%s", i, i==1 ? "" : "s");
	if (newret < age)
	    goto finish;
	strcpy(buf, newbuf);
	ret = newret;
    }

    for (i = 1; i < 11; i++) {
	tm2 = tm;		       /* structure copy */
	tm2.tm_year = (ym - i) / 12;
	tm2.tm_mon = (ym - i) % 12;
	newret = mktime(&tm2);
	sprintf(newbuf, "%d month%s", i, i==1 ? "" : "s");
	if (newret < age)
	    goto finish;
	strcpy(buf, newbuf);
	ret = newret;
    }

    for (i = 1;; i++) {
	tm2 = tm;		       /* structure copy */
	tm2.tm_year = (ym - i*12) / 12;
	tm2.tm_mon = (ym - i*12) % 12;
	newret = mktime(&tm2);
	sprintf(newbuf, "%d year%s", i, i==1 ? "" : "s");
	if (newret < age)
	    goto finish;
	strcpy(buf, newbuf);
	ret = newret;
    }

    finish:
    if (direction > 0) {
	/*
	 * Round toward newest, i.e. use the existing (buf,ret).
	 */
    } else if (direction < 0) {
	/*
	 * Round toward oldest, i.e. use (newbuf,newret);
	 */
	strcpy(buf, newbuf);
	ret = newret;
    } else {
	/*
	 * Round to nearest.
	 */
	if (ret - age > age - newret) {
	    strcpy(buf, newbuf);
	    ret = newret;
	}
    }
    return ret;
}

static void get_indices(const void *t, char *path,
			unsigned long *xi1, unsigned long *xi2)
{
    size_t pathlen = strlen(path);
    int c1 = path[pathlen], c2 = (pathlen > 0 ? path[pathlen-1] : 0);

    *xi1 = trie_before(t, path);
    make_successor(path);
    *xi2 = trie_before(t, path);
    path[pathlen] = c1;
    if (pathlen > 0)
	path[pathlen-1] = c2;
}

static unsigned long long fetch_size(const void *t,
				     unsigned long xi1, unsigned long xi2,
				     unsigned long long atime)
{
    if (xi2 - xi1 == 1) {
	/*
	 * We are querying an individual file, so we should not
	 * depend on the index entries either side of the node,
	 * since they almost certainly don't both exist. Instead,
	 * just look up the file's size and atime in the main trie.
	 */
	const struct trie_file *f = trie_getfile(t, xi1);
	if (f->atime < atime)
	    return f->size;
	else
	    return 0;
    } else {
	return index_query(t, xi2, atime) - index_query(t, xi1, atime);
    }
}

static void htescape(struct html *ctx, const char *s, int n, int italics)
{
    while (n > 0 && *s) {
	unsigned char c = (unsigned char)*s++;

	if (c == '&')
	    htprintf(ctx, "&amp;");
	else if (c == '<')
	    htprintf(ctx, "&lt;");
	else if (c == '>')
	    htprintf(ctx, "&gt;");
	else if (c >= ' ' && c < '\177')
	    htprintf(ctx, "%c", c);
	else {
	    if (italics) htprintf(ctx, "<i>");
	    htprintf(ctx, "[%02x]", c);
	    if (italics) htprintf(ctx, "</i>");
	}

	n--;
    }
}

static void begin_colour_bar(struct html *ctx)
{
    htprintf(ctx, "<table cellspacing=0 cellpadding=0"
	     " style=\"border:0\">\n<tr>\n");
}

static void add_to_colour_bar(struct html *ctx, int colour, int pixels)
{
    int r, g, b;

    if (colour >= 0 && colour < 256)   /* red -> yellow fade */
	r = 255, g = colour, b = 0;
    else if (colour >= 256 && colour <= 511)   /* yellow -> green fade */
	r = 511 - colour, g = 255, b = 0;
    else			       /* background grey */
	r = g = b = 240;

    if (pixels > 0) {
	htprintf(ctx, "<td style=\"width:%dpx; height:1em; "
		 "background-color:#%02x%02x%02x\"",
		 pixels, r, g, b);
	if (colour >= 0)
	    htprintf(ctx, " title=\"%s\"", ctx->titletexts[colour]);
	htprintf(ctx, "></td>\n");
    }
}

static void end_colour_bar(struct html *ctx)
{
    htprintf(ctx, "</tr>\n</table>\n");
}

struct vector {
    int want_href, essential;
    char *name;
    int literal; /* should the name be formatted in fixed-pitch? */
    unsigned long index;
    unsigned long long sizes[MAXCOLOUR+1];
};

int vec_compare(const void *av, const void *bv)
{
    const struct vector *a = *(const struct vector **)av;
    const struct vector *b = *(const struct vector **)bv;

    if (a->sizes[MAXCOLOUR] > b->sizes[MAXCOLOUR])
	return -1;
    else if (a->sizes[MAXCOLOUR] < b->sizes[MAXCOLOUR])
	return +1;
    else if (a->want_href < b->want_href)
	return +1;
    else if (a->want_href > b->want_href)
	return -1;
    else if (a->want_href)
	return strcmp(a->name, b->name);
    else if (a->index < b->index)
	return -1;
    else if (a->index > b->index)
	return +1;
    else if (a->essential < b->essential)
	return +1;
    else if (a->essential > b->essential)
	return -1;
    return 0;
}

static struct vector *make_vector(struct html *ctx, char *path,
				  int want_href, int essential,
				  char *name, int literal)
{
    unsigned long xi1, xi2;
    struct vector *vec = snew(struct vector);
    int i;

    vec->want_href = want_href;
    vec->essential = essential;
    vec->name = name ? dupstr(name) : NULL;
    vec->literal = literal;

    get_indices(ctx->t, path, &xi1, &xi2);

    vec->index = xi1;

    for (i = 0; i <= MAXCOLOUR; i++) {
	unsigned long long atime;
	if (i == MAXCOLOUR)
	    atime = ULLONG_MAX;
	else
	    atime = ctx->thresholds[i];
	vec->sizes[i] = fetch_size(ctx->t, xi1, xi2, atime);
    }

    return vec;
}

static void print_heading(struct html *ctx, const char *title)
{
    htprintf(ctx, "<tr style=\"padding: 0.2em; background-color:#e0e0e0\">\n"
	     "<td colspan=4 align=center>%s</td>\n</tr>\n", title);
}

static void compute_display_size(unsigned long long size,
				 const char **fmt, double *display_size)
{
    static const char *const fmts[] = {
	"%g b", "%g Kb", "%#.1f Mb", "%#.1f Gb", "%#.1f Tb",
	"%#.1f Pb", "%#.1f Eb", "%#.1f Zb", "%#.1f Yb"
    };
    int shift = 0;

    while (size >= 1024 && shift < lenof(fmts)-1) {
	size >>= 10;
	shift++;
    }
    *display_size = (double)size;
    *fmt = fmts[shift];
}

static void make_filename(char *buf, size_t buflen,
			  const char *format, const char *rootpage,
			  unsigned long index)
{
    if (index == 0 && rootpage)
	snprintf(buf, buflen, "%s", rootpage);
    else
	snprintf(buf, buflen, format, index);
}

#define PIXEL_SIZE 600		       /* FIXME: configurability? */
static void write_report_line(struct html *ctx, struct vector *vec)
{
    unsigned long long size, asize, divisor;
    double display_size;
    int pix, newpix;
    int i;
    const char *unitsfmt;

    /*
     * A line with literally zero space usage should not be
     * printed at all if it's a link to a subdirectory (since it
     * probably means the whole thing was excluded by some
     * --exclude-path wildcard). If it's [files] or the top-level
     * line, though, we must always print _something_, and in that
     * case we must fiddle about to prevent divisions by zero in
     * the code below.
     */
    if (!vec->sizes[MAXCOLOUR] && !vec->essential)
	return;
    divisor = ctx->totalsize;
    if (!divisor) {
	divisor = 1;
    }

    /*
     * Find the total size of this subdirectory.
     */
    size = vec->sizes[MAXCOLOUR];
    compute_display_size(size, &unitsfmt, &display_size);
    htprintf(ctx, "<tr>\n"
              "<td style=\"padding: 0.2em; text-align: right\">");
    htprintf(ctx, unitsfmt, display_size);
    htprintf(ctx, "</td>\n");

    /*
     * Generate a colour bar.
     */
    htprintf(ctx, "<td style=\"padding: 0.2em\">\n");
    begin_colour_bar(ctx);
    pix = 0;
    for (i = 0; i <= MAXCOLOUR; i++) {
	asize = vec->sizes[i];
	newpix = asize * PIXEL_SIZE / divisor;
	add_to_colour_bar(ctx, i, newpix - pix);
	pix = newpix;
    }
    add_to_colour_bar(ctx, -1, PIXEL_SIZE - pix);
    end_colour_bar(ctx);
    htprintf(ctx, "</td>\n");

    /*
     * Output size as a percentage of totalsize.
     */
    htprintf(ctx, "<td style=\"padding: 0.2em; text-align: right\">"
	     "%.2f%%</td>\n", (double)size / divisor * 100.0);

    /*
     * Output a subdirectory marker.
     */
    htprintf(ctx, "<td style=\"padding: 0.2em\">");
    if (vec->name) {
	int doing_href = 0;

	if (ctx->format && vec->want_href) {
	    make_filename(ctx->href, ctx->hreflen,
			  ctx->format, ctx->rootpage,
			  vec->index);
	    htprintf(ctx, "<a href=\"%s\">", ctx->href);
	    doing_href = 1;
	}
	if (vec->literal)
	    htprintf(ctx, "<code>");
	htescape(ctx, vec->name, strlen(vec->name), 1);
	if (vec->literal)
	    htprintf(ctx, "</code>");
	if (doing_href)
	    htprintf(ctx, "</a>");
    }
    htprintf(ctx, "</td>\n</tr>\n");
}

int strcmptrailingpathsep(const char *a, const char *b)
{
    while (*a == *b && *a)
	a++, b++;

    if ((*a == pathsep && !a[1] && !*b) ||
	(*b == pathsep && !b[1] && !*a))
	return 0;

    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *html_query(const void *t, unsigned long index,
		 const struct html_config *cfg, int downlink)
{
    struct html actx, *ctx = &actx;
    char *path, *path2, *p, *q, *href;
    char agebuf1[80], agebuf2[80];
    size_t pathlen, subdirpos, hreflen;
    unsigned long index2;
    int i;
    struct vector **vecs;
    int nvecs, vecsize;
    unsigned long xi1, xi2, xj1, xj2;

    if (index >= trie_count(t))
	return NULL;

    ctx->buf = NULL;
    ctx->buflen = ctx->bufsize = 0;
    ctx->t = t;
    ctx->format = cfg->format;
    ctx->rootpage = cfg->rootpage;
    htprintf(ctx, "<html>\n");

    path = snewn(1+trie_maxpathlen(t), char);
    ctx->path2 = path2 = snewn(1+trie_maxpathlen(t), char);
    if (cfg->format) {
	hreflen = strlen(cfg->format) + 100;
	href = snewn(hreflen, char);
    } else {
	hreflen = 0;
	href = NULL;
    }
    ctx->hreflen = hreflen;
    ctx->href = href;

    /*
     * HEAD section.
     */
    htprintf(ctx, "<head>\n");
    trie_getpath(t, index, path);
    htprintf(ctx, "<title>%s: ", PNAME);
    htescape(ctx, path, strlen(path), 0);
    htprintf(ctx, "</title>\n");
    htprintf(ctx, "</head>\n");

    /*
     * Begin BODY section.
     */
    htprintf(ctx, "<body>\n");
    htprintf(ctx, "<h3 align=center>Disk space breakdown by"
	     " last-access time</h3>\n");

    /*
     * Show the pathname we're centred on, with hyperlinks to
     * parent directories where available.
     */
    htprintf(ctx, "<p align=center>\n<code>");
    q = path;
    for (p = strchr(path, pathsep); p && p[1]; p = strchr(p, pathsep)) {
	int doing_href = 0;
	char c, *zp;

	/*
	 * See if this path prefix exists in the trie. If so,
	 * generate a hyperlink.
	 */
	zp = p;
	if (p == path)		       /* special case for "/" at start */
	    zp++;

	p++;

	c = *zp;
	*zp = '\0';
	index2 = trie_before(t, path);
	trie_getpath(t, index2, path2);
	if (!strcmptrailingpathsep(path, path2) && cfg->format) {
	    make_filename(href, hreflen, cfg->format, cfg->rootpage, index2);
	    if (!*href)		       /* special case that we understand */
		strcpy(href, "./");
	    htprintf(ctx, "<a href=\"%s\">", href);
	    doing_href = 1;
	}
	*zp = c;
	htescape(ctx, q, zp - q, 1);
	if (doing_href)
	    htprintf(ctx, "</a>");
	htescape(ctx, zp, p - zp, 1);
	q = p;
    }
    htescape(ctx, q, strlen(q), 1);
    htprintf(ctx, "</code>\n");

    /*
     * Decide on the age limit of our colour coding, establish the
     * colour thresholds, and write out a key.
     */
    ctx->now = time(NULL);
    if (cfg->autoage) {
	ctx->oldest = index_order_stat(t, 0.05);
	ctx->newest = index_order_stat(t, 1.0);
	ctx->oldest = round_and_format_age(ctx, ctx->oldest, agebuf1, -1);
	ctx->newest = round_and_format_age(ctx, ctx->newest, agebuf2, +1);
    } else {
	ctx->oldest = cfg->oldest;
	ctx->newest = cfg->newest;
	ctx->oldest = round_and_format_age(ctx, ctx->oldest, agebuf1, 0);
	ctx->newest = round_and_format_age(ctx, ctx->newest, agebuf2, 0);
    }
    for (i = 0; i < MAXCOLOUR; i++) {
	ctx->thresholds[i] =
	    ctx->oldest + (ctx->newest - ctx->oldest) * i / (MAXCOLOUR-1);
    }
    for (i = 0; i <= MAXCOLOUR; i++) {
	char buf[80];

	if (i == 0) {
	    strcpy(buf, "&lt; ");
	    round_and_format_age(ctx, ctx->thresholds[0], buf+5, 0);
	} else if (i == MAXCOLOUR) {
	    strcpy(buf, "&gt; ");
	    round_and_format_age(ctx, ctx->thresholds[MAXCOLOUR-1], buf+5, 0);
	} else {
	    unsigned long long midrange =
		(ctx->thresholds[i-1] + ctx->thresholds[i]) / 2;
	    round_and_format_age(ctx, midrange, buf, 0);
	}

	ctx->titletexts[i] = dupstr(buf);
    }
    htprintf(ctx, "<p align=center>Key to colour coding (mouse over for more detail):\n");
    htprintf(ctx, "<p align=center style=\"padding: 0; margin-top:0.4em; "
	     "margin-bottom:1em\">");
    begin_colour_bar(ctx);
    htprintf(ctx, "<td style=\"padding-right:1em\">%s</td>\n", agebuf1);
    for (i = 0; i < MAXCOLOUR; i++)
	add_to_colour_bar(ctx, i, 1);
    htprintf(ctx, "<td style=\"padding-left:1em\">%s</td>\n", agebuf2);
    end_colour_bar(ctx);

    /*
     * Begin the main table.
     */
    htprintf(ctx, "<p align=center>\n<table style=\"margin:0; border:0\">\n");

    /*
     * Find the total size of our entire subdirectory. We'll use
     * that as the scale for all the colour bars in this report.
     */
    get_indices(t, path, &xi1, &xi2);
    ctx->totalsize = fetch_size(t, xi1, xi2, ULLONG_MAX);

    /*
     * Generate a report line for the whole subdirectory.
     */
    vecsize = 64;
    vecs = snewn(vecsize, struct vector *);
    nvecs = 1;
    vecs[0] = make_vector(ctx, path, 0, 1, NULL, 0);
    print_heading(ctx, "Overall");
    write_report_line(ctx, vecs[0]);

    /*
     * Now generate report lines for all its children, and the
     * files contained in it.
     */
    print_heading(ctx, "Subdirectories");

    vecs[0]->name = dupstr("[files]");
    get_indices(t, path, &xi1, &xi2);
    xi1++;
    pathlen = strlen(path);
    subdirpos = pathlen + 1;
    if (pathlen > 0 && path[pathlen-1] == pathsep)
	subdirpos--;
    while (xi1 < xi2) {
	trie_getpath(t, xi1, path2);
	get_indices(t, ctx->path2, &xj1, &xj2);
	xi1 = xj2;
	if (!cfg->showfiles && xj2 - xj1 <= 1)
	    continue;		       /* skip individual files */
	if (nvecs >= vecsize) {
	    vecsize = nvecs * 3 / 2 + 64;
	    vecs = sresize(vecs, vecsize, struct vector *);
	}
	assert(strlen(path2) > pathlen);
	vecs[nvecs] = make_vector(ctx, path2, downlink && (xj2 - xj1 > 1), 0,
				  path2 + subdirpos, 1);
	for (i = 0; i <= MAXCOLOUR; i++)
	    vecs[0]->sizes[i] -= vecs[nvecs]->sizes[i];
	nvecs++;
    }

    qsort(vecs, nvecs, sizeof(vecs[0]), vec_compare);

    for (i = 0; i < nvecs; i++)
	write_report_line(ctx, vecs[i]);

    /*
     * Close the main table.
     */
    htprintf(ctx, "</table>\n");

    /*
     * Finish up and tidy up.
     */
    htprintf(ctx, "</body>\n");
    htprintf(ctx, "</html>\n");
    sfree(href);
    sfree(path2);
    sfree(path);
    for (i = 0; i < nvecs; i++) {
	sfree(vecs[i]->name);
	sfree(vecs[i]);
    }
    sfree(vecs);

    return ctx->buf;
}

int html_dump(const void *t, unsigned long index, unsigned long endindex,
	      int maxdepth, const struct html_config *cfg,
	      const char *pathprefix)
{
    /*
     * Determine the filename for this file.
     */
    assert(cfg->format != NULL);
    int prefixlen = strlen(pathprefix);
    int fnmax = strlen(pathprefix) + strlen(cfg->format) + 100;
    char filename[fnmax];
    strcpy(filename, pathprefix);
    make_filename(filename + prefixlen, fnmax - prefixlen,
		  cfg->format, cfg->rootpage, index);

    /*
     * Create the HTML itself. Don't write out downlinks from our
     * deepest level.
     */
    char *html = html_query(t, index, cfg, maxdepth != 0);

    /*
     * Write it out.
     */
    FILE *fp = fopen(filename, "w");
    if (!fp) {
	fprintf(stderr, "%s: %s: open: %s\n", PNAME,
		filename, strerror(errno));
	return 1;
    }
    if (fputs(html, fp) < 0) {
	fprintf(stderr, "%s: %s: write: %s\n", PNAME,
		filename, strerror(errno));
	fclose(fp);
	return 1;
    }
    if (fclose(fp) < 0) {
	fprintf(stderr, "%s: %s: fclose: %s\n", PNAME,
		filename, strerror(errno));
	return 1;
    }

    /*
     * Recurse.
     */
    if (maxdepth != 0) {
	unsigned long subindex, subendindex;
	int newdepth = (maxdepth > 0 ? maxdepth - 1 : maxdepth);
	char path[1+trie_maxpathlen(t)];

	index++;
	while (index < endindex) {
	    trie_getpath(t, index, path);
	    get_indices(t, path, &subindex, &subendindex);
	    index = subendindex;
	    if (subendindex - subindex > 1) {
		if (html_dump(t, subindex, subendindex, newdepth,
			      cfg, pathprefix))
		    return 1;
	    }
	}
    }
    return 0;
}
