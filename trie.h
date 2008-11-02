/*
 * trie.h: functions to build and search a trie-like structure
 * mapping pathnames to file records.
 */

#include <sys/types.h>

/*
 * An entry in the trie file describing an actual file.
 */
struct trie_file {
    unsigned long long size;
    unsigned long long atime;
};

/* ----------------------------------------------------------------------
 * Functions which can be passed a list of pathnames and file data
 * in strict order, and will build up a trie and write it out to a
 * file.
 */

typedef struct triebuild triebuild;

/*
 * Create a new trie-building context given a writable file
 * descriptor, which should point to the start of an empty file.
 */
triebuild *triebuild_new(int fd);

/*
 * Add a trie_file record to the trie. The pathnames should appear
 * in trie collation order (i.e. strict ASCII sorting order except
 * that / is moved so that it sorts before any other non-NUL
 * character), on penalty of assertion failure.
 */
void triebuild_add(triebuild *tb, const char *pathname,
		   const struct trie_file *file);

/*
 * Put the finishing touches to the contents of the output file
 * once all the trie_file records are present. Returns the total
 * number of elements in the trie.
 */
int triebuild_finish(triebuild *tb);

/*
 * Free the context. (Does not close the file, but may leave the
 * file pointer in an arbitrary place.)
 */
void triebuild_free(triebuild *tb);

/* ----------------------------------------------------------------------
 * Anomalous function which modifies a trie after it's memory-mapped.
 */

/*
 * Invent new fake atimes for each directory in the trie, by
 * taking the maximum (latest) of the directory's previously
 * stored atime and the atimes of everything below it.
 */
void trie_fake_dir_atimes(void *t);

/* ----------------------------------------------------------------------
 * Functions to query a trie given a pointer to the start of the
 * memory-mapped file.
 */

/*
 * Return the path separator character in use in the trie.
 */
char trie_pathsep(const void *t);

/*
 * Return the length of the longest pathname stored in the trie,
 * including its trailing NUL.
 */
size_t trie_maxpathlen(const void *t);

/*
 * Determine the total number of entries in the trie which sort
 * strictly before the given pathname (irrespective of whether the
 * pathname actually exists in the trie).
 */
unsigned long trie_before(const void *t, const char *pathname);

/*
 * Return the pathname for the nth entry in the trie, written into
 * the supplied buffer (which must be large enough, i.e. at least
 * trie_maxpathlen(t) bytes).
 */
void trie_getpath(const void *t, unsigned long n, char *buf);

/*
 * Return the total number of entries in the whole trie.
 */
unsigned long trie_count(const void *t);

/*
 * Enumerate all the trie_file records in the trie, in sequence,
 * and return pointers to their trie_file structures. Returns NULL
 * at end of list, naturally.
 *
 * Optionally returns the pathnames as well: if "buf" is non-NULL
 * then it is expected to point to a buffer large enough to store
 * all the pathnames in the trie (i.e. at least trie_maxpathlen(t)
 * bytes). This buffer is also expected to remain unchanged
 * between calls to triewalk_next(), or else the returned
 * pathnames will be corrupted.
 */
typedef struct triewalk triewalk;
triewalk *triewalk_new(const void *t);
const struct trie_file *triewalk_next(triewalk *tw, char *buf);
void triewalk_free(triewalk *tw);

/* ----------------------------------------------------------------------
 * The trie file also contains an index managed by index.h, so we
 * must be able to ask about that too.
 */
void trie_set_index_offset(void *t, off_t ptr);
off_t trie_get_index_offset(const void *t);

/* ----------------------------------------------------------------------
 * Utility functions not directly involved with the trie.
 */

/*
 * Given a pathname in a buffer, adjust the pathname in place so
 * that it points at a string which, when passed to trie_before,
 * will reliably return the index of the thing that comes after
 * that pathname and all its descendants. Usually this is done by
 * suffixing ^A (since foo^A is guaranteeably the first thing that
 * sorts after foo and foo/stuff); however, if the pathname
 * actually ends in a path separator (e.g. if it's just "/"), that
 * must be stripped off first.
 */
void make_successor(char *pathbuf);
