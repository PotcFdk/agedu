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
    char *oururi;
    size_t hreflen;
    const char *uriformat;
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
	if (newret > ret)              /* integer overflow - give up! */
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
    bool want_href, essential;
    char *name;
    bool literal;   /* should the name be formatted in fixed-pitch? */
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
				  bool want_href, bool essential,
				  char *name, bool literal)
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
	"%g B", "%g kB", "%#.1f MB", "%#.1f GB", "%#.1f TB",
	"%#.1f PB", "%#.1f EB", "%#.1f ZB", "%#.1f YB"
    };
    int shift = 0;
    unsigned long long tmpsize;
    double denominator;

    tmpsize = size;
    denominator = 1.0;
    while (tmpsize >= 1024 && shift < lenof(fmts)-1) {
	tmpsize >>= 10;
        denominator *= 1024.0;
	shift++;
    }
    *display_size = size / denominator;
    *fmt = fmts[shift];
}

struct format_option {
    const char *prefix, *suffix;       /* may include '%%' */
    int prefixlen, suffixlen;          /* does not count '%%' */
    char fmttype;                      /* 0 for none, or 'n' or 'p' */
    bool translate_pathsep;            /* pathsep rendered as '/'? */
    bool shorten_path;                 /* omit common prefix? */
};

/*
 * Gets the next format option from a format string. Advances '*fmt'
 * past it, or sets it to NULL if nothing is left.
 */
struct format_option get_format_option(const char **fmt)
{
    struct format_option ret;

    /*
     * Scan for prefix of format.
     */
    ret.prefix = *fmt;
    ret.prefixlen = 0;
    while (1) {
        if (**fmt == '\0') {
            /*
             * No formatting directive, and this is the last option.
             */
            ret.suffix = *fmt;
            ret.suffixlen = 0;
            ret.fmttype = '\0';
            *fmt = NULL;
            return ret;
        } else if (**fmt == '%') {
            if ((*fmt)[1] == '%') {
                (*fmt) += 2;           /* just advance one extra */
                ret.prefixlen++;
            } else if ((*fmt)[1] == '|') {
                /*
                 * No formatting directive.
                 */
                ret.suffix = *fmt;
                ret.suffixlen = 0;
                ret.fmttype = '\0';
                (*fmt) += 2;           /* advance to start of next option */
                return ret;
            } else {
                break;
            }
        } else {
            (*fmt)++;                  /* normal character */
            ret.prefixlen++;
        }
    }

    /*
     * Interpret formatting directive with flags.
     */
    (*fmt)++;
    ret.translate_pathsep = true;
    ret.shorten_path = true;
    while (1) {
        char c = *(*fmt)++;
        assert(c);
        if (c == '/') {
            ret.translate_pathsep = false;
        } else if (c == '-') {
            ret.shorten_path = false;
        } else {
            assert(c == 'n' || c == 'p');
            ret.fmttype = c;
            break;
        }
    }

    /*
     * Scan for suffix.
     */
    ret.suffix = *fmt;
    ret.suffixlen = 0;
    while (1) {
        if (**fmt == '\0') {
            /*
             * This is the last option.
             */
            *fmt = NULL;
            return ret;
        } else if (**fmt != '%') {
            (*fmt)++;                  /* normal character */
            ret.suffixlen++;
        } else {
            if ((*fmt)[1] == '%') {
                (*fmt) += 2;           /* just advance one extra */
                ret.suffixlen++;
            } else {
                assert((*fmt)[1] == '|');
                (*fmt) += 2;           /* advance to start of next option */
                return ret;
            }
        }
    }
}

char *format_string_inner(const char *fmt, int nescape,
                          unsigned long index, const void *t)
{
    int maxlen;
    char *ret = NULL, *p = NULL;
    char *path = NULL, *q = NULL;
    char pathsep = trie_pathsep(t);
    int maxpathlen = trie_maxpathlen(t);
    int charindex;

    while (fmt) {
        struct format_option opt = get_format_option(&fmt);
        if (index && !opt.fmttype)
            continue; /* option is only good for the root, which this isn't */

        maxlen = opt.prefixlen + opt.suffixlen + 1;
        switch (opt.fmttype) {
          case 'n':
            maxlen += 40;              /* generous length for an integer */
            break;
          case 'p':
            maxlen += 3*maxpathlen;    /* might have to escape everything */
            break;
        }
        ret = snewn(maxlen, char);
        p = ret;
        while (opt.prefixlen-- > 0) {
            if ((*p++ = *opt.prefix++) == '%')
                opt.prefix++;
        }
        switch (opt.fmttype) {
          case 'n':
            p += sprintf(p, "%lu", index);
            break;
          case 'p':
            path = snewn(1+trie_maxpathlen(t), char);
            if (opt.shorten_path) {
                trie_getpath(t, 0, path);
                q = path + strlen(path);
                trie_getpath(t, index, path);
                if (*q == pathsep)
                    q++;
            } else {
                trie_getpath(t, index, path);
                q = path;
            }
            charindex = 0;
            while (*q) {
                char c = *q++;
                if (c == pathsep && opt.translate_pathsep) {
                    *p++ = '/';
                    charindex = 0;
                } else if (charindex < nescape ||
                           (!isalnum((unsigned char)c) &&
                            ((charindex == 0 && c=='.') ||
                             !strchr("-.@_", c)))) {
                    p += sprintf(p, "=%02X", (unsigned char)c);
                    charindex++;
                } else {
                    *p++ = c;
                    charindex++;
                }
            }
            sfree(path);
            break;
        }
        while (opt.suffixlen-- > 0) {
            if ((*p++ = *opt.suffix++) == '%')
                opt.suffix++;
        }
        *p = '\0';
        assert(p - ret < maxlen);
        return ret;
    }
    assert(!"Getting here implies an incomplete set of formats");
}

int parse_path(const void *t, const char *path,
               const char *fmt, unsigned long *index)
{
    int len = strlen(path);
    int midlen;
    const char *p, *q;
    char *r;
    char pathsep = trie_pathsep(t);

    while (fmt) {
        struct format_option opt = get_format_option(&fmt);

        /*
         * Check prefix and suffix.
         */
        midlen = len - opt.prefixlen - opt.suffixlen;
        if (midlen < 0)
            continue;                  /* prefix and suffix don't even fit */

        p = path;
        while (opt.prefixlen > 0) {
            char c = *opt.prefix++;
            if (c == '%')
                opt.prefix++;
            if (*p != c)
                break;
            p++;
            opt.prefixlen--;
        }
        if (opt.prefixlen > 0)
            continue;                  /* prefix didn't match */

        q = path + len - opt.suffixlen;
        while (opt.suffixlen > 0) {
            char c = *opt.suffix++;
            if (c == '%')
                opt.suffix++;
            if (*q != c)
                break;
            q++;
            opt.suffixlen--;
        }
        if (opt.suffixlen > 0)
            continue;                  /* suffix didn't match */

        /*
         * Check the data in between. p points at it, and it's midlen
         * characters long.
         */
        if (opt.fmttype == '\0') {
            if (midlen == 0) {
                /*
                 * Successful match against a root format.
                 */
                *index = 0;
                return 1;
            }
        } else if (opt.fmttype == 'n') {
            *index = 0;
            while (midlen > 0) {
                if (*p >= '0' && *p <= '9')
                    *index = *index * 10 + (*p - '0');
                else
                    break;
                midlen--;
                p++;
            }
            if (midlen == 0) {
                /*
                 * Successful match against a numeric format.
                 */
                return 1;
            }
        } else {
            assert(opt.fmttype == 'p');

            int maxoutlen = trie_maxpathlen(t) + 1;
            int maxinlen = midlen + 1;
            char triepath[maxinlen+maxoutlen];

            if (opt.shorten_path) {
                trie_getpath(t, 0, triepath);
                r = triepath + strlen(triepath);
                if (r > triepath && r[-1] != pathsep)
                    *r++ = pathsep;
            } else {
                r = triepath;
            }

            while (midlen > 0) {
                if (*p == '/' && opt.translate_pathsep) {
                    *r++ = pathsep;
                    p++;
                    midlen--;
                } else if (*p == '=') {
                    /*
                     * We intentionally do not check whether the
                     * escaped character _should_ have been escaped
                     * according to the rules in html_format_path.
                     *
                     * All clients of this parsing function, after a
                     * successful parse, call html_format_path to find
                     * the canonical URI for the same index and return
                     * an HTTP redirect if the provided URI was not
                     * exactly equal to that canonical form. This is
                     * critical when the correction involves adding or
                     * removing a trailing slash (because then
                     * relative hrefs on the generated page can be
                     * computed with respect to the canonical URI
                     * instead of having to remember what the actual
                     * URI was), but also has the useful effect that
                     * if a user attempts to type in (guess) a URI by
                     * hand they don't have to remember the escaping
                     * rules - as long as they type _something_ that
                     * this code can parse into a recognisable
                     * pathname, it will be automatically 301ed into
                     * the canonical form.
                     */
                    if (midlen < 3 ||
                        !isxdigit((unsigned char)p[1]) ||
                        !isxdigit((unsigned char)p[2]))
                        break;         /* faulty escape encoding */
                    char x[3];
                    unsigned cval;
                    x[0] = p[1];
                    x[1] = p[2];
                    x[2] = '\0';
                    sscanf(x, "%x", &cval);
                    *r++ = cval;
                    p += 3;
                    midlen -= 3;
                } else {
                    *r++ = *p;
                    p++;
                    midlen--;
                }
            }
            if (midlen > 0)
                continue;      /* something went wrong in that loop */
            assert(r - triepath < maxinlen+maxoutlen);
            *r = '\0';

            unsigned long gotidx = trie_before(t, triepath);
            if (gotidx >= trie_count(t))
                continue;              /* index out of range */
            char retpath[1+maxoutlen];
            trie_getpath(t, gotidx, retpath);
            if (strcmp(triepath, retpath))
                continue;           /* exact path not found in trie */
            if (!index_has_root(t, gotidx))
                continue;              /* path is not a directory */

            /*
             * Successful path-based match.
             */
            *index = gotidx;
            return 1;
        }
    }

    return 0;                    /* no match from any format option */
}

char *format_string(const char *fmt, unsigned long index, const void *t)
{
    unsigned long indexout;
    char *ret;
    int nescape = 0;

    /*
     * Format the string using whichever format option first works.
     */
    ret = format_string_inner(fmt, 0, index, t);

    /*
     * Now re-_parse_ the string, to see if it gives the same index
     * back. It might not, if a pathname is valid in two formats: for
     * instance, if you use '-H -d max' to generate a static HTML dump
     * from scanning a directory which has a subdir called 'index',
     * you might well find that the top-level file wants to be called
     * index.html and so does the one for that subdir.
     *
     * We fix this by formatting the string again with more and more
     * characters escaped, so that the non-root 'index.html' becomes
     * (e.g.) '=69ndex.html', or '=69=6edex.html' if that doesn't
     * work, etc.
     */
    while (1) {
        /*
         * Parse the pathname and see if it gives the right index.
         */
        int parseret = parse_path(t, ret, fmt, &indexout);
        assert(parseret != 0);
        if (indexout == index)
            break;                     /* path now parses successfully */

        /*
         * If not, try formatting it again.
         */
        char *new = format_string_inner(fmt, ++nescape, index, t);
        assert(strcmp(new, ret));      /* if nescape gets too big, give up */
        sfree(ret);
        ret = new;
    }

    return ret;
}

char *html_format_path(const void *t, const struct html_config *cfg,
                       unsigned long index)
{
    return format_string(cfg->uriformat, index, t);
}

int html_parse_path(const void *t, const char *path,
                    const struct html_config *cfg, unsigned long *index)
{
    return parse_path(t, path, cfg->uriformat, index);
}

char *make_href(const char *source, const char *target)
{
    /*
     * We insist that both source and target URIs start with a /, or
     * else we won't be reliably able to construct relative hrefs
     * between them (e.g. because we've got a suffix on the end of
     * some CGI pathname that this function doesn't know the final
     * component of).
     */
    assert(*source == '/');
    assert(*target == '/');

    /*
     * Find the last / in source. Everything up to but not including
     * that is the directory to which the output href will be
     * relative. We enforce by assertion that there must be a /
     * somewhere in source, or else we can't construct a relative href
     * at all
     */
    const char *sourceend = strrchr(source, '/');
    assert(sourceend != NULL);

    /*
     * See how far the target URI agrees with the source one, up to
     * and including that /.
     */
    const char *s = source, *t = target;
    while (s <= sourceend && *s == *t)
        s++, t++;

    /*
     * We're only interested in agreement of complete path components,
     * so back off until we're sitting just after a shared /.
     */
    while (s > source && s[-1] != '/')
        s--, t--;
    assert(s > source);

    /*
     * Now we need some number of levels of "../" to get from source
     * to here, and then we just replicate the rest of 'target'.
     */
    int levels = 0;
    while (s <= sourceend) {
        if (*s == '/')
            levels++;
        s++;
    }
    int len = 3*levels + strlen(t);
    if (len == 0) {
        /* One last special case: if target has no tail _and_ we
         * haven't written out any "../". */
        return dupstr("./");
    } else {
        char *ret = snewn(len+1, char);
        char *p = ret;
        while (levels-- > 0) {
            *p++ = '.';
            *p++ = '.';
            *p++ = '/';
        }
        strcpy(p, t);
        return ret;
    }
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
	bool doing_href = false;

	if (ctx->uriformat && vec->want_href) {
	    char *targeturi = format_string(ctx->uriformat, vec->index,
                                            ctx->t);
            char *link = make_href(ctx->oururi, targeturi);
	    htprintf(ctx, "<a href=\"%s\">", link);
            sfree(link);
            sfree(targeturi);
	    doing_href = true;
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
		 const struct html_config *cfg, bool downlink)
{
    struct html actx, *ctx = &actx;
    char *path, *path2, *p, *q;
    char agebuf1[80], agebuf2[80];
    size_t pathlen, subdirpos;
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
    ctx->uriformat = cfg->uriformat;
    htprintf(ctx, "<html>\n");

    path = snewn(1+trie_maxpathlen(t), char);
    ctx->path2 = path2 = snewn(1+trie_maxpathlen(t), char);
    if (cfg->uriformat)
        ctx->oururi = format_string(cfg->uriformat, index, t);
    else
        ctx->oururi = NULL;

    /*
     * HEAD section.
     */
    htprintf(ctx, "<head>\n");
    trie_getpath(t, index, path);
    htprintf(ctx, "<title>");
    htescape(ctx, cfg->html_title, strlen(cfg->html_title), 0);
    htprintf(ctx, ": ");
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
	if (!strcmptrailingpathsep(path, path2) && cfg->uriformat) {
	    char *targeturi = format_string(cfg->uriformat, index2, t);
            char *link = make_href(ctx->oururi, targeturi);
	    htprintf(ctx, "<a href=\"%s\">", link);
            sfree(link);
            sfree(targeturi);
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
	    strcpy(buf, "&gt; ");
	    round_and_format_age(ctx, ctx->thresholds[0], buf+5, 0);
	} else if (i == MAXCOLOUR) {
	    strcpy(buf, "&lt; ");
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
    vecs[0] = make_vector(ctx, path, false, true, NULL, false);
    print_heading(ctx, "Overall");
    write_report_line(ctx, vecs[0]);

    /*
     * Now generate report lines for all its children, and the
     * files contained in it.
     */
    print_heading(ctx, "Subdirectories");

    if (cfg->showfiles) {
        /* Every file directly in this directory is going to end up in
         * its own entry in the loop below, and then it'll be
         * subtracted from vecs[0]. So vecs[0] will end up tracking
         * the size of the _directory inode_ only. */
        vecs[0]->name = dupstr("[directory]");
    } else {
        /* Otherwise, vecs[0] will track everything that wasn't part
         * of a subdirectory, which includes the directory inode but
         * also all the files within it. Use this more general name. */
        vecs[0]->name = dupstr("[files]");
    }
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
	vecs[nvecs] = make_vector(ctx, path2, downlink && (xj2 - xj1 > 1),
                                  false, path2 + subdirpos, 1);
	for (i = 0; i <= MAXCOLOUR; i++)
	    vecs[0]->sizes[i] -= vecs[nvecs]->sizes[i];
	nvecs++;
    }

    qsort(vecs, nvecs, sizeof(vecs[0]), vec_compare);

    for (i = 0; i < nvecs; i++)
        if (vecs[i]->sizes[MAXCOLOUR])
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
    sfree(ctx->oururi);
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
    assert(cfg->fileformat != NULL);
    char *filename = format_string(cfg->fileformat, index, t);
    char *path = dupfmt("%s%s", pathprefix, filename);
    sfree(filename);

    /*
     * Create the HTML itself. Don't write out downlinks from our
     * deepest level.
     */
    char *html = html_query(t, index, cfg, maxdepth != 0);

    /*
     * Write it out.
     */
    FILE *fp = fopen(path, "w");
    if (!fp) {
	fprintf(stderr, "%s: %s: open: %s\n", PNAME, path, strerror(errno));
	return 1;
    }
    if (fputs(html, fp) < 0) {
	fprintf(stderr, "%s: %s: write: %s\n", PNAME, path, strerror(errno));
	fclose(fp);
	return 1;
    }
    if (fclose(fp) < 0) {
	fprintf(stderr, "%s: %s: fclose: %s\n", PNAME, path, strerror(errno));
	return 1;
    }
    sfree(path);

    /*
     * Recurse.
     */
    if (maxdepth != 0) {
	unsigned long subindex, subendindex;
	int newdepth = (maxdepth > 0 ? maxdepth - 1 : maxdepth);
	char rpath[1+trie_maxpathlen(t)];

	index++;
	while (index < endindex) {
	    trie_getpath(t, index, rpath);
	    get_indices(t, rpath, &subindex, &subendindex);
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
