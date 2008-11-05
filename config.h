/*
 * config.h: stub version of the autoconf-generated config.h, to
 * stop the #include in agedu.h from failing when run from the
 * master source directory instead of from the unpacked contents
 * of the autotoolsified tarball.
 *
 * This version of config.h hardwires some parameters that match
 * the sorts of systems I tend to be building on. A nasty hack and
 * a self-centred one, but autotools is so icky that I can't quite
 * bring myself to inflict it on my main development directory,
 * and am instead keeping it at arm's length in the distribution
 * tarballs. Sorry about that.
 */

#define HAVE_LSTAT64
#define HAVE_FEATURES_H
