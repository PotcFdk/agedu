#include <stdio.h>
#include <stdbool.h>

typedef struct dumpfile_load_state dumpfile_load_state;
typedef struct dumpfile_record {
    struct trie_file tf;
    const char *pathname;
} dumpfile_record;

dumpfile_load_state *dumpfile_load_init(FILE *fp);
/* dumpfile_load_record returns -1 for bad format, 0 for EOF, 1 for success */
int dumpfile_load_record(dumpfile_load_state *dls, dumpfile_record *dr);
FILE *dumpfile_load_finish(dumpfile_load_state *dls);
char dumpfile_load_get_pathsep(dumpfile_load_state *dls);

typedef struct dumpfile_write_state {
    FILE *fp;
    bool sortable;
    char pathsep;
} dumpfile_write_state;
bool dump_write_header(dumpfile_write_state *ws);
bool dump_write_record(dumpfile_write_state *ws, const dumpfile_record *dr);
