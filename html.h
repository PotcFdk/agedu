/*
 * html.h: HTML output format for agedu.
 */

struct html_config {
    /*
     * If "format" is non-NULL, it is treated as an sprintf format
     * string which must contain exactly one %lu and no other
     * formatting directives (other than %%, which doesn't count);
     * this will be used to construct URLs to use in hrefs
     * pointing to queries of other related (parent and child)
     * pathnames.
     */
    const char *format;

    /*
     * Time stamps to assign to the extreme ends of the colour
     * scale. If "autoage" is true, they are ignored and the time
     * stamps are derived from the limits of the age data stored
     * in the index.
     */
    int autoage;
    time_t oldest, newest;

    /*
     * Specify whether to show individual files as well as
     * directories in the report.
     */
    int showfiles;
};

/*
 * Generate an HTML document containing the results of a query
 * against the pathname at a given index. Returns a dynamically
 * allocated piece of memory containing the entire HTML document,
 * as an ordinary C zero-terminated string.
 */
char *html_query(const void *t, unsigned long index, 
		 const struct html_config *cfg);
