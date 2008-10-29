/*
 * index.h: Manage indexes for agedu.
 */

#include <sys/types.h>

/*
 * Given the size of an existing data file and the number of
 * entries required in the index, calculate and return an upper
 * bound on the required size of the index file after an index has
 * been written on to the end of it.
 */
off_t index_compute_size(off_t currentsize, int nodecount);

/*
 * Build an index, given the address of a memory-mapped data file
 * and the starting offset within it.
 *
 * trie_file structures passed to tf must of course be within the
 * bounds of the memory-mapped file.
 *
 * indexbuild_realsize returns the total amount of data _actually_
 * written into the file, to allow it to be truncated afterwards.
 */
typedef struct indexbuild indexbuild;
indexbuild *indexbuild_new(void *t, off_t startoff, int nodecount);
void indexbuild_add(indexbuild *ib, const struct trie_file *tf);
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
