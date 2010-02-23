/*
 * Main program for agedu.
 */

#include "agedu.h"

#include "du.h"
#include "trie.h"
#include "index.h"
#include "alloc.h"
#include "html.h"
#include "httpd.h"
#include "fgetline.h"

/*
 * Path separator. This global variable affects the behaviour of
 * various parts of the code when they need to deal with path
 * separators. The path separator appropriate to a particular data
 * set is encoded in the index file storing that data set; data
 * sets generated on Unix will of course have the default '/', but
 * foreign data sets are conceivable and must be handled correctly.
 */
char pathsep = '/';

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
    int type;
    const char *wildcard;
    int path;
};

struct ctx {
    triebuild *tb;
    dev_t datafile_dev, filesystem_dev;
    ino_t datafile_ino;
    time_t last_output_update;
    int progress, progwidth;
    int straight_to_dump;
    struct inclusion_exclusion *inex;
    int ninex;
    int crossfs;
    int usemtime;
    int fakeatimes;
};

static void dump_line(const char *pathname, const struct trie_file *tf)
{
    const char *p;
    if (printf("%llu %llu ", tf->size, tf->atime) < 0) goto error;
    for (p = pathname; *p; p++) {
	if (*p >= ' ' && *p < 127 && *p != '%') {
	    if (putchar(*p) == EOF) goto error;
	} else {
	    if (printf("%%%02x", (unsigned char)*p) < 0) goto error;
	}
    }
    if (putchar('\n') == EOF) goto error;
    return;
    error:
    fatal("standard output: %s", strerror(errno));
}

static int gotdata(void *vctx, const char *pathname, const STRUCT_STAT *st)
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

    file.size = (unsigned long long)512 * st->st_blocks;
    if (ctx->usemtime || (ctx->fakeatimes && S_ISDIR(st->st_mode)))
	file.atime = st->st_mtime;
    else
	file.atime = max(st->st_mtime, st->st_atime);

    /*
     * Filter based on wildcards.
     */
    include = 1;
    filename = strrchr(pathname, pathsep);
    if (!filename)
	filename = pathname;
    else
	filename++;
    for (i = 0; i < ctx->ninex; i++) {
	if (fnmatch(ctx->inex[i].wildcard,
		    ctx->inex[i].path ? pathname : filename, 0) == 0)
	    include = ctx->inex[i].type;
    }
    if (include == -1)
	return 0;		       /* ignore this entry and any subdirs */
    if (include == 0) {
	/*
	 * Here we are supposed to be filtering an entry out, but
	 * still recursing into it if it's a directory. However,
	 * we can't actually leave out any directory whose
	 * subdirectories we then look at. So we cheat, in that
	 * case, by setting the size to zero.
	 */
	if (!S_ISDIR(st->st_mode))
	    return 0;		       /* just ignore */
	else
	    file.size = 0;
    }

    if (ctx->straight_to_dump)
	dump_line(pathname, &file);
    else
	triebuild_add(ctx->tb, pathname, &file);

    if (ctx->progress) {
	t = time(NULL);
	if (t != ctx->last_output_update) {
	    fprintf(stderr, "%-*.*s\r", ctx->progwidth, ctx->progwidth,
		    pathname);
	    fflush(stderr);
	    ctx->last_output_update = t;
	}
    }

    return 1;
}

static void scan_error(void *vctx, const char *fmt, ...)
{
    struct ctx *ctx = (struct ctx *)vctx;
    va_list ap;

    if (ctx->progress) {
	fprintf(stderr, "%-*s\r", ctx->progwidth, "");
	fflush(stderr);
    }

    fprintf(stderr, "%s: ", PNAME);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    ctx->last_output_update--;	       /* force a progress report next time */
}

static void text_query(const void *mappedfile, const char *querydir,
		       time_t t, int showfiles, int depth, FILE *fp)
{
    size_t maxpathlen;
    char *pathbuf;
    unsigned long xi1, xi2;
    unsigned long long size;

    maxpathlen = trie_maxpathlen(mappedfile);
    pathbuf = snewn(maxpathlen + 1, char);

    /*
     * We want to query everything between the supplied filename
     * (inclusive) and that filename with a ^A on the end
     * (exclusive). So find the x indices for each.
     */
    strcpy(pathbuf, querydir);
    make_successor(pathbuf);
    xi1 = trie_before(mappedfile, querydir);
    xi2 = trie_before(mappedfile, pathbuf);

    if (!showfiles && xi2 - xi1 == 1)
	return;			       /* file, or empty dir => no display */

    /*
     * Now do the lookups in the age index.
     */
    if (xi2 - xi1 == 1) {
	/*
	 * We are querying an individual file, so we should not
	 * depend on the index entries either side of the node,
	 * since they almost certainly don't both exist. Instead,
	 * just look up the file's size and atime in the main trie.
	 */
	const struct trie_file *f = trie_getfile(mappedfile, xi1);
	if (f->atime < t)
	    size = f->size;
	else
	    size = 0;
    } else {
	unsigned long long s1, s2;
	s1 = index_query(mappedfile, xi1, t);
	s2 = index_query(mappedfile, xi2, t);
	size = s2 - s1;
    }

    if (size == 0)
	return;			       /* no space taken up => no display */

    if (depth != 0) {
	/*
	 * Now scan for first-level subdirectories and report
	 * those too.
	 */
	int newdepth = (depth > 0 ? depth - 1 : depth);
	xi1++;
	while (xi1 < xi2) {
	    trie_getpath(mappedfile, xi1, pathbuf);
	    text_query(mappedfile, pathbuf, t, showfiles, newdepth, fp);
	    make_successor(pathbuf);
	    xi1 = trie_before(mappedfile, pathbuf);
	}
    }

    /* Display in units of 1Kb */
    fprintf(fp, "%-11llu %s\n", (size) / 1024, querydir);
}

/*
 * Largely frivolous way to define all my command-line options. I
 * present here a parametric macro which declares a series of
 * _logical_ option identifiers, and for each one declares zero or
 * more short option characters and zero or more long option
 * words. Then I repeatedly invoke that macro with its arguments
 * defined to be various other macros, which allows me to
 * variously:
 * 
 *  - define an enum allocating a distinct integer value to each
 *    logical option id
 *  - define a string consisting of precisely all the short option
 *    characters
 *  - define a string array consisting of all the long option
 *    strings
 *  - define (with help from auxiliary enums) integer arrays
 *    parallel to both of the above giving the logical option id
 *    for each physical short and long option
 *  - define an array indexed by logical option id indicating
 *    whether the option in question takes a value
 *  - define a function which prints out brief online help for all
 *    the options.
 *
 * It's not at all clear to me that this trickery is actually
 * particularly _efficient_ - it still, after all, requires going
 * linearly through the option list at run time and doing a
 * strcmp, whereas in an ideal world I'd have liked the lists of
 * long and short options to be pre-sorted so that a binary search
 * or some other more efficient lookup was possible. (Not that
 * asymptotic algorithmic complexity is remotely vital in option
 * parsing, but if I were doing this in, say, Lisp or something
 * with an equivalently powerful preprocessor then once I'd had
 * the idea of preparing the option-parsing data structures at
 * compile time I would probably have made the effort to prepare
 * them _properly_. I could have Perl generate me a source file
 * from some sort of description, I suppose, but that would seem
 * like overkill. And in any case, it's more of a challenge to
 * achieve as much as possible by cunning use of cpp and enum than
 * to just write some sensible and logical code in a Turing-
 * complete language. I said it was largely frivolous :-)
 *
 * This approach does have the virtue that it brings together the
 * option ids, option spellings and help text into a single
 * combined list and defines them all in exactly one place. If I
 * want to add a new option, or a new spelling for an option, I
 * only have to modify the main OPTHELP macro below and then add
 * code to process the new logical id.
 *
 * (Though, really, even that isn't ideal, since it still involves
 * modifying the source file in more than one place. In a
 * _properly_ ideal world, I'd be able to interleave the option
 * definitions with the code fragments that process them. And then
 * not bother defining logical identifiers for them at all - those
 * would be automatically generated, since I wouldn't have any
 * need to specify them manually in another part of the code.)
 *
 * One other helpful consequence of the enum-based structure here
 * is that it causes a compiler error if I accidentally try to
 * define the same option (short or long) twice.
 */

#define OPTHELP(NOVAL, VAL, SHORT, LONG, HELPPFX, HELPARG, HELPLINE, HELPOPT) \
    HELPPFX("usage") HELPLINE(PNAME " [options] action [action...]") \
    HELPPFX("actions") \
    VAL(SCAN) SHORT(s) LONG(scan) \
	HELPARG("directory") HELPOPT("scan and index a directory") \
    NOVAL(HTTPD) SHORT(w) LONG(web) LONG(server) LONG(httpd) \
        HELPOPT("serve HTML reports from a temporary web server") \
    VAL(TEXT) SHORT(t) LONG(text) \
	HELPARG("subdir") HELPOPT("print a plain text report on a subdirectory") \
    NOVAL(REMOVE) SHORT(R) LONG(remove) LONG(delete) LONG(unlink) \
        HELPOPT("remove the index file") \
    NOVAL(DUMP) SHORT(D) LONG(dump) HELPOPT("dump the index file on stdout") \
    NOVAL(LOAD) SHORT(L) LONG(load) \
	HELPOPT("load and index a dump file") \
    VAL(SCANDUMP) SHORT(S) LONG(scan_dump) \
	HELPARG("directory") HELPOPT("scan only, generating a dump") \
    VAL(HTML) SHORT(H) LONG(html) \
	HELPARG("subdir") HELPOPT("print an HTML report on a subdirectory") \
    NOVAL(CGI) LONG(cgi) \
        HELPOPT("do the right thing when run from a CGI script") \
    HELPPFX("options") \
    VAL(DATAFILE) SHORT(f) LONG(file) \
        HELPARG("filename") HELPOPT("[most modes] specify index file") \
    NOVAL(CROSSFS) LONG(cross_fs) \
        HELPOPT("[--scan] cross filesystem boundaries") \
    NOVAL(NOCROSSFS) LONG(no_cross_fs) \
        HELPOPT("[--scan] stick to one filesystem") \
    VAL(PRUNE) LONG(prune) \
        HELPARG("wildcard") HELPOPT("[--scan] prune files matching pattern") \
    VAL(PRUNEPATH) LONG(prune_path) \
        HELPARG("wildcard") HELPOPT("[--scan] prune pathnames matching pattern") \
    VAL(EXCLUDE) LONG(exclude) \
        HELPARG("wildcard") HELPOPT("[--scan] exclude files matching pattern") \
    VAL(EXCLUDEPATH) LONG(exclude_path) \
        HELPARG("wildcard") HELPOPT("[--scan] exclude pathnames matching pattern") \
    VAL(INCLUDE) LONG(include) \
        HELPARG("wildcard") HELPOPT("[--scan] include files matching pattern") \
    VAL(INCLUDEPATH) LONG(include_path) \
        HELPARG("wildcard") HELPOPT("[--scan] include pathnames matching pattern") \
    NOVAL(PROGRESS) LONG(progress) LONG(scan_progress) \
        HELPOPT("[--scan] report progress on stderr") \
    NOVAL(NOPROGRESS) LONG(no_progress) LONG(no_scan_progress) \
        HELPOPT("[--scan] do not report progress") \
    NOVAL(TTYPROGRESS) LONG(tty_progress) LONG(tty_scan_progress) \
		       LONG(progress_tty) LONG(scan_progress_tty) \
        HELPOPT("[--scan] report progress if stderr is a tty") \
    NOVAL(DIRATIME) LONG(dir_atime) LONG(dir_atimes) \
        HELPOPT("[--scan,--load] keep real atimes on directories") \
    NOVAL(NODIRATIME) LONG(no_dir_atime) LONG(no_dir_atimes) \
        HELPOPT("[--scan,--load] fake atimes on directories") \
    NOVAL(MTIME) LONG(mtime) \
        HELPOPT("[--scan] use mtime instead of atime") \
    NOVAL(SHOWFILES) LONG(files) \
        HELPOPT("[--web,--html,--text] list individual files") \
    VAL(AGERANGE) SHORT(r) LONG(age_range) LONG(range) LONG(ages) \
        HELPARG("age[-age]") HELPOPT("[--web,--html] set limits of colour coding") \
    VAL(OUTFILE) SHORT(o) LONG(output) \
	HELPARG("filename") HELPOPT("[--html] specify output file or directory name") \
    VAL(SERVERADDR) LONG(address) LONG(addr) LONG(server_address) \
              LONG(server_addr) \
        HELPARG("addr[:port]") HELPOPT("[--web] specify HTTP server address") \
    VAL(AUTH) LONG(auth) LONG(http_auth) LONG(httpd_auth) \
              LONG(server_auth) LONG(web_auth) \
        HELPARG("type") HELPOPT("[--web] specify HTTP authentication method") \
    VAL(AUTHFILE) LONG(auth_file) \
        HELPARG("filename") HELPOPT("[--web] read HTTP Basic user/pass from file") \
    VAL(AUTHFD) LONG(auth_fd) \
        HELPARG("fd") HELPOPT("[--web] read HTTP Basic user/pass from fd") \
    VAL(DEPTH) SHORT(d) LONG(depth) LONG(max_depth) LONG(maximum_depth) \
        HELPARG("levels") HELPOPT("[--text,--html] recurse to this many levels") \
    VAL(MINAGE) SHORT(a) LONG(age) LONG(min_age) LONG(minimum_age) \
        HELPARG("age") HELPOPT("[--text] include only files older than this") \
    HELPPFX("also") \
    NOVAL(HELP) SHORT(h) LONG(help) HELPOPT("display this help text") \
    NOVAL(VERSION) SHORT(V) LONG(version) HELPOPT("report version number") \
    NOVAL(LICENCE) LONG(licence) LONG(license) \
        HELPOPT("display (MIT) licence text") \

#define IGNORE(x)
#define DEFENUM(x) OPT_ ## x,
#define ZERO(x) 0,
#define ONE(x) 1,
#define STRING(x) #x ,
#define STRINGNOCOMMA(x) #x
#define SHORTNEWOPT(x) SHORTtmp_ ## x = OPT_ ## x,
#define SHORTTHISOPT(x) SHORTtmp2_ ## x, SHORTVAL_ ## x = SHORTtmp2_ ## x - 1,
#define SHORTOPTVAL(x) SHORTVAL_ ## x,
#define SHORTTMP(x) SHORTtmp3_ ## x,
#define LONGNEWOPT(x) LONGtmp_ ## x = OPT_ ## x,
#define LONGTHISOPT(x) LONGtmp2_ ## x, LONGVAL_ ## x = LONGtmp2_ ## x - 1,
#define LONGOPTVAL(x) LONGVAL_ ## x,
#define LONGTMP(x) SHORTtmp3_ ## x,

#define OPTIONS(NOVAL, VAL, SHORT, LONG) \
    OPTHELP(NOVAL, VAL, SHORT, LONG, IGNORE, IGNORE, IGNORE, IGNORE)

enum { OPTIONS(DEFENUM,DEFENUM,IGNORE,IGNORE) NOPTIONS };
enum { OPTIONS(IGNORE,IGNORE,SHORTTMP,IGNORE) NSHORTOPTS };
enum { OPTIONS(IGNORE,IGNORE,IGNORE,LONGTMP) NLONGOPTS };
static const int opthasval[NOPTIONS] = {OPTIONS(ZERO,ONE,IGNORE,IGNORE)};
static const char shortopts[] = {OPTIONS(IGNORE,IGNORE,STRINGNOCOMMA,IGNORE)};
static const char *const longopts[] = {OPTIONS(IGNORE,IGNORE,IGNORE,STRING)};
enum { OPTIONS(SHORTNEWOPT,SHORTNEWOPT,SHORTTHISOPT,IGNORE) };
enum { OPTIONS(LONGNEWOPT,LONGNEWOPT,IGNORE,LONGTHISOPT) };
static const int shortvals[] = {OPTIONS(IGNORE,IGNORE,SHORTOPTVAL,IGNORE)};
static const int longvals[] = {OPTIONS(IGNORE,IGNORE,IGNORE,LONGOPTVAL)};

static void usage(FILE *fp)
{
    char longbuf[80];
    const char *prefix, *shortopt, *longopt, *optarg;
    int i, optex;

#define HELPRESET prefix = shortopt = longopt = optarg = NULL, optex = -1
#define HELPNOVAL(s) optex = 0;
#define HELPVAL(s) optex = 1;
#define HELPSHORT(s) if (!shortopt) shortopt = "-" #s;
#define HELPLONG(s) if (!longopt) { \
    strcpy(longbuf, "--" #s); longopt = longbuf; \
    for (i = 0; longbuf[i]; i++) if (longbuf[i] == '_') longbuf[i] = '-'; }
#define HELPPFX(s) prefix = s;
#define HELPARG(s) optarg = s;
#define HELPLINE(s) assert(optex == -1); \
    fprintf(fp, "%7s%c %s\n", prefix?prefix:"", prefix?':':' ', s); \
    HELPRESET;
#define HELPOPT(s) assert((optex == 1 && optarg) || (optex == 0 && !optarg)); \
    assert(shortopt || longopt); \
    i = fprintf(fp, "%7s%c %s%s%s%s%s", prefix?prefix:"", prefix?':':' ', \
        shortopt?shortopt:"", shortopt&&longopt?", ":"", longopt?longopt:"", \
	optarg?" ":"", optarg?optarg:""); \
    fprintf(fp, "%*s %s\n", i<32?32-i:0,"",s); HELPRESET;

    HELPRESET;
    OPTHELP(HELPNOVAL, HELPVAL, HELPSHORT, HELPLONG,
	    HELPPFX, HELPARG, HELPLINE, HELPOPT);

#undef HELPRESET
#undef HELPNOVAL
#undef HELPVAL
#undef HELPSHORT
#undef HELPLONG
#undef HELPPFX
#undef HELPARG
#undef HELPLINE
#undef HELPOPT
}

static time_t parse_age(time_t now, const char *agestr)
{
    time_t t;
    struct tm tm;
    int nunits;
    char unit[2];

    t = now;

    if (2 != sscanf(agestr, "%d%1[DdWwMmYy]", &nunits, unit)) {
	fprintf(stderr, "%s: age specification should be a number followed by"
		" one of d,w,m,y\n", PNAME);
	exit(1);
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

    return t;
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
    const struct trie_file *tf, *prevtf;
    char *filename = PNAME ".dat";
    int doing_opts = 1;
    enum { TEXT, HTML, SCAN, DUMP, SCANDUMP, LOAD, HTTPD, REMOVE };
    struct action {
	int mode;
	char *arg;
    } *actions = NULL;
    int nactions = 0, actionsize = 0, action;
    time_t now = time(NULL);
    time_t textcutoff = now, htmlnewest = now, htmloldest = now;
    int htmlautoagerange = 1;
    const char *httpserveraddr = NULL;
    int httpserverport = 0;
    const char *httpauthdata = NULL;
    const char *outfile = NULL;
    int auth = HTTPD_AUTH_MAGIC | HTTPD_AUTH_BASIC;
    int progress = 1;
    struct inclusion_exclusion *inex = NULL;
    int ninex = 0, inexsize = 0;
    int crossfs = 0;
    int depth = -1, gotdepth = 0;
    int fakediratimes = 1;
    int mtime = 0;
    int showfiles = 0;

#ifdef DEBUG_MAD_OPTION_PARSING_MACROS
    {
	static const char *const optnames[NOPTIONS] = {
	    OPTIONS(STRING,STRING,IGNORE,IGNORE)
	};
	int i;
	for (i = 0; i < NSHORTOPTS; i++)
	    printf("-%c == %s [%s]\n", shortopts[i], optnames[shortvals[i]],
		   opthasval[shortvals[i]] ? "value" : "no value");
	for (i = 0; i < NLONGOPTS; i++)
	    printf("--%s == %s [%s]\n", longopts[i], optnames[longvals[i]],
		   opthasval[longvals[i]] ? "value" : "no value");
    }
#endif

    while (--argc > 0) {
        char *p = *++argv;

        if (doing_opts && *p == '-') {
	    int wordstart = 1;

            if (!strcmp(p, "--")) {
                doing_opts = 0;
		continue;
            }

	    p++;
	    while (*p) {
		int optid = -1;
		int i;
		char *optval;

		if (wordstart && *p == '-') {
		    /*
		     * GNU-style long option.
		     */
		    p++;
		    optval = strchr(p, '=');
		    if (optval)
			*optval++ = '\0';

		    for (i = 0; i < NLONGOPTS; i++) {
			const char *opt = longopts[i], *s = p;
			int match = 1;
			/*
			 * The underscores in the option names
			 * defined above may be given by the user
			 * as underscores or dashes, or omitted
			 * entirely.
			 */
			while (*opt) {
			    if (*opt == '_') {
				if (*s == '-' || *s == '_')
				    s++;
			    } else {
				if (*opt != *s) {
				    match = 0;
				    break;
				}
				s++;
			    }
			    opt++;
			}
			if (match && !*s) {
			    optid = longvals[i];
			    break;
			}
		    }

		    if (optid < 0) {
			fprintf(stderr, "%s: unrecognised option '--%s'\n",
				PNAME, p);
			return 1;
		    }

		    if (!opthasval[optid]) {
			if (optval) {
			    fprintf(stderr, "%s: unexpected argument to option"
				    " '--%s'\n", PNAME, p);
			    return 1;
			}
		    } else {
			if (!optval) {
			    if (--argc > 0) {
				optval = *++argv;
			    } else {
				fprintf(stderr, "%s: option '--%s' expects"
					" an argument\n", PNAME, p);
				return 1;
			    }
			}
		    }

		    p += strlen(p);    /* finished with this argument word */
		} else {
		    /*
		     * Short option.
		     */
                    char c = *p++;

		    for (i = 0; i < NSHORTOPTS; i++)
			if (c == shortopts[i]) {
			    optid = shortvals[i];
			    break;
			}

		    if (optid < 0) {
			fprintf(stderr, "%s: unrecognised option '-%c'\n",
				PNAME, c);
			return 1;
		    }

		    if (opthasval[optid]) {
                        if (*p) {
                            optval = p;
                            p += strlen(p);
                        } else if (--argc > 0) {
                            optval = *++argv;
                        } else {
                            fprintf(stderr, "%s: option '-%c' expects"
                                    " an argument\n", PNAME, c);
                            return 1;
                        }
		    } else {
			optval = NULL;
		    }
		}

		wordstart = 0;

		/*
		 * Now actually process the option.
		 */
		switch (optid) {
		  case OPT_HELP:
		    usage(stdout);
		    return 0;
		  case OPT_VERSION:
#ifdef PACKAGE_VERSION
		    printf("%s, revision %s\n", PNAME, PACKAGE_VERSION);
#else
		    printf("%s: version number not available when not built"
			   " via automake\n", PNAME);
#endif
		    return 0;
		  case OPT_LICENCE:
		    {
			extern const char *const licence[];
			int i;

			for (i = 0; licence[i]; i++)
			    fputs(licence[i], stdout);

			return 0;
		    }
		    return 0;
		  case OPT_SCAN:
		    if (nactions >= actionsize) {
			actionsize = nactions * 3 / 2 + 16;
			actions = sresize(actions, actionsize, struct action);
		    }
		    actions[nactions].mode = SCAN;
		    actions[nactions].arg = optval;
		    nactions++;
		    break;
		  case OPT_SCANDUMP:
		    if (nactions >= actionsize) {
			actionsize = nactions * 3 / 2 + 16;
			actions = sresize(actions, actionsize, struct action);
		    }
		    actions[nactions].mode = SCANDUMP;
		    actions[nactions].arg = optval;
		    nactions++;
		    break;
		  case OPT_DUMP:
		    if (nactions >= actionsize) {
			actionsize = nactions * 3 / 2 + 16;
			actions = sresize(actions, actionsize, struct action);
		    }
		    actions[nactions].mode = DUMP;
		    actions[nactions].arg = NULL;
		    nactions++;
		    break;
		  case OPT_LOAD:
		    if (nactions >= actionsize) {
			actionsize = nactions * 3 / 2 + 16;
			actions = sresize(actions, actionsize, struct action);
		    }
		    actions[nactions].mode = LOAD;
		    actions[nactions].arg = NULL;
		    nactions++;
		    break;
		  case OPT_TEXT:
		    if (nactions >= actionsize) {
			actionsize = nactions * 3 / 2 + 16;
			actions = sresize(actions, actionsize, struct action);
		    }
		    actions[nactions].mode = TEXT;
		    actions[nactions].arg = optval;
		    nactions++;
		    break;
		  case OPT_HTML:
		  case OPT_CGI:
		    if (nactions >= actionsize) {
			actionsize = nactions * 3 / 2 + 16;
			actions = sresize(actions, actionsize, struct action);
		    }
		    actions[nactions].mode = HTML;
		    actions[nactions].arg = (optid == OPT_HTML ? optval :
					     NULL);
		    nactions++;
		    break;
		  case OPT_HTTPD:
		    if (nactions >= actionsize) {
			actionsize = nactions * 3 / 2 + 16;
			actions = sresize(actions, actionsize, struct action);
		    }
		    actions[nactions].mode = HTTPD;
		    actions[nactions].arg = NULL;
		    nactions++;
		    break;
		  case OPT_REMOVE:
		    if (nactions >= actionsize) {
			actionsize = nactions * 3 / 2 + 16;
			actions = sresize(actions, actionsize, struct action);
		    }
		    actions[nactions].mode = REMOVE;
		    actions[nactions].arg = NULL;
		    nactions++;
		    break;
		  case OPT_PROGRESS:
		    progress = 2;
		    break;
		  case OPT_NOPROGRESS:
		    progress = 0;
		    break;
		  case OPT_TTYPROGRESS:
		    progress = 1;
		    break;
		  case OPT_CROSSFS:
		    crossfs = 1;
		    break;
		  case OPT_NOCROSSFS:
		    crossfs = 0;
		    break;
		  case OPT_DIRATIME:
		    fakediratimes = 0;
		    break;
		  case OPT_NODIRATIME:
		    fakediratimes = 1;
		    break;
		  case OPT_SHOWFILES:
		    showfiles = 1;
		    break;
		  case OPT_MTIME:
		    mtime = 1;
		    break;
		  case OPT_DATAFILE:
		    filename = optval;
		    break;
		  case OPT_DEPTH:
		    if (!strcasecmp(optval, "unlimited") ||
			!strcasecmp(optval, "infinity") ||
			!strcasecmp(optval, "infinite") ||
			!strcasecmp(optval, "inf") ||
			!strcasecmp(optval, "maximum") ||
			!strcasecmp(optval, "max"))
			depth = -1;
		    else
			depth = atoi(optval);
		    gotdepth = 1;
		    break;
		  case OPT_OUTFILE:
		    outfile = optval;
		    break;
		  case OPT_MINAGE:
		    textcutoff = parse_age(now, optval);
		    break;
		  case OPT_AGERANGE:
		    if (!strcmp(optval, "auto")) {
			htmlautoagerange = 1;
		    } else {
			char *q = optval + strcspn(optval, "-:");
			if (*q)
			    *q++ = '\0';
			htmloldest = parse_age(now, optval);
			htmlnewest = *q ? parse_age(now, q) : now;
			htmlautoagerange = 0;
		    }
		    break;
		  case OPT_SERVERADDR:
		    {
			char *port;
			if (optval[0] == '[' &&
			    (port = strchr(optval, ']')) != NULL)
			    port++;
			else
			    port = optval;
			port += strcspn(port, ":");
			if (port)
			    *port++ = '\0';
			httpserveraddr = optval;
			httpserverport = atoi(port);
		    }
		    break;
		  case OPT_AUTH:
		    if (!strcmp(optval, "magic"))
			auth = HTTPD_AUTH_MAGIC;
		    else if (!strcmp(optval, "basic"))
			auth = HTTPD_AUTH_BASIC;
		    else if (!strcmp(optval, "none"))
			auth = HTTPD_AUTH_NONE;
		    else if (!strcmp(optval, "default"))
			auth = HTTPD_AUTH_MAGIC | HTTPD_AUTH_BASIC;
		    else if (!strcmp(optval, "help") ||
			     !strcmp(optval, "list")) {
			printf(PNAME ": supported HTTP authentication types"
			       " are:\n"
			       "       magic      use Linux /proc/net/tcp to"
			       " determine owner of peer socket\n"
			       "       basic      HTTP Basic username and"
			       " password authentication\n"
			       "       default    use 'magic' if possible, "
			       " otherwise fall back to 'basic'\n"
			       "       none       unauthenticated HTTP (if"
			       " the data file is non-confidential)\n");
			return 0;
		    } else {
			fprintf(stderr, "%s: unrecognised authentication"
				" type '%s'\n%*s  options are 'magic',"
				" 'basic', 'none', 'default'\n",
				PNAME, optval, (int)strlen(PNAME), "");
			return 1;
		    }
		    break;
		  case OPT_AUTHFILE:
		  case OPT_AUTHFD:
		    {
			int fd;
			char namebuf[40];
			const char *name;
			char *authbuf;
			int authlen, authsize;
			int ret;

			if (optid == OPT_AUTHFILE) {
			    fd = open(optval, O_RDONLY);
			    if (fd < 0) {
				fprintf(stderr, "%s: %s: open: %s\n", PNAME,
					optval, strerror(errno));
				return 1;
			    }
			    name = optval;
			} else {
			    fd = atoi(optval);
			    name = namebuf;
			    sprintf(namebuf, "fd %d", fd);
			}

			authlen = 0;
			authsize = 256;
			authbuf = snewn(authsize, char);
			while ((ret = read(fd, authbuf+authlen,
					   authsize-authlen)) > 0) {
			    authlen += ret;
			    if ((authsize - authlen) < (authsize / 16)) {
				authsize = authlen * 3 / 2 + 4096;
				authbuf = sresize(authbuf, authsize, char);
			    }
			}
			if (ret < 0) {
			    fprintf(stderr, "%s: %s: read: %s\n", PNAME,
				    name, strerror(errno));
			    return 1;
			}
			if (optid == OPT_AUTHFILE)
			    close(fd);
			httpauthdata = authbuf;
		    }
		    break;
		  case OPT_INCLUDE:
		  case OPT_INCLUDEPATH:
		  case OPT_EXCLUDE:
		  case OPT_EXCLUDEPATH:
		  case OPT_PRUNE:
		  case OPT_PRUNEPATH:
		    if (ninex >= inexsize) {
			inexsize = ninex * 3 / 2 + 16;
			inex = sresize(inex, inexsize,
				       struct inclusion_exclusion);
		    }
		    inex[ninex].path = (optid == OPT_INCLUDEPATH ||
					optid == OPT_EXCLUDEPATH ||
					optid == OPT_PRUNEPATH);
		    inex[ninex].type = (optid == OPT_INCLUDE ? 1 :
					optid == OPT_INCLUDEPATH ? 1 :
					optid == OPT_EXCLUDE ? 0 :
					optid == OPT_EXCLUDEPATH ? 0 :
					optid == OPT_PRUNE ? -1 :
					/* optid == OPT_PRUNEPATH ? */ -1);
		    inex[ninex].wildcard = optval;
		    ninex++;
		    break;
		}
	    }
        } else {
	    fprintf(stderr, "%s: unexpected argument '%s'\n", PNAME, p);
	    return 1;
        }
    }

    if (nactions == 0) {
	usage(stderr);
	return 1;
    }

    for (action = 0; action < nactions; action++) {
	int mode = actions[action].mode;

	if (mode == SCAN || mode == SCANDUMP || mode == LOAD) {
	    const char *scandir = actions[action].arg;

	    if (mode == LOAD) {
		char *buf = fgetline(stdin);
		unsigned newpathsep;
		buf[strcspn(buf, "\r\n")] = '\0';
		if (1 != sscanf(buf, DUMPHDR "%x",
				&newpathsep)) {
		    fprintf(stderr, "%s: header in dump file not recognised\n",
			    PNAME);
		    return 1;
		}
		pathsep = (char)newpathsep;
		sfree(buf);
	    }

	    if (mode == SCAN || mode == LOAD) {
		/*
		 * Prepare to write out the index file.
		 */
		fd = open(filename, O_RDWR | O_TRUNC | O_CREAT,
			  S_IRUSR | S_IWUSR);
		if (fd < 0) {
		    fprintf(stderr, "%s: %s: open: %s\n", PNAME, filename,
			    strerror(errno));
		    return 1;
		}
		if (fstat(fd, &st) < 0) {
		    perror(PNAME ": fstat");
		    return 1;
		}
		ctx->datafile_dev = st.st_dev;
		ctx->datafile_ino = st.st_ino;
		ctx->straight_to_dump = 0;
	    } else {
		ctx->datafile_dev = -1;
		ctx->datafile_ino = -1;
		ctx->straight_to_dump = 1;
	    }

	    if (mode == SCAN || mode == SCANDUMP) {
		if (stat(scandir, &st) < 0) {
		    fprintf(stderr, "%s: %s: stat: %s\n", PNAME, scandir,
			    strerror(errno));
		    return 1;
		}
		ctx->filesystem_dev = crossfs ? 0 : st.st_dev;
	    }

	    ctx->inex = inex;
	    ctx->ninex = ninex;
	    ctx->crossfs = crossfs;
	    ctx->fakeatimes = fakediratimes;
	    ctx->usemtime = mtime;

	    ctx->last_output_update = time(NULL);

	    /* progress==1 means report progress only if stderr is a tty */
	    if (progress == 1)
		progress = isatty(2) ? 2 : 0;
	    ctx->progress = progress;
	    {
		struct winsize ws;
		if (progress &&
		    ioctl(2, TIOCGWINSZ, &ws) == 0 &&
		    ws.ws_col > 0)
		    ctx->progwidth = ws.ws_col - 1;
		else
		    ctx->progwidth = 79;
	    }

	    if (mode == SCANDUMP)
		printf(DUMPHDR "%02x\n", (unsigned char)pathsep);

	    /*
	     * Scan the directory tree, and write out the trie component
	     * of the data file.
	     */
	    if (mode != SCANDUMP) {
		ctx->tb = triebuild_new(fd);
	    }
	    if (mode == LOAD) {
		char *buf;
		int line = 2;
		while ((buf = fgetline(stdin)) != NULL) {
		    struct trie_file tf;
		    char *p, *q;

		    buf[strcspn(buf, "\r\n")] = '\0';

		    p = buf;
		    q = p;
		    while (*p && *p != ' ') p++;
		    if (!*p) {
			fprintf(stderr, "%s: dump file line %d: expected at least"
				" three fields\n", PNAME, line);
			return 1;
		    }
		    *p++ = '\0';
		    tf.size = strtoull(q, NULL, 10);
		    q = p;
		    while (*p && *p != ' ') p++;
		    if (!*p) {
			fprintf(stderr, "%s: dump file line %d: expected at least"
				" three fields\n", PNAME, line);
			return 1;
		    }
		    *p++ = '\0';
		    tf.atime = strtoull(q, NULL, 10);
		    q = buf;
		    while (*p) {
			int c = *p;
			if (*p == '%') {
			    int i;
			    p++;
			    c = 0;
			    for (i = 0; i < 2; i++) {
				c *= 16;
				if (*p >= '0' && *p <= '9')
				    c += *p - '0';
				else if (*p >= 'A' && *p <= 'F')
				    c += *p - ('A' - 10);
				else if (*p >= 'a' && *p <= 'f')
				    c += *p - ('a' - 10);
				else {
				    fprintf(stderr, "%s: dump file line %d: unable"
					    " to parse hex escape\n", PNAME, line);
				}
				p++;
			    }
			} else {
			    p++;
			}
			*q++ = c;
		    }
		    *q = '\0';
		    triebuild_add(ctx->tb, buf, &tf);
		    sfree(buf);
		    line++;
		}
	    } else {
		du(scandir, gotdata, scan_error, ctx);
	    }
	    if (mode != SCANDUMP) {
		size_t maxpathlen;
		size_t delta;
		char *buf, *prevbuf;

		count = triebuild_finish(ctx->tb);
		triebuild_free(ctx->tb);

		if (ctx->progress) {
		    fprintf(stderr, "%-*s\r", ctx->progwidth, "");
		    fflush(stderr);
		}

		/*
		 * Work out how much space the cumulative index trees
		 * will take; enlarge the file, and memory-map it.
		 */
		if (fstat(fd, &st) < 0) {
		    perror(PNAME ": fstat");
		    return 1;
		}

		printf("Built pathname index, %d entries,"
		       " %llu bytes of index\n", count,
		       (unsigned long long)st.st_size);

		totalsize = index_initial_size(st.st_size, count);
		totalsize += totalsize / 10;

		if (lseek(fd, totalsize-1, SEEK_SET) < 0) {
		    perror(PNAME ": lseek");
		    return 1;
		}
		if (write(fd, "\0", 1) < 1) {
		    perror(PNAME ": write");
		    return 1;
		}

		mappedfile = mmap(NULL, totalsize, PROT_READ|PROT_WRITE,MAP_SHARED, fd, 0);
		if (!mappedfile) {
		    perror(PNAME ": mmap");
		    return 1;
		}

		if (fakediratimes) {
		    printf("Faking directory atimes\n");
		    trie_fake_dir_atimes(mappedfile);
		}

		printf("Building index\n");
		ib = indexbuild_new(mappedfile, st.st_size, count, &delta);
		maxpathlen = trie_maxpathlen(mappedfile);
		buf = snewn(maxpathlen, char);
		prevbuf = snewn(maxpathlen, char);
		tw = triewalk_new(mappedfile);
		prevbuf[0] = '\0';
		tf = triewalk_next(tw, buf);
		assert(tf);
		while (1) {
		    int i;

		    if (totalsize - indexbuild_realsize(ib) < delta) {
			const void *oldfile = mappedfile;
			ptrdiff_t diff;

			/*
			 * Unmap the file, grow it, and remap it.
			 */
			munmap(mappedfile, totalsize);

			totalsize += delta;
			totalsize += totalsize / 10;

			if (lseek(fd, totalsize-1, SEEK_SET) < 0) {
			    perror(PNAME ": lseek");
			    return 1;
			}
			if (write(fd, "\0", 1) < 1) {
			    perror(PNAME ": write");
			    return 1;
			}

			mappedfile = mmap(NULL, totalsize, PROT_READ|PROT_WRITE,MAP_SHARED, fd, 0);
			if (!mappedfile) {
			    perror(PNAME ": mmap");
			    return 1;
			}

			indexbuild_rebase(ib, mappedfile);
			triewalk_rebase(tw, mappedfile);
			diff = (const unsigned char *)mappedfile -
			    (const unsigned char *)oldfile;
			if (prevtf)
			    prevtf = (const struct trie_file *)
				(((const unsigned char *)prevtf) + diff);
			if (tf)
			    tf = (const struct trie_file *)
				(((const unsigned char *)tf) + diff);
		    }

		    /*
		     * Get the next file from the index. So we are
		     * currently holding, and have not yet
		     * indexed, prevtf (with pathname prevbuf) and
		     * tf (with pathname buf).
		     */
		    prevtf = tf;
		    memcpy(prevbuf, buf, maxpathlen);
		    tf = triewalk_next(tw, buf);

		    if (!tf)
			buf[0] = '\0';

		    /*
		     * Find the first differing character position
		     * between our two pathnames.
		     */
		    for (i = 0; prevbuf[i] && prevbuf[i] == buf[i]; i++);

		    /*
		     * If prevbuf was a directory name and buf is
		     * something inside that directory, then
		     * trie_before() will be called on prevbuf
		     * itself. Hence we must drop a tag before it,
		     * so that the resulting index is usable.
		     */
		    if ((!prevbuf[i] && (buf[i] == pathsep ||
					 (i > 0 && buf[i-1] == pathsep))))
			indexbuild_tag(ib);

		    /*
		     * Add prevtf to the index.
		     */
		    indexbuild_add(ib, prevtf);

		    if (!tf) {
			/*
			 * Drop an unconditional final tag, and
			 * get out of this loop.
			 */
			indexbuild_tag(ib);
			break;
		    }

		    /*
		     * If prevbuf was a filename inside some
		     * directory which buf is outside, then
		     * trie_before() will be called on some
		     * pathname either equal to buf or epsilon
		     * less than it. Either way, we're going to
		     * need to drop a tag after prevtf.
		     */
		    if (strchr(prevbuf+i, pathsep) || !tf)
			indexbuild_tag(ib);
		}

		triewalk_free(tw);
		realsize = indexbuild_realsize(ib);
		indexbuild_free(ib);

		munmap(mappedfile, totalsize);
		ftruncate(fd, realsize);
		close(fd);
		printf("Final index file size = %llu bytes\n",
		       (unsigned long long)realsize);
	    }
	} else if (mode == TEXT) {
	    char *querydir = actions[action].arg;
	    size_t pathlen;

	    fd = open(filename, O_RDONLY);
	    if (fd < 0) {
		fprintf(stderr, "%s: %s: open: %s\n", PNAME, filename,
			strerror(errno));
		return 1;
	    }
	    if (fstat(fd, &st) < 0) {
		perror(PNAME ": fstat");
		return 1;
	    }
	    totalsize = st.st_size;
	    mappedfile = mmap(NULL, totalsize, PROT_READ, MAP_SHARED, fd, 0);
	    if (!mappedfile) {
		perror(PNAME ": mmap");
		return 1;
	    }
	    pathsep = trie_pathsep(mappedfile);

	    /*
	     * Trim trailing slash, just in case.
	     */
	    pathlen = strlen(querydir);
	    if (pathlen > 0 && querydir[pathlen-1] == pathsep)
		querydir[--pathlen] = '\0';

	    if (!gotdepth)
		depth = 1;	       /* default for text mode */
	    if (outfile != NULL) {
		FILE *fp = fopen(outfile, "w");
		if (!fp) {
		    fprintf(stderr, "%s: %s: open: %s\n", PNAME,
			    outfile, strerror(errno));
		    return 1;
		}
		text_query(mappedfile, querydir, textcutoff, showfiles,
			   depth, fp);
		fclose(fp);
	    } else {
		text_query(mappedfile, querydir, textcutoff, showfiles,
			   depth, stdout);
	    }

	    munmap(mappedfile, totalsize);
	} else if (mode == HTML) {
	    char *querydir = actions[action].arg;
	    size_t pathlen, maxpathlen;
	    char *pathbuf;
	    struct html_config cfg;
	    unsigned long xi;
	    char *html;

	    fd = open(filename, O_RDONLY);
	    if (fd < 0) {
		fprintf(stderr, "%s: %s: open: %s\n", PNAME, filename,
			strerror(errno));
		if (!querydir) {
		    printf("Status: 500\nContent-type: text/html\n\n"
			   "<html><head>"
			   "<title>500 Internal Server Error</title>"
			   "</head><body>"
			   "<h1>500 Internal Server Error</h1>"
			   "<p><code>agedu</code> suffered an internal error."
			   "</body></html>\n");
		    return 0;
		}
		return 1;
	    }
	    if (fstat(fd, &st) < 0) {
		fprintf(stderr, "%s: %s: fstat: %s\n", PNAME, filename,
			strerror(errno));
		if (!querydir) {
		    printf("Status: 500\nContent-type: text/html\n\n"
			   "<html><head>"
			   "<title>500 Internal Server Error</title>"
			   "</head><body>"
			   "<h1>500 Internal Server Error</h1>"
			   "<p><code>agedu</code> suffered an internal error."
			   "</body></html>\n");
		    return 0;
		}
		return 1;
	    }
	    totalsize = st.st_size;
	    mappedfile = mmap(NULL, totalsize, PROT_READ, MAP_SHARED, fd, 0);
	    if (!mappedfile) {
		fprintf(stderr, "%s: %s: mmap: %s\n", PNAME, filename,
			strerror(errno));
		if (!querydir) {
		    printf("Status: 500\nContent-type: text/html\n\n"
			   "<html><head>"
			   "<title>500 Internal Server Error</title>"
			   "</head><body>"
			   "<h1>500 Internal Server Error</h1>"
			   "<p><code>agedu</code> suffered an internal error."
			   "</body></html>\n");
		    return 0;
		}
		return 1;
	    }
	    pathsep = trie_pathsep(mappedfile);

	    maxpathlen = trie_maxpathlen(mappedfile);
	    pathbuf = snewn(maxpathlen, char);

	    if (!querydir) {
		/*
		 * If we're run in --cgi mode, read PATH_INFO to get
		 * a numeric pathname index.
		 */
		char *path_info = getenv("PATH_INFO");

		if (!path_info)
		    path_info = "";

		/*
		 * Because we need relative links to go to the
		 * right place, it's important that our
		 * PATH_INFO should contain a slash right at the
		 * start, and no slashes anywhere else.
		 */
		if (path_info[0] != '/') {
		    char *servername = getenv("SERVER_NAME");
		    char *scriptname = getenv("SCRIPT_NAME");
		    if (!servername || !scriptname) {
			if (servername)
			    fprintf(stderr, "%s: SCRIPT_NAME unset\n", PNAME);
			else if (scriptname)
			    fprintf(stderr, "%s: SCRIPT_NAME unset\n", PNAME);
			else
			    fprintf(stderr, "%s: SERVER_NAME and "
				    "SCRIPT_NAME both unset\n", PNAME);
			printf("Status: 500\nContent-type: text/html\n\n"
			       "<html><head>"
			       "<title>500 Internal Server Error</title>"
			       "</head><body>"
			       "<h1>500 Internal Server Error</h1>"
			       "<p><code>agedu</code> suffered an internal "
			       "error."
			       "</body></html>\n");
			return 0;
		    }
		    printf("Status: 301\n"
			   "Location: http://%s/%s/\n"
			   "Content-type: text/html\n\n"
			   "<html><head>"
			   "<title>301 Moved</title>"
			   "</head><body>"
			   "<h1>301 Moved</h1>"
			   "<p>Moved."
			   "</body></html>\n",
			   servername, scriptname);
		    return 0;
		} else if (strchr(path_info+1, '/')) {
		    printf("Status: 404\nContent-type: text/html\n\n"
			   "<html><head>"
			   "<title>404 Not Found</title>"
			   "</head><body>"
			   "<h1>400 Not Found</h1>"
			   "<p>Invalid <code>agedu</code> pathname."
			   "</body></html>\n");
		    return 0;
		}
		xi = atoi(path_info + 1);

		if (xi >= trie_count(mappedfile)) {
		    printf("Status: 404\nContent-type: text/html\n\n"
			   "<html><head>"
			   "<title>404 Not Found</title>"
			   "</head><body>"
			   "<h1>400 Not Found</h1>"
			   "<p>This is not a valid pathname index."
			   "</body></html>\n");
		    return 0;
		} else if (!index_has_root(mappedfile, xi)) {
		    printf("Status: 404\nContent-type: text/html\n\n"
			   "<html><head>"
			   "<title>404 Not Found</title>"
			   "</head><body>"
			   "<h1>404 Not Found</h1>"
			   "<p>Pathname index out of range."
			   "</body></html>\n");
		    return 0;
		}
	    } else {
		/*
		 * In ordinary --html mode, process a query
		 * directory passed in on the command line.
		 */

		/*
		 * Trim trailing slash, just in case.
		 */
		pathlen = strlen(querydir);
		if (pathlen > 0 && querydir[pathlen-1] == pathsep)
		    querydir[--pathlen] = '\0';

		xi = trie_before(mappedfile, querydir);
		if (xi >= trie_count(mappedfile) ||
		    (trie_getpath(mappedfile, xi, pathbuf),
		     strcmp(pathbuf, querydir))) {
		    fprintf(stderr, "%s: pathname '%s' does not exist in index\n"
			    "%*s(check it is spelled exactly as it is in the "
			    "index, including\n%*sany leading './')\n",
			    PNAME, querydir,
			    (int)(1+sizeof(PNAME)), "",
			    (int)(1+sizeof(PNAME)), "");
		    return 1;
		} else if (!index_has_root(mappedfile, xi)) {
		    fprintf(stderr, "%s: pathname '%s' is"
			    " a file, not a directory\n", PNAME, querydir);
		    return 1;
		}
	    }

	    if (!querydir || !gotdepth) {
		/*
		 * Single output file.
		 */
		if (!querydir) {
		    cfg.format = "%.0lu";  /* use crosslinks in --cgi mode */
		} else {
		    cfg.format = NULL;
		}
		cfg.rootpage = NULL;
		cfg.autoage = htmlautoagerange;
		cfg.oldest = htmloldest;
		cfg.newest = htmlnewest;
		cfg.showfiles = showfiles;
		html = html_query(mappedfile, xi, &cfg, 1);
		if (querydir && outfile != NULL) {
		    FILE *fp = fopen(outfile, "w");
		    if (!fp) {
			fprintf(stderr, "%s: %s: open: %s\n", PNAME,
				outfile, strerror(errno));
			return 1;
		    } else if (fputs(html, fp) < 0) {
			fprintf(stderr, "%s: %s: write: %s\n", PNAME,
				outfile, strerror(errno));
			fclose(fp);
			return 1;
		    } else if (fclose(fp) < 0) {
			fprintf(stderr, "%s: %s: fclose: %s\n", PNAME,
				outfile, strerror(errno));
			return 1;
		    }
		} else {
		    if (!querydir) {
			printf("Content-type: text/html\n\n");
		    }
		    fputs(html, stdout);
		}
	    } else {
		/*
		 * Multiple output files.
		 */
		int dirlen = outfile ? 2+strlen(outfile) : 3;
		char prefix[dirlen];
		if (outfile) {
		    if (mkdir(outfile, 0777) < 0 && errno != EEXIST) {
			fprintf(stderr, "%s: %s: mkdir: %s\n", PNAME,
				outfile, strerror(errno));
			return 1;
		    }
		    snprintf(prefix, dirlen, "%s/", outfile);
		} else
		    snprintf(prefix, dirlen, "./");

		unsigned long xi2;
		/*
		 * pathbuf is only set up in the plain-HTML case and
		 * not in the CGI case; but that's OK, because the
		 * CGI case can't come to this branch of the if
		 * anyway.
		 */
		make_successor(pathbuf);
		xi2 = trie_before(mappedfile, pathbuf);

		cfg.format = "%lu.html";
		cfg.rootpage = "index.html";
		cfg.autoage = htmlautoagerange;
		cfg.oldest = htmloldest;
		cfg.newest = htmlnewest;
		cfg.showfiles = showfiles;
		if (html_dump(mappedfile, xi, xi2, depth, &cfg, prefix))
		    return 1;
	    }

	    munmap(mappedfile, totalsize);
	    sfree(pathbuf);
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
		perror(PNAME ": fstat");
		return 1;
	    }
	    totalsize = st.st_size;
	    mappedfile = mmap(NULL, totalsize, PROT_READ, MAP_SHARED, fd, 0);
	    if (!mappedfile) {
		perror(PNAME ": mmap");
		return 1;
	    }
	    pathsep = trie_pathsep(mappedfile);

	    maxpathlen = trie_maxpathlen(mappedfile);
	    buf = snewn(maxpathlen, char);

	    printf(DUMPHDR "%02x\n", (unsigned char)pathsep);
	    tw = triewalk_new(mappedfile);
	    while ((tf = triewalk_next(tw, buf)) != NULL)
		dump_line(buf, tf);
	    triewalk_free(tw);

	    munmap(mappedfile, totalsize);
	} else if (mode == HTTPD) {
	    struct html_config pcfg;
	    struct httpd_config dcfg;

	    fd = open(filename, O_RDONLY);
	    if (fd < 0) {
		fprintf(stderr, "%s: %s: open: %s\n", PNAME, filename,
			strerror(errno));
		return 1;
	    }
	    if (fstat(fd, &st) < 0) {
		perror(PNAME ": fstat");
		return 1;
	    }
	    totalsize = st.st_size;
	    mappedfile = mmap(NULL, totalsize, PROT_READ, MAP_SHARED, fd, 0);
	    if (!mappedfile) {
		perror(PNAME ": mmap");
		return 1;
	    }
	    pathsep = trie_pathsep(mappedfile);

	    dcfg.address = httpserveraddr;
	    dcfg.port = httpserverport;
	    dcfg.basicauthdata = httpauthdata;
	    pcfg.format = NULL;
	    pcfg.rootpage = NULL;
	    pcfg.autoage = htmlautoagerange;
	    pcfg.oldest = htmloldest;
	    pcfg.newest = htmlnewest;
	    pcfg.showfiles = showfiles;
	    run_httpd(mappedfile, auth, &dcfg, &pcfg);
	    munmap(mappedfile, totalsize);
	} else if (mode == REMOVE) {
	    if (remove(filename) < 0) {
		fprintf(stderr, "%s: %s: remove: %s\n", PNAME, filename,
			strerror(errno));
		return 1;
	    }
	}
    }

    return 0;
}
