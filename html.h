/*
 * html.h: HTML output format for agedu.
 */

struct html_config {
    /*
     * Configure the format of the URI pathname fragment corresponding
     * to a given tree entry.
     *
     * 'uriformat' is expected to have the following format:
     *  - it consists of one or more _options_, each indicating a
     *    particular way to format a URI, separated by '%|'
     *  - each option contains _at most one_ formatting directive;
     *    without any, it is assumed to only be able to encode the
     *    root tree entry
     *  - the formatting directive may be followed before and/or
     *    afterwards with literal text; percent signs in that literal
     *    text are specified as %% (which doesn't count as a
     *    formatting directive for the 'at most one' rule)
     *  - formatting directives are as follows:
     *     + '%n' outputs the numeric index (in decimal) of the tree
     *       entry
     *     + '%p' outputs the pathname of the tree entry, not counting
     *       any common prefix of the whole tree or a subdirectory
     *       separator following that (so that the root directory of
     *       the tree will always be rendered as the empty string).
     *       The subdirectory separator is translated into '/'; any
     *       remotely worrying character is escaped as = followed by
     *       two hex digits (including, in particular, = itself). The
     *       only characters not escaped are the ASCII alphabets and
     *       numbers, the subdirectory separator as mentioned above,
     *       and the four punctuation characters -.@_ (with the
     *       exception that at the very start of a pathname, even '.'
     *       is escaped).
     *     - '%/p' outputs the pathname of the tree entry, but this time
     *       the subdirectory separator is also considered to be a
     *       worrying character and is escaped.
     *     - '%-p' and '%-/p' are like '%p' and '%/p' respectively,
     *       except that they use the full pathname stored in the tree
     *       without stripping a common prefix.
     *
     * These formats are used both for generating and parsing URI
     * fragments. When generating, the first valid option is used
     * (which is always the very first one if we're generating the
     * root URI, or else it's the first option with any formatting
     * directive); when parsing, the first option that matches will be
     * accepted. (Thus, you can have '.../subdir' and '.../subdir/'
     * both accepted, but make the latter canonical; clients of this
     * mechanism will typically regenerate a URI string after parsing
     * an index out of it, and return an HTTP redirect if it isn't in
     * canonical form.)
     *
     * All hyperlinks should be correctly generated as relative (i.e.
     * with the right number of ../ and ./ considering both the
     * pathname for the page currently being generated, and the one
     * for the link target).
     *
     * If 'uriformat' is NULL, the HTML is generated without hyperlinks.
     */
    const char *uriformat;

    /*
     * Configure the filenames output by html_dump(). These can be
     * configured separately from the URI formats, so that the root
     * file can be called index.html on disk but have a notional URI
     * of just / or similar.
     *
     * Formatting directives are the same as the uriformat above.
     */
    const char *fileformat;

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
    /*
     * The string appearing in the <title> part of HTML pages, before
     * a colon followed by the path being served. Default is "agedu".
     */
    const char *html_title;
};

/*
 * Parse a URI pathname segment against the URI formats specified in
 * 'cfg', and return a numeric index in '*index'. Return value is true
 * on success, or false if the pathname makes no sense, or the index
 * is out of range, or the index does not correspond to a directory in
 * the trie.
 */
int html_parse_path(const void *t, const char *path,
                    const struct html_config *cfg, unsigned long *index);

/*
 * Generate a URI pathname segment from an index.
 */
char *html_format_path(const void *t, const struct html_config *cfg,
                       unsigned long index);

/*
 * Generate an HTML document containing the results of a query
 * against the pathname at a given index. Returns a dynamically
 * allocated piece of memory containing the entire HTML document,
 * as an ordinary C zero-terminated string.
 *
 * 'downlink' is TRUE if hyperlinks should be generated for
 * subdirectories. (This can also be disabled by setting cfg->format
 * to NULL, but that also disables the upward hyperlinks to parent
 * directories. Setting cfg->format to non-NULL but downlink to NULL
 * will generate uplinks but no downlinks.)
 */
char *html_query(const void *t, unsigned long index, 
		 const struct html_config *cfg, int downlink);

/*
 * Recursively output a dump of lots of HTML files which crosslink
 * to each other. cfg->format and cfg->rootpath will be used to
 * generate the filenames for both the hyperlinks and the output
 * file names; the file names will have "pathprefix" prepended to
 * them before being opened.
 *
 * "index" and "endindex" point to the region of index file that
 * should be generated by the dump, which must be a subdirectory.
 *
 * "maxdepth" limits the depth of recursion. Setting it to zero
 * outputs only one page, 1 outputs the current directory and its
 * immediate children but no further, and so on. Making it negative
 * gives unlimited depth.
 *
 * Return value is 0 on success, or 1 if an error occurs during
 * output.
 */
int html_dump(const void *t, unsigned long index, unsigned long endindex,
	      int maxdepth, const struct html_config *cfg,
	      const char *pathprefix);
