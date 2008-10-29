/*
 * html.h: HTML output format for agedu.
 */

/*
 * Generate an HTML document containing the results of a query
 * against the pathname at a given index. Returns a dynamically
 * allocated piece of memory containing the entire HTML document,
 * as an ordinary C zero-terminated string.
 *
 * If "format" is non-NULL, it is treated as an sprintf format
 * string which must contain exactly one %lu and no other
 * formatting directives (other than %%, which doesn't count);
 * this will be used to construct URLs to use in hrefs pointing to
 * queries of other related (parent and child) pathnames.
 */
char *html_query(const void *t, unsigned long index, const char *format);
