/*
 * fgetline.h: Utility function to read a complete line of text
 * from a file, allocating memory for it as large as necessary.
 */

/*
 * Read an entire line of text from a file. Return a dynamically
 * allocated buffer containing the complete line including
 * trailing newline.
 *
 * On error, returns NULL.
 */
char *fgetline(FILE *fp);
