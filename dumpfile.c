#include "agedu.h"
#include "alloc.h"
#include "trie.h"
#include "dumpfile.h"
#include "fgetline.h"

#define DUMPHDR "agedu dump file. pathsep="

struct dumpfile_load_state {
    FILE *fp;
    char pathsep;
    int lineno;
    char *prev_pathname;
};

dumpfile_load_state *dumpfile_load_init(FILE *fp)
{
    char *buf = fgetline(fp);
    if (!buf) {
        fprintf(stderr, "%s: EOF at start of dump file\n", PNAME);
        return NULL;
    }

    unsigned pathsep;

    buf[strcspn(buf, "\r\n")] = '\0';
    if (1 != sscanf(buf, DUMPHDR "%x", &pathsep)) {
        sfree(buf);
        fprintf(stderr, "%s: header in dump file not recognised\n", PNAME);
        return NULL;
    }
    sfree(buf);

    dumpfile_load_state *dls = snew(dumpfile_load_state);
    dls->fp = fp;
    dls->pathsep = (char)pathsep;
    dls->lineno = 1;
    dls->prev_pathname = NULL;
    return dls;
}

FILE *dumpfile_load_finish(dumpfile_load_state *dls)
{
    FILE *fp = dls->fp;
    sfree(dls->prev_pathname);
    sfree(dls);
    return fp;
}

char dumpfile_load_get_pathsep(dumpfile_load_state *dls)
{
    return dls->pathsep;
}

int dumpfile_load_record(dumpfile_load_state *dls, dumpfile_record *dr)
{
    dr->pathname = NULL;

    char *buf = fgetline(stdin);
    if (!buf)
        return 0;
    dls->lineno++;
    buf[strcspn(buf, "\r\n")] = '\0';

    char *p = buf;

    char *q = p;
    while (*p && *p != ' ') p++;
    if (!*p) {
        fprintf(stderr, "%s: dump file line %d: could not find size field\n",
                PNAME, dls->lineno);
        return -1;
    }
    *p++ = '\0';
    dr->tf.size = strtoull(q, NULL, 10);

    q = p;
    while (*p && *p != ' ') p++;
    if (*p)
        *p++ = '\0';
    dr->tf.atime = strtoull(q, NULL, 10);

    q = buf;
    if (!*p) {
        fprintf(stderr, "%s: dump file line %d: could not find "
                "pathname field\n", PNAME, dls->lineno);
        return -1;
    }
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
                            " to parse hex escape\n", PNAME, dls->lineno);
                    return -1;
                }
                p++;
            }
        } else {
            p++;
        }
        *q++ = c;
    }

    *q = '\0';

    sfree(dls->prev_pathname);
    dls->prev_pathname = buf;
    dr->pathname = buf;
    return 1;
}

bool dump_write_header(dumpfile_write_state *ws)
{
    if (fprintf(ws->fp, DUMPHDR "%02x\n",
                (unsigned char)ws->pathsep) < 0)
        return false;
    return true;
}

bool dump_write_record(dumpfile_write_state *ws, const dumpfile_record *dr)
{
    const char *p;

    if (fprintf(ws->fp, "%llu %llu", dr->tf.size, dr->tf.atime) < 0)
        return false;

    if (putc(' ', ws->fp) == EOF)
        return false;

    /* Pathname comes second, and is %-escaped. */
    for (p = dr->pathname; *p; p++) {
        if (*p >= ' ' && *p < 127 && *p != '%') {
            if (putc(*p, ws->fp) == EOF)
                return false;
        } else {
            if (fprintf(ws->fp, "%%%02x", (unsigned char)*p) < 0)
                return false;
        }
    }

    if (putc('\n', ws->fp) == EOF)
        return false;

    return true;
}
