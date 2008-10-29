/*
 * malloc.c: implementation of malloc.h
 */

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <assert.h>
#include <stdio.h>

#include "malloc.h"

#define lenof(x) (sizeof((x))/sizeof(*(x)))

extern void fatal(const char *, ...);

void *smalloc(size_t size) {
    void *p;
    p = malloc(size);
    if (!p) {
	fatal("out of memory");
    }
    return p;
}

void sfree(void *p) {
    if (p) {
	free(p);
    }
}

void *srealloc(void *p, size_t size) {
    void *q;
    if (p) {
	q = realloc(p, size);
    } else {
	q = malloc(size);
    }
    if (!q)
	fatal("out of memory");
    return q;
}

char *dupstr(const char *s) {
    char *r = smalloc(1+strlen(s));
    strcpy(r,s);
    return r;
}

char *dupfmt(const char *fmt, ...)
{
    int pass;
    int totallen;
    char *ret = NULL, *rp = NULL;
    char datebuf[80];
    va_list ap;
    time_t t;
    struct tm tm;
    int got_time = 0;

    datebuf[0] = '\0';
    totallen = 0;

    for (pass = 0; pass < 2; pass++) {
	const char *p = fmt;

	va_start(ap, fmt);

	while (*p) {
	    const char *data = NULL;
	    int datalen = 0, stuffcr = 0;

	    if (*p == '%') {
		p++;
		if (*p == 'D') {
		    if (!datebuf[0]) {
			if (!got_time) {
			    t = time(NULL);
			    tm = *gmtime(&t);
			    got_time = 1;
			}
			strftime(datebuf, lenof(datebuf),
				 "%a, %d %b %Y %H:%M:%S GMT", &tm);
		    }
		    data = datebuf;
		    datalen = strlen(data);
		} else if (*p == 'd') {
		    int i = va_arg(ap, int);
		    sprintf(datebuf, "%d", i);
		    data = datebuf;
		    datalen = strlen(data);
		} else if (*p == 's') {
		    data = va_arg(ap, const char *);
		    datalen = strlen(data);
		} else if (assert(*p == 'S'), 1) {
		    stuffcr = va_arg(ap, int);
		    data = va_arg(ap, const char *);
		    datalen = strlen(data);
		}
		p++;
	    } else {
		data = p;
		while (*p && *p != '%') p++;
		datalen = p - data;
	    }

	    if (pass == 0) {
		totallen += datalen;
		if (stuffcr) {
		    while (datalen > 0) {
			if (*data == '\n')
			    totallen++;
			data++, datalen--;
		    }
		}
	    } else {
		while (datalen > 0) {
		    if (stuffcr && *data == '\n')
			*rp++ = '\r';
		    *rp++ = *data++;
		    datalen--;
		}
	    }
	}

	va_end(ap);

	if (pass == 0) {
	    rp = ret = snewn(totallen+1, char);
	} else {
	    assert(rp - ret == totallen);
	    *rp = '\0';
	}
    }

    return ret;
}
