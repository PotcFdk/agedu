#include "agedu.h"
#include "alloc.h"
#include "trie.h"
#include "dumpfile.h"
#include "fgetline.h"

#define DUMPHDR "agedu dump file. pathsep="
#define DUMPHDR_SORTABLE "0:agedu sortable dump file. pathsep="

struct dumpfile_load_state {
    FILE *fp;
    char pathsep;
    int lineno;
    char *prev_pathname;
    bool sortable;
    bool check_order;
};

dumpfile_load_state *dumpfile_load_init(FILE *fp, bool check_order)
{
    char *buf = fgetline(fp);
    if (!buf) {
        fprintf(stderr, "%s: EOF at start of dump file\n", PNAME);
        return NULL;
    }

    unsigned pathsep;
    bool sortable;

    buf[strcspn(buf, "\r\n")] = '\0';
    if (1 == sscanf(buf, DUMPHDR "%x", &pathsep)) {
        sortable = false;
    } else if (1 == sscanf(buf, DUMPHDR_SORTABLE "%x", &pathsep)) {
        sortable = true;
    } else {
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
    dls->sortable = sortable;
    dls->check_order = check_order;
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

    if (dls->sortable) {
        if (buf[0] != '1' || buf[1] != ':') {
            fprintf(stderr, "%s: dump file line %d: could not find expected "
                    "pathname prefix\n", PNAME, dls->lineno);
            return -1;
        }
        /* Skip over the sortably-encoded pathname looking for the
         * numeric fields after it */
        while (*p && *p != ' ') p++;
        if (*p) {
            p++;
        } else {
            fprintf(stderr, "%s: dump file line %d: could not find space "
                    "after encoded pathname\n", PNAME, dls->lineno);
            return -1;
        }
    }

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
    if (!dls->sortable) {
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
    } else {
        /* Rewind to the start of the line for the pathname */
        p = buf + 2;

        while (*p && *p != ' ') {
            if (*p == 'A') {
                *q++ = dls->pathsep;
                p++;
            } else if (p[0] >= 'B' && p[0] < 'B'+16 &&
                       p[1] >= 'b' && p[1] < 'b'+16) {
                *q++ = (p[0] - 'B') * 16 + (p[1] - 'b');
                p += 2;
            } else {
                fprintf(stderr, "%s: dump file line %d: unable"
                        " to parse encoded pathname\n", PNAME, dls->lineno);
                return -1;
            }
        }
    }

    *q = '\0';

    if (dls->check_order && dls->prev_pathname &&
        triecmp(dls->prev_pathname, buf, NULL, dls->pathsep) >= 0) {
        fprintf(stderr, "%s: dump file line %d: pathname sorts before "
                "pathname on previous line\n", PNAME, dls->lineno);
        return -1;
    }

    sfree(dls->prev_pathname);
    dls->prev_pathname = buf;
    dr->pathname = buf;
    return 1;
}

bool dump_write_header(dumpfile_write_state *ws)
{
    if (!ws->sortable) {
        if (fprintf(ws->fp, DUMPHDR "%02x\n",
                    (unsigned char)ws->pathsep) < 0)
            return false;
    } else {
        if (fprintf(ws->fp, DUMPHDR_SORTABLE "%02x\n",
                    (unsigned char)ws->pathsep) < 0)
            return false;
    }
    return true;
}

bool dump_write_record(dumpfile_write_state *ws, const dumpfile_record *dr)
{
    const char *p;

    if (ws->sortable) {
        /* Pathname comes first, and is hex-ishly encoded */
        printf("1:");                  /* prefix to sort after header line */
        for (const char *p = dr->pathname; *p; p++) {
            if (*p == ws->pathsep) {
                if (putc('A', ws->fp) == EOF)
                    return false;
            } else {
                unsigned char val = *p;
                char c1 = 'B' + (0xF & (val >> 4));
                char c2 = 'b' + (0xF & (val >> 0));
                if (putc(c1, ws->fp) == EOF || putc(c2, ws->fp) == EOF)
                    return false;
            }
        }
        if (putc(' ', ws->fp) == EOF)
            return false;
    }

    if (fprintf(ws->fp, "%llu %llu", dr->tf.size, dr->tf.atime) < 0)
        return false;

    if (!ws->sortable) {
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
    }

    if (putc('\n', ws->fp) == EOF)
        return false;

    return true;
}
