/*
 * malloc.h: safe wrappers around malloc, realloc, free, strdup
 */

#ifndef AGEDU_MALLOC_H
#define AGEDU_MALLOC_H

#include <stddef.h>

/*
 * smalloc should guarantee to return a useful pointer.
 */
void *smalloc(size_t size);

/*
 * srealloc should guaranteeably be able to realloc NULL
 */
void *srealloc(void *p, size_t size);

/*
 * sfree should guaranteeably deal gracefully with freeing NULL
 */
void sfree(void *p);

/*
 * dupstr is like strdup, but with the never-return-NULL property
 * of smalloc (and also reliably defined in all environments :-)
 */
char *dupstr(const char *s);

/*
 * dupfmt is a bit like printf, but does its own allocation and
 * returns a dynamic string. It also supports a different (and
 * much less featureful) set of format directives:
 * 
 *  - %D takes no argument, and gives the current date and time in
 *    a format suitable for an HTTP Date header
 *  - %d takes an int argument and formats it like normal %d (but
 *    doesn't support any of the configurability of standard
 *    printf)
 *  - %s takes a const char * and formats it like normal %s
 *    (again, no fine-tuning available)
 *  - %h takes a const char * but escapes it so that it's safe for
 *    HTML
 *  - %S takes an int followed by a const char *. If the int is
 *    zero, it behaves just like %s. If the int is nonzero, it
 *    transforms the string by stuffing a \r before every \n.
 */
char *dupfmt(const char *fmt, ...);

/*
 * snew allocates one instance of a given type, and casts the
 * result so as to type-check that you're assigning it to the
 * right kind of pointer. Protects against allocation bugs
 * involving allocating the wrong size of thing.
 */
#define snew(type) \
    ( (type *) smalloc (sizeof (type)) )

/*
 * snewn allocates n instances of a given type, for arrays.
 */
#define snewn(number, type) \
    ( (type *) smalloc ((number) * sizeof (type)) )

/*
 * sresize wraps realloc so that you specify the new number of
 * elements and the type of the element, with the same type-
 * checking advantages. Also type-checks the input pointer.
 */
#define sresize(array, number, type) \
    ( (void)sizeof((array)-(type *)0), \
      (type *) srealloc ((array), (number) * sizeof (type)) )

#endif /* AGEDU_MALLOC_H */
