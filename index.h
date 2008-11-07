/*
 * index.h: Manage indexes for agedu.
 */

#include <sys/types.h>

/*
 * Given the size of an existing data file and the number of
 * entries required in the index, return the file size required to
 * store the fixed initial fragment of the index before the
 * indexbuild functions are invoked.
 */
off_t index_initial_size(off_t currentsize, int nodecount);

/*
 * Build an index, given the address of a memory-mapped data file
 * and the starting offset within it.
 *
 * trie_file structures passed to tf must of course be within the
 * bounds of the memory-mapped file.
 *
 * The "delta" parameter to indexbuild_new is filled in with the
 * maximum size by which the index can ever grow in a single
 * indexbuild_add call. This can be used, together with
 * indexbuild_realsize, to detect when the index is about to get
 * too big for its file and the file needs resizing.
 *
 * indexbuild_add adds a trie_file structure to the index.
 * indexbuild_tag, called after that, causes the current state of
 * the index to be preserved so that it can be queried later. It's
 * idempotent, and will safely do nothing if called before
 * indexbuild_add.
 *
 * indexbuild_realsize returns the total amount of data _actually_
 * written into the file, to allow it to be truncated afterwards,
 * and to allow the caller of the indexbuild functions to know
 * when the file is about to need to grow bigger during index
 * building.
 *
 * indexbuild_rebase is used when the file has been
 * re-memory-mapped at a different address (because it needs to
 * grow).
 */
typedef struct indexbuild indexbuild;
indexbuild *indexbuild_new(void *t, off_t startoff, int nodecount,
			   size_t *delta);
void indexbuild_add(indexbuild *ib, const struct trie_file *tf);
void indexbuild_tag(indexbuild *ib);
void indexbuild_rebase(indexbuild *ib, void *t);
off_t indexbuild_realsize(indexbuild *ib);
void indexbuild_free(indexbuild *ib);

/*
 * Query an index to find the total size of records with name
 * index strictly less than n, with atime less than at.
 */
unsigned long long index_query(const void *t, int n, unsigned long long at);

/*
 * Retrieve an order statistic from the index: given a fraction f,
 * return an atime such that (at most) the requested fraction of
 * the total data is older than it.
 */
unsigned long long index_order_stat(const void *t, double f);
