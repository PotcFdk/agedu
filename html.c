/*
 * html.c: implementation of html.h.
 */

#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <time.h>

#include "html.h"
#include "malloc.h"
#include "trie.h"
#include "index.h"

#define lenof(x) ( sizeof((x)) / sizeof(*(x)) )

#define MAXCOLOUR 511

extern char pathsep;

struct html {
    char *buf;
    size_t buflen, bufsize;
    const void *t;
    unsigned long long totalsize, oldest, newest;
    char *path2;
    char *href;
    size_t hreflen;
    const char *format;
    unsigned long long thresholds[MAXCOLOUR-1];
    time_t now;
};

static void vhtprintf(struct html *ctx, char *fmt, va_list ap)
{
    va_list ap2;
    int size, size2;

    va_copy(ap2, ap);
    size = vsnprintf(NULL, 0, fmt, ap2);
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

static void htprintf(struct html *ctx, char *fmt, ...)
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

    *xi1 = trie_before(t, path);
    path[pathlen] = '\001';
    path[pathlen+1] = '\0';
    *xi2 = trie_before(t, path);
    path[pathlen] = '\0';
}

static unsigned long long fetch_size(const void *t, char *path,
				     unsigned long long atime)
{
    unsigned long xi1, xi2;

    get_indices(t, path, &xi1, &xi2);

    return index_query(t, xi2, atime) - index_query(t, xi1, atime);
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
    char buf[80];

    if (colour >= 0 && colour < 256)   /* red -> yellow fade */
	r = 255, g = colour, b = 0;
    else if (colour >= 256 && colour <= 511)   /* yellow -> green fade */
	r = 511 - colour, g = 255, b = 0;
    else			       /* background grey */
	r = g = b = 240;

    if (colour < 0) {
	/* no title text here */
    } else if (colour == 0) {
	strcpy(buf, "&lt; ");
	round_and_format_age(ctx, ctx->thresholds[0], buf+5, 0);
    } else if (colour == MAXCOLOUR) {
	strcpy(buf, "&gt; ");
	round_and_format_age(ctx, ctx->thresholds[MAXCOLOUR-1], buf+5, 0);
    } else {
	unsigned long long midrange =
	    (ctx->thresholds[colour] + ctx->thresholds[colour+1]) / 2;
	round_and_format_age(ctx, midrange, buf, 0);
    }

    if (pixels > 0) {
	htprintf(ctx, "<td style=\"width:%dpx; height:1em; "
		 "background-color:#%02x%02x%02x\"",
		 pixels, r, g, b);
	if (colour >= 0)
	    htprintf(ctx, " title=\"%s\"", buf);
	htprintf(ctx, "></td>\n");
    }
}

static void end_colour_bar(struct html *ctx)
{
    htprintf(ctx, "</tr>\n</table>\n");
}

struct vector {
    int want_href;
    char *name;
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
    return 0;
}

static struct vector *make_vector(struct html *ctx, char *path,
				  int want_href, char *name)
{
    unsigned long xi1, xi2;
    struct vector *vec = snew(struct vector);
    int i;

    vec->want_href = want_href;
    vec->name = name ? dupstr(name) : NULL;

    get_indices(ctx->t, path, &xi1, &xi2);

    vec->index = xi1;

    for (i = 0; i <= MAXCOLOUR; i++) {
	unsigned long long atime;
	if (i == MAXCOLOUR)
	    atime = ULLONG_MAX;
	else
	    atime = ctx->thresholds[i];
	vec->sizes[i] = fetch_size(ctx->t, path, atime);
    }

    return vec;
}

static void print_heading(struct html *ctx, const char *title)
{
    htprintf(ctx, "<tr style=\"padding: 0.2em; background-color:#e0e0e0\">\n"
	     "<td colspan=4 align=center>%s</td>\n</tr>\n", title);
}

#define PIXEL_SIZE 600		       /* FIXME: configurability? */
static void write_report_line(struct html *ctx, struct vector *vec)
{
    unsigned long long size, asize, divisor;
    int pix, newpix;
    int i;

    /*
     * A line with literally zero space usage should not be
     * printed at all if it's a link to a subdirectory (since it
     * probably means the whole thing was excluded by some
     * --exclude-path wildcard). If it's [files] or the top-level
     * line, though, we must always print _something_, and in that
     * case we must fiddle about to prevent divisions by zero in
     * the code below.
     */
    if (!vec->sizes[MAXCOLOUR] && vec->want_href)
	return;
    divisor = ctx->totalsize;
    if (!divisor) {
	divisor = 1;
    }

    /*
     * Find the total size of this subdirectory.
     */
    size = vec->sizes[MAXCOLOUR];
    htprintf(ctx, "<tr>\n"
	     "<td style=\"padding: 0.2em; text-align: right\">%lluMb</td>\n",
	     ((size + ((1<<20)-1)) >> 20)); /* convert to Mb, rounding up */

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
	    snprintf(ctx->href, ctx->hreflen, ctx->format, vec->index);
	    htprintf(ctx, "<a href=\"%s\">", ctx->href);
	    doing_href = 1;
	}
	htescape(ctx, vec->name, strlen(vec->name), 1);
	if (doing_href)
	    htprintf(ctx, "</a>");
    }
    htprintf(ctx, "</td>\n</tr>\n");
}

char *html_query(const void *t, unsigned long index,
		 const struct html_config *cfg)
{
    struct html actx, *ctx = &actx;
    char *path, *path2, *p, *q, *href;
    char agebuf1[80], agebuf2[80];
    size_t pathlen, hreflen;
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
    htprintf(ctx, "<title>agedu: ");
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
    for (p = strchr(path, pathsep); p; p = strchr(p+1, pathsep)) {
	int doing_href = 0;
	/*
	 * See if this path prefix exists in the trie. If so,
	 * generate a hyperlink.
	 */
	*p = '\0';
	index2 = trie_before(t, path);
	trie_getpath(t, index2, path2);
	if (!strcmp(path, path2) && cfg->format) {
	    snprintf(href, hreflen, cfg->format, index2);
	    htprintf(ctx, "<a href=\"%s\">", href);
	    doing_href = 1;
	}
	*p = pathsep;
	htescape(ctx, q, p - q, 1);
	q = p + 1;
	if (doing_href)
	    htprintf(ctx, "</a>");
	htescape(ctx, p, 1, 1);
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
    for (i = 0; i < MAXCOLOUR-1; i++) {
	ctx->thresholds[i] =
	    ctx->oldest + (ctx->newest - ctx->oldest) * i / MAXCOLOUR;
    }
    htprintf(ctx, "<p align=center>Key to colour coding (mouse over for more detail):\n");
    htprintf(ctx, "<p align=center style=\"padding: 0; margin-top:0.4em; "
	     "margin-bottom:1em\"");
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
    ctx->totalsize = fetch_size(t, path, ULLONG_MAX);

    /*
     * Generate a report line for the whole subdirectory.
     */
    vecsize = 64;
    vecs = snewn(vecsize, struct vector *);
    nvecs = 1;
    vecs[0] = make_vector(ctx, path, 0, NULL);
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
    while (xi1 < xi2) {
	trie_getpath(t, xi1, path2);
	get_indices(t, ctx->path2, &xj1, &xj2);
	xi1 = xj2;
	if (xj2 - xj1 <= 1)
	    continue;		       /* skip individual files */
	if (nvecs >= vecsize) {
	    vecsize = nvecs * 3 / 2 + 64;
	    vecs = sresize(vecs, vecsize, struct vector *);
	}
	assert(strlen(path2) > pathlen);
	vecs[nvecs] = make_vector(ctx, path2, 1, path2 + pathlen + 1);
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
