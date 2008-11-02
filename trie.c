/*
 * trie.c: implementation of trie.h.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/types.h>
#include <unistd.h>

#include "agedu.h"
#include "alloc.h"
#include "trie.h"

#define alignof(typ) ( offsetof(struct { char c; typ t; }, t) )

/*
 * Compare functions for pathnames. Returns the relative order of
 * the names, like strcmp; also passes back the offset of the
 * first differing character if desired.
 */
static int trieccmp(unsigned char a, unsigned char b)
{
    a = (a == '\0' ? '\0' : a == pathsep ? '\1' : a+1);
    b = (b == '\0' ? '\0' : b == pathsep ? '\1' : b+1);
    return (int)a - (int)b;
}

static int triencmp(const char *a, size_t alen,
		    const char *b, size_t blen, int *offset)
{
    int off = 0;
    while (off < alen && off < blen && a[off] == b[off])
	off++;
    if (offset)
	*offset = off;
    if (off == alen || off == blen) return (off == blen) - (off == alen);
    return trieccmp(a[off], b[off]);
}

static int triecmp(const char *a, const char *b, int *offset)
{
    return triencmp(a, strlen(a), b, strlen(b), offset);
}

/* ----------------------------------------------------------------------
 * Trie node structures.
 *
 * The trie format stored in the file consists of three distinct
 * node types, each with a distinguishing type field at the start.
 * 
 * TRIE_LEAF is a leaf node; it contains an actual trie_file
 * structure, and indicates that when you're searching down the
 * trie with a string, you should now expect to encounter
 * end-of-string.
 *
 * TRIE_SWITCH indicates that the set of strings in the trie
 * include strings with more than one distinct character after the
 * prefix leading up to this point. Hence, it stores multiple
 * subnode pointers and a different single character for each one.
 *
 * TRIE_STRING indicates that at this point everything in the trie
 * has the same next few characters; it stores a single mandatory
 * string fragment and exactly one subnode pointer.
 */
enum {
    TRIE_LEAF = 0x7fffe000,
    TRIE_SWITCH,
    TRIE_STRING
};

struct trie_common {
    int type;
};

struct trie_switchentry {
    off_t subnode;
    int subcount;
};

struct trie_leaf {
    struct trie_common c;
    struct trie_file file;
};

struct trie_switch {
    struct trie_common c;
    /*
     * sw[0] to sw[len-1] give the subnode pointers and element
     * counts. At &sw[len] is stored len single bytes which are
     * the characters corresponding to each subnode.
     */
    int len;
    struct trie_switchentry sw[];
};

struct trie_string {
    struct trie_common c;
    int stringlen;
    off_t subnode;
    char string[];
};

struct trie_header {
    unsigned long magic;
    off_t root, indexroot;
    int count;
    size_t maxpathlen;
    int pathsep;
};

/* Union only used for computing alignment */
union trie_node {
    struct trie_leaf leaf;
    struct {  /* fake trie_switch with indeterminate array length filled in */
	struct trie_common c;
	int len;
	struct trie_switchentry sw[1];
    } sw;
    struct {  /* fake trie_string with indeterminate array length filled in */
	struct trie_common c;
	int stringlen;
	off_t subnode;
	char string[1];
    } str;
};
#define TRIE_MAGIC 0x75646761UL
#define TRIE_ALIGN alignof(union trie_node)

/* ----------------------------------------------------------------------
 * Trie-building functions.
 */

struct tbswitch {
    int len;
    char c[256];
    off_t off[256];
    int count[256];
};

struct triebuild {
    int fd;
    off_t offset;
    char *lastpath;
    int lastlen, lastsize;
    off_t lastoff;
    struct tbswitch *switches;
    int switchsize;
    size_t maxpathlen;
};

static void tb_seek(triebuild *tb, off_t off)
{
    tb->offset = off;
    if (lseek(tb->fd, off, SEEK_SET) < 0) {
	fprintf(stderr, PNAME ": lseek: %s\n", strerror(errno));
	exit(1);
    }
}

static void tb_write(triebuild *tb, const void *buf, size_t len)
{
    tb->offset += len;
    while (len > 0) {
	int ret = write(tb->fd, buf, len);
	if (ret < 0) {
	    fprintf(stderr, PNAME ": write: %s\n", strerror(errno));
	    exit(1);
	}
	len -= ret;
	buf = (const void *)((const char *)buf + ret);
    }
}

static char trie_align_zeroes[TRIE_ALIGN];

static void tb_align(triebuild *tb)
{
    int off = (TRIE_ALIGN - ((tb)->offset % TRIE_ALIGN)) % TRIE_ALIGN;
    tb_write(tb, trie_align_zeroes, off);
}

triebuild *triebuild_new(int fd)
{
    triebuild *tb = snew(triebuild);
    struct trie_header th;

    tb->fd = fd;
    tb->lastpath = NULL;
    tb->lastlen = tb->lastsize = 0;
    tb->lastoff = 0;
    tb->switches = NULL;
    tb->switchsize = 0;
    tb->maxpathlen = 0;

    th.magic = TRIE_MAGIC;
    th.root = th.count = 0;
    th.indexroot = 0;
    th.maxpathlen = 0;
    th.pathsep = (unsigned char)pathsep;

    tb_seek(tb, 0);
    tb_write(tb, &th, sizeof(th));

    return tb;
}

static off_t triebuild_unwind(triebuild *tb, int targetdepth, int *outcount)
{
    off_t offset;
    int count, depth;

    if (tb->lastoff == 0) {
	*outcount = 0;
	return 0;
    }

    offset = tb->lastoff;
    count = 1;
    depth = tb->lastlen + 1;

    assert(depth >= targetdepth);

    while (depth > targetdepth) {
	int odepth = depth;
	while (depth > targetdepth &&
	       (depth-1 > tb->switchsize || tb->switches[depth-1].len == 0))
	    depth--;
	if (odepth > depth) {
	    /*
	     * Write out a string node.
	     */
	    size_t nodesize = sizeof(struct trie_string) + odepth - depth;
	    struct trie_string *st = (struct trie_string *)smalloc(nodesize);
	    st->c.type = TRIE_STRING;
	    st->stringlen = odepth - depth;
	    st->subnode = offset;
	    memcpy(st->string, tb->lastpath + depth, odepth - depth);
	    tb_align(tb);
	    offset = tb->offset;
	    tb_write(tb, st, nodesize);
	    sfree(st);
	}

	assert(depth >= targetdepth);
	if (depth <= targetdepth)
	    break;

	/*
	 * Now we expect to be sitting just below a switch node.
	 * Add our final entry to it and write it out.
	 */
	depth--;
	{
	    struct trie_switch *sw;
	    char *chars;
	    size_t nodesize;
	    int swlen = tb->switches[depth].len;
	    int i;

	    assert(swlen > 0);

	    tb->switches[depth].c[swlen] = tb->lastpath[depth];
	    tb->switches[depth].off[swlen] = offset;
	    tb->switches[depth].count[swlen] = count;
	    swlen++;

	    nodesize = sizeof(struct trie_switch) +
		swlen * sizeof(struct trie_switchentry) + swlen;
	    sw = (struct trie_switch *)smalloc(nodesize);
	    chars = (char *)&sw->sw[swlen];

	    sw->c.type = TRIE_SWITCH;
	    sw->len = swlen;
	    count = 0;
	    for (i = 0; i < swlen; i++) {
		sw->sw[i].subnode = tb->switches[depth].off[i];
		sw->sw[i].subcount = tb->switches[depth].count[i];
		chars[i] = tb->switches[depth].c[i];

		count += tb->switches[depth].count[i];
	    }

	    tb_align(tb);
	    offset = tb->offset;
	    tb_write(tb, sw, nodesize);
	    sfree(sw);

	    tb->switches[depth].len = 0;   /* clear this node */
	}
    }

    *outcount = count;
    return offset;
}

void triebuild_add(triebuild *tb, const char *pathname,
		   const struct trie_file *file)
{
    int pathlen = strlen(pathname);
    int depth;

    if (tb->maxpathlen < pathlen+1)
	tb->maxpathlen = pathlen+1;

    if (tb->lastpath) {
	off_t offset;
	int count;

	/*
	 * Find the first differing character between this pathname
	 * and the previous one.
	 */
	int ret = triecmp(tb->lastpath, pathname, &depth);
	assert(ret < 0);

	/*
	 * Finalise all nodes above this depth.
	 */
	offset = triebuild_unwind(tb, depth+1, &count);

	/*
	 * Add the final node we just acquired to the switch node
	 * at our chosen depth, creating it if it isn't already
	 * there.
	 */
	if (tb->switchsize <= depth) {
	    int oldsize = tb->switchsize;
	    tb->switchsize = depth * 3 / 2 + 64;
	    tb->switches = sresize(tb->switches, tb->switchsize,
				   struct tbswitch);
	    while (oldsize < tb->switchsize)
		tb->switches[oldsize++].len = 0;
	}

	tb->switches[depth].c[tb->switches[depth].len] = tb->lastpath[depth];
	tb->switches[depth].off[tb->switches[depth].len] = offset;
	tb->switches[depth].count[tb->switches[depth].len] = count;
	tb->switches[depth].len++;
    }

    /*
     * Write out a leaf node for the new file, and remember its
     * file offset.
     */
    {
	struct trie_leaf leaf;

	leaf.c.type = TRIE_LEAF;
	leaf.file = *file;	       /* structure copy */

	tb_align(tb);
	tb->lastoff = tb->offset;
	tb_write(tb, &leaf, sizeof(leaf));
    }

    /*
     * Store this pathname for comparison with the next one.
     */
    if (tb->lastsize < pathlen+1) {
	tb->lastsize = pathlen * 3 / 2 + 64;
	tb->lastpath = sresize(tb->lastpath, tb->lastsize, char);
    }
    strcpy(tb->lastpath, pathname);
    tb->lastlen = pathlen;
}

int triebuild_finish(triebuild *tb)
{
    struct trie_header th;

    th.magic = TRIE_MAGIC;
    th.root = triebuild_unwind(tb, 0, &th.count);
    th.indexroot = 0;
    th.maxpathlen = tb->maxpathlen;
    th.pathsep = (unsigned char)pathsep;

    tb_seek(tb, 0);
    tb_write(tb, &th, sizeof(th));

    return th.count;
}

void triebuild_free(triebuild *tb)
{
    sfree(tb->switches);
    sfree(tb->lastpath);
    sfree(tb);
}

/* ----------------------------------------------------------------------
 * Memory-mapped trie modification.
 */

#define MNODE(t, off, type) \
    ((struct type *)((char *)(t) + (off)))

static unsigned long long fake_atime_recurse(void *t, struct trie_common *node,
					     int last_seen_pathsep)
{
    while (node->type == TRIE_STRING) {
	struct trie_string *st = (struct trie_string *)node;
	last_seen_pathsep = (st->string[st->stringlen-1] == pathsep);
	node = MNODE(t, st->subnode, trie_common);
    }

    if (node->type == TRIE_LEAF) {
	struct trie_leaf *leaf = (struct trie_leaf *)node;
	return leaf->file.atime;
    } else if (assert(node->type == TRIE_SWITCH), 1) {
	struct trie_switch *sw = (struct trie_switch *)node;
	const char *chars = (const char *)&sw->sw[sw->len];
	unsigned long long max = 0, subdir, ret;
	int i;
	int slashindex = -1, bareindex = -1;

	/*
	 * First, process all the children of this node whose
	 * switch characters are not \0 or pathsep. We do this in
	 * reverse order so as to maintain best cache locality
	 * (tracking generally backwards through the file), though
	 * it doesn't matter semantically.
	 *
	 * For each of these children, we're just recursing into
	 * it to do any fixups required below it, and amalgamating
	 * the max atimes we get back.
	 */
	for (i = sw->len; i-- > 0 ;) {
	    if (chars[i] == '\0') {
		bareindex = i;
	    } else if (chars[i] == pathsep) {
		slashindex = i;
	    } else {
		ret = fake_atime_recurse(t, MNODE(t, sw->sw[i].subnode,
						  trie_common), 0);
		if (max < ret)
		    max = ret;
	    }
	}

	/*
	 * Now we have at most two child nodes left to deal with:
	 * one with a slash (or general pathsep) and one with \0.
	 *
	 * If there's a slash node and a bare node, then the slash
	 * node contains precisely everything inside the directory
	 * described by the bare node; so we must retrieve the max
	 * atime for the slash node and use it to fix up the bare
	 * node.
	 *
	 * If there's only a bare node but the pathname leading up
	 * to this point ends in a slash, then _all_ of the child
	 * nodes of this node contain stuff inside the directory
	 * described by the bare node; so we use the whole of the
	 * maximum value we've computed so far to update the bare
	 * node.
	 */
	if (slashindex >= 0) {
	    ret = fake_atime_recurse(t, MNODE(t, sw->sw[slashindex].subnode,
					      trie_common), 1);
	    if (max < ret)
		max = ret;

	    subdir = ret;
	} else if (last_seen_pathsep) {
	    subdir = max;
	} else {
	    /* Don't update the bare subnode at all. */
	    subdir = 0;
	}

	if (bareindex >= 0) {
	    struct trie_leaf *leaf;

	    leaf = MNODE(t, sw->sw[bareindex].subnode, trie_leaf);

	    if (leaf && leaf->c.type == TRIE_LEAF) {
		if (leaf->file.atime < subdir)
		    leaf->file.atime = subdir;
		ret = leaf->file.atime;
	    } else {
		/* Shouldn't really happen, but be cautious anyway */
		ret = fake_atime_recurse(t, &leaf->c, 0);
	    }

	    if (max < ret)
		max = ret;
	}

	return max;
    }
}

void trie_fake_dir_atimes(void *t)
{
    struct trie_header *hdr = MNODE(t, 0, trie_header);
    struct trie_common *node = MNODE(t, hdr->root, trie_common);

    fake_atime_recurse(t, node, 1);
}

/* ----------------------------------------------------------------------
 * Querying functions.
 */

#define NODE(t, off, type) \
    ((const struct type *)((const char *)(t) + (off)))

size_t trie_maxpathlen(const void *t)
{
    const struct trie_header *hdr = NODE(t, 0, trie_header);
    return hdr->maxpathlen;
}

unsigned long trie_before(const void *t, const char *pathname)
{
    const struct trie_header *hdr = NODE(t, 0, trie_header);
    int ret = 0, lastcount = hdr->count;
    int len = 1 + strlen(pathname), depth = 0;
    off_t off = hdr->root;

    while (1) {
	const struct trie_common *node = NODE(t, off, trie_common);
	if (node->type == TRIE_LEAF) {
	    if (depth < len)
		ret += lastcount;   /* _shouldn't_ happen, but in principle */
	    return ret;
	} else if (node->type == TRIE_STRING) {
	    const struct trie_string *st = NODE(t, off, trie_string);

	    int offset;
	    int cmp = triencmp(st->string, st->stringlen,
			       pathname + depth, len-depth, &offset);

	    if (offset < st->stringlen) {
		if (cmp < 0)
		    ret += lastcount;
		return ret;
	    }

	    depth += st->stringlen;
	    off = st->subnode;
	} else if (node->type == TRIE_SWITCH) {
	    const struct trie_switch *sw = NODE(t, off, trie_switch);
	    const char *chars = (const char *)&sw->sw[sw->len];
	    int i;

	    for (i = 0; i < sw->len; i++) {
		int c = chars[i];
		int cmp = trieccmp(pathname[depth], c);
		if (cmp > 0)
		    ret += sw->sw[i].subcount;
		else if (cmp < 0)
		    return ret;
		else {
		    off = sw->sw[i].subnode;
		    lastcount = sw->sw[i].subcount;
		    depth++;
		    break;
		}
	    }
	    if (i == sw->len)
		return ret;
	}
    }
}

void trie_getpath(const void *t, unsigned long n, char *buf)
{
    const struct trie_header *hdr = NODE(t, 0, trie_header);
    int depth = 0;
    off_t off = hdr->root;

    while (1) {
	const struct trie_common *node = NODE(t, off, trie_common);
	if (node->type == TRIE_LEAF) {
	    assert(depth > 0 && buf[depth-1] == '\0');
	    return;
	} else if (node->type == TRIE_STRING) {
	    const struct trie_string *st = NODE(t, off, trie_string);

	    memcpy(buf + depth, st->string, st->stringlen);
	    depth += st->stringlen;
	    off = st->subnode;
	} else if (node->type == TRIE_SWITCH) {
	    const struct trie_switch *sw = NODE(t, off, trie_switch);
	    const char *chars = (const char *)&sw->sw[sw->len];
	    int i;

	    for (i = 0; i < sw->len; i++) {
		if (n < sw->sw[i].subcount) {
		    buf[depth++] = chars[i];
		    off = sw->sw[i].subnode;
		    break;
		} else
		    n -= sw->sw[i].subcount;
	    }
	    assert(i < sw->len);
	}
    }
}    

unsigned long trie_count(const void *t)
{
    const struct trie_header *hdr = NODE(t, 0, trie_header);
    return hdr->count;
}

char trie_pathsep(const void *t)
{
    const struct trie_header *hdr = NODE(t, 0, trie_header);
    return (char)hdr->pathsep;
}

struct triewalk_switch {
    const struct trie_switch *sw;
    int pos, depth, count;
};
struct triewalk {
    const void *t;
    struct triewalk_switch *switches;
    int nswitches, switchsize;
    int count;
};
triewalk *triewalk_new(const void *vt)
{
    triewalk *tw = snew(triewalk);

    tw->t = (const char *)vt;
    tw->switches = NULL;
    tw->switchsize = 0;
    tw->nswitches = -1;
    tw->count = 0;

    return tw;
}
const struct trie_file *triewalk_next(triewalk *tw, char *buf)
{
    off_t off;
    int depth;

    if (tw->nswitches < 0) {
	const struct trie_header *hdr = NODE(tw->t, 0, trie_header);
	off = hdr->root;
	depth = 0;
	tw->nswitches = 0;
    } else {
	while (1) {
	    int swpos;
	    const struct trie_switch *sw;
	    const char *chars;

	    if (tw->nswitches == 0) {
		assert(tw->count == NODE(tw->t, 0, trie_header)->count);
		return NULL;	       /* run out of trie */
	    }

	    swpos = tw->switches[tw->nswitches-1].pos;
	    sw = tw->switches[tw->nswitches-1].sw;
	    chars = (const char *)&sw->sw[sw->len];

	    if (swpos < sw->len) {
		depth = tw->switches[tw->nswitches-1].depth;
		off = sw->sw[swpos].subnode;
		if (buf)
		    buf[depth++] = chars[swpos];
		assert(tw->count == tw->switches[tw->nswitches-1].count);
		tw->switches[tw->nswitches-1].count += sw->sw[swpos].subcount;
		tw->switches[tw->nswitches-1].pos++;
		break;
	    }

	    tw->nswitches--;
	}
    }

    while (1) {
	const struct trie_common *node = NODE(tw->t, off, trie_common);
	if (node->type == TRIE_LEAF) {
	    const struct trie_leaf *lf = NODE(tw->t, off, trie_leaf);
	    if (buf)
		assert(depth > 0 && buf[depth-1] == '\0');
	    tw->count++;
	    return &lf->file;
	} else if (node->type == TRIE_STRING) {
	    const struct trie_string *st = NODE(tw->t, off, trie_string);

	    if (buf)
		memcpy(buf + depth, st->string, st->stringlen);
	    depth += st->stringlen;
	    off = st->subnode;
	} else if (node->type == TRIE_SWITCH) {
	    const struct trie_switch *sw = NODE(tw->t, off, trie_switch);
	    const char *chars = (const char *)&sw->sw[sw->len];

	    if (tw->nswitches >= tw->switchsize) {
		tw->switchsize = tw->nswitches * 3 / 2 + 32;
		tw->switches = sresize(tw->switches, tw->switchsize,
				       struct triewalk_switch);
	    }

	    tw->switches[tw->nswitches].sw = sw;
	    tw->switches[tw->nswitches].pos = 1;
	    tw->switches[tw->nswitches].depth = depth;
	    tw->switches[tw->nswitches].count = tw->count + sw->sw[0].subcount;
	    off = sw->sw[0].subnode;
	    if (buf)
		buf[depth++] = chars[0];
	    tw->nswitches++;
	}
    }
}
void triewalk_free(triewalk *tw)
{
    sfree(tw->switches);
    sfree(tw);
}

void trie_set_index_offset(void *t, off_t ptr)
{
    ((struct trie_header *)t)->indexroot = ptr;
}
off_t trie_get_index_offset(const void *t)
{
    return ((const struct trie_header *)t)->indexroot;
}

void make_successor(char *pathbuf)
{
    int len = strlen(pathbuf);
    if (len > 0 && pathbuf[len-1] == pathsep)
	len--;
    pathbuf[len] = '\001';
    pathbuf[len+1] = '\0';
}
