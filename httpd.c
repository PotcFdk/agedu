/*
 * httpd.c: implementation of httpd.h.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <pwd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <syslog.h>

#include "malloc.h"
#include "html.h"
#include "httpd.h"

/* --- Logic driving what the web server's responses are. --- */

enum { /* connctx states */
    READING_REQ_LINE,
    READING_HEADERS,
    DONE
};

struct connctx {
    const void *t;
    char *data;
    int datalen, datasize;
    char *method, *url, *headers, *auth;
    int state;
};

/*
 * Called when a new connection arrives on a listening socket.
 * Returns a connctx for the new connection.
 */
struct connctx *new_connection(const void *t)
{
    struct connctx *cctx = snew(struct connctx);
    cctx->t = t;
    cctx->data = NULL;
    cctx->datalen = cctx->datasize = 0;
    cctx->state = READING_REQ_LINE;
    cctx->method = cctx->url = cctx->headers = cctx->auth = NULL;
    return cctx;
}

void free_connection(struct connctx *cctx)
{
    sfree(cctx->data);
    sfree(cctx);
}

static char *http_error(char *code, char *errmsg, char *extraheader,
			char *errtext, ...)
{
    return dupfmt("HTTP/1.1 %s %s\r\n"
		  "Date: %D\r\n"
		  "Server: agedu\r\n"
		  "Connection: close\r\n"
		  "%s"
		  "Content-Type: text/html; charset=US-ASCII\r\n"
		  "\r\n"
		  "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n"
		  "<HTML><HEAD>\r\n"
		  "<TITLE>%s %s</TITLE>\r\n"
		  "</HEAD><BODY>\r\n"
		  "<H1>%s %s</H1>\r\n"
		  "<P>%s</P>\r\n"
		  "</BODY></HTML>\r\n", code, errmsg,
		  extraheader ? extraheader : "",
		  code, errmsg, code, errmsg, errtext);
}

static char *http_success(char *mimetype, int stuff_cr, char *document)
{
    return dupfmt("HTTP/1.1 200 OK\r\n"
		  "Date: %D\r\n"
		  "Expires: %D\r\n"
		  "Server: agedu\r\n"
		  "Connection: close\r\n"
		  "Content-Type: %s\r\n"
		  "\r\n"
		  "%S", mimetype, stuff_cr, document);
}

/*
 * Called when data comes in on a connection.
 * 
 * If this function returns NULL, the platform code continues
 * reading from the socket. Otherwise, it returns some dynamically
 * allocated data which the platform code will then write to the
 * socket before closing it.
 */
char *got_data(struct connctx *ctx, char *data, int length,
	       int magic_access, const char *auth_string,
	       const struct html_config *cfg)
{
    char *line, *p, *q, *r, *z1, *z2, c1, c2;
    int auth_provided = 0, auth_correct = 0;
    unsigned long index;
    char *document, *ret;

    /*
     * Add the data we've just received to our buffer.
     */
    if (ctx->datasize < ctx->datalen + length) {
	ctx->datasize = (ctx->datalen + length) * 3 / 2 + 4096;
	ctx->data = sresize(ctx->data, ctx->datasize, char);
    }
    memcpy(ctx->data + ctx->datalen, data, length);
    ctx->datalen += length;

    /*
     * Gradually process the HTTP request as we receive it.
     */
    if (ctx->state == READING_REQ_LINE) {
	/*
	 * We're waiting for the first line of the input, which
	 * contains the main HTTP request. See if we've got it
	 * yet.
	 */

	line = ctx->data;
	/*
	 * RFC 2616 section 4.1: `In the interest of robustness,
	 * [...] if the server is reading the protocol stream at
	 * the beginning of a message and receives a CRLF first,
	 * it should ignore the CRLF.'
	 */
	while (line - ctx->data < ctx->datalen &&
	       (*line == '\r' || *line == '\n'))
	    line++;
	q = line;
	while (q - ctx->data < ctx->datalen && *q != '\n')
	    q++;
	if (q - ctx->data >= ctx->datalen)
	    return NULL;	       /* not got request line yet */

	/*
	 * We've got the first line of the request. Zero-terminate
	 * and parse it into method, URL and optional HTTP
	 * version.
	 */
	*q = '\0';
	ctx->headers = q+1;
	if (q > line && q[-1] == '\r')
	    *--q = '\0';
	z1 = z2 = q;
	c1 = c2 = *q;
	p = line;
	while (*p && !isspace((unsigned char)*p)) p++;
	if (*p) {
	    z1 = p++;
	    c1 = *z1;
	    *z1 = '\0';
	}
	while (*p && isspace((unsigned char)*p)) p++;
	q = p;
	while (*q && !isspace((unsigned char)*q)) q++;
	z2 = q++;
	c2 = *z2;
	*z2 = '\0';
	while (*q && isspace((unsigned char)*q)) q++;

	/*
	 * Now `line' points at the method name; p points at the
	 * URL, if any; q points at the HTTP version, if any.
	 */

	/*
	 * There should _be_ a URL, on any request type at all.
	 */
	if (!*p) {
	    char *ret, *text;
	    /* Restore the request to the way we received it. */
	    *z2 = c2;
	    *z1 = c1;
	    text = dupfmt("<code>agedu</code> received the HTTP request"
			  " \"<code>%h</code>\", which contains no URL.",
			  line);
	    ret = http_error("400", "Bad request", NULL, text);
	    sfree(text);
	    return ret;
	}

	ctx->method = line;
	ctx->url = p;

	/*
	 * If there was an HTTP version, we might need to see
	 * headers. Otherwise, the request is done.
	 */
	if (*q) {
	    ctx->state = READING_HEADERS;
	} else {
	    ctx->state = DONE;
	}
    }

    if (ctx->state == READING_HEADERS) {
	/*
	 * While we're receiving the HTTP request headers, all we
	 * do is to keep scanning to see if we find two newlines
	 * next to each other.
	 */
	q = ctx->data + ctx->datalen;
	for (p = ctx->headers; p < q; p++) {
	    if (*p == '\n' &&
		((p+1 < q && p[1] == '\n') ||
		 (p+2 < q && p[1] == '\r' && p[2] == '\n'))) {
		p[1] = '\0';
		ctx->state = DONE;
		break;
	    }
	}
    }

    if (ctx->state == DONE) {
	/*
	 * Now we have the entire HTTP request. Decide what to do
	 * with it.
	 */
	if (auth_string) {
	    /*
	     * Search the request headers for Authorization.
	     */
	    q = ctx->data + ctx->datalen;
	    for (p = ctx->headers; p < q; p++) {
		const char *hdr = "Authorization:";
		int i;
		for (i = 0; hdr[i]; i++) {
		    if (p >= q || tolower((unsigned char)*p) !=
			tolower((unsigned char)hdr[i]))
			break;
		    p++;
		}
		if (!hdr[i])
		    break;	       /* found our header */
		p = memchr(p, '\n', q - p);
		if (!p)
		    p = q;
	    }
	    if (p < q) {
		auth_provided = 1;
		while (p < q && isspace((unsigned char)*p))
		    p++;
		r = p;
		while (p < q && !isspace((unsigned char)*p))
		    p++;
		if (p < q) {
		    *p++ = '\0';
		    if (!strcasecmp(r, "Basic")) {
			while (p < q && isspace((unsigned char)*p))
			    p++;
			r = p;
			while (p < q && !isspace((unsigned char)*p))
			    p++;
			if (p < q) {
			    *p++ = '\0';
			    if (!strcmp(r, auth_string))
				auth_correct = 1;
			}
		    }
		}
	    }
	}

	if (!magic_access && !auth_correct) {
	    if (auth_string && !auth_provided) {
		ret = http_error("401", "Unauthorized",
				 "WWW-Authenticate: Basic realm=\"agedu\"\r\n",
				 "Please authenticate to view these pages.");
	    } else {
		ret = http_error("403", "Forbidden", NULL,
				 "This is a restricted-access set of pages.");
	    }
	} else {
	    p = ctx->url;
	    p += strspn(p, "/?");
	    index = strtoul(p, NULL, 10);
	    document = html_query(ctx->t, index, cfg);
	    if (document) {
		ret = http_success("text/html", 1, document);
		sfree(document);
	    } else {
		ret = http_error("404", "Not Found", NULL,
				 "Pathname index out of range.");
	    }
	}
	return ret;
    } else
	return NULL;
}

/* --- Platform support for running a web server. --- */

enum { FD_CLIENT, FD_LISTENER, FD_CONNECTION };

struct fd {
    int fd;
    int type;
    int deleted;
    char *wdata;
    int wdatalen, wdatapos;
    int magic_access;
    struct connctx *cctx;
};

struct fd *fds = NULL;
int nfds = 0, fdsize = 0;

struct fd *new_fdstruct(int fd, int type)
{
    struct fd *ret;

    if (nfds >= fdsize) {
	fdsize = nfds * 3 / 2 + 32;
	fds = sresize(fds, fdsize, struct fd);
    }

    ret = &fds[nfds++];

    ret->fd = fd;
    ret->type = type;
    ret->wdata = NULL;
    ret->wdatalen = ret->wdatapos = 0;
    ret->cctx = NULL;
    ret->deleted = 0;
    ret->magic_access = 0;

    return ret;
}

int check_owning_uid(int fd, int flip)
{
    struct sockaddr_in sock, peer;
    socklen_t addrlen;
    char linebuf[4096], matchbuf[80];
    FILE *fp;

    addrlen = sizeof(sock);
    if (getsockname(fd, (struct sockaddr *)&sock, &addrlen)) {
	fprintf(stderr, "getsockname: %s\n", strerror(errno));
	exit(1);
    }
    addrlen = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &addrlen)) {
	if (errno == ENOTCONN) {
	    peer.sin_addr.s_addr = htonl(0);
	    peer.sin_port = htons(0);
	} else {
	    fprintf(stderr, "getpeername: %s\n", strerror(errno));
	    exit(1);
	}
    }

    if (flip) {
	struct sockaddr_in tmp = sock;
	sock = peer;
	peer = tmp;
    }

    sprintf(matchbuf, "%08X:%04X %08X:%04X",
	    peer.sin_addr.s_addr, ntohs(peer.sin_port),
	    sock.sin_addr.s_addr, ntohs(sock.sin_port));
    fp = fopen("/proc/net/tcp", "r");
    if (fp) {
	while (fgets(linebuf, sizeof(linebuf), fp)) {
	    if (strlen(linebuf) >= 75 &&
		!strncmp(linebuf+6, matchbuf, strlen(matchbuf))) {
		return atoi(linebuf + 75);
	    }
	}
    }

    return -1;
}

void check_magic_access(struct fd *fd)
{
    if (check_owning_uid(fd->fd, 0) == getuid())
	fd->magic_access = 1;
}

static void base64_encode_atom(unsigned char *data, int n, char *out)
{
    static const char base64_chars[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    unsigned word;

    word = data[0] << 16;
    if (n > 1)
	word |= data[1] << 8;
    if (n > 2)
	word |= data[2];
    out[0] = base64_chars[(word >> 18) & 0x3F];
    out[1] = base64_chars[(word >> 12) & 0x3F];
    if (n > 1)
	out[2] = base64_chars[(word >> 6) & 0x3F];
    else
	out[2] = '=';
    if (n > 2)
	out[3] = base64_chars[word & 0x3F];
    else
	out[3] = '=';
}

void run_httpd(const void *t, int authmask, const struct httpd_config *dcfg,
	       const struct html_config *incfg)
{
    int fd;
    int authtype;
    char *authstring = NULL;
    unsigned long ipaddr;
    struct fd *f;
    struct sockaddr_in addr;
    socklen_t addrlen;
    struct html_config cfg = *incfg;

    cfg.format = "%.0lu";

    /*
     * Establish the listening socket and retrieve its port
     * number.
     */
    fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
	fprintf(stderr, "socket(PF_INET): %s\n", strerror(errno));
	exit(1);
    }
    addr.sin_family = AF_INET;
    if (!dcfg->address) {
	srand(0L);
	ipaddr = 0x7f000000;
	ipaddr += (1 + rand() % 255) << 16;
	ipaddr += (1 + rand() % 255) << 8;
	ipaddr += (1 + rand() % 255);
	addr.sin_addr.s_addr = htonl(ipaddr);
	addr.sin_port = htons(0);
    } else {
	addr.sin_addr.s_addr = inet_addr(dcfg->address);
	addr.sin_port = dcfg->port ? htons(dcfg->port) : 80;
    }
    addrlen = sizeof(addr);
    if (bind(fd, (struct sockaddr *)&addr, addrlen) < 0) {
	fprintf(stderr, "bind: %s\n", strerror(errno));
	exit(1);
    }
    if (listen(fd, 5) < 0) {
	fprintf(stderr, "listen: %s\n", strerror(errno));
	exit(1);
    }
    addrlen = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &addrlen)) {
	fprintf(stderr, "getsockname: %s\n", strerror(errno));
	exit(1);
    }
    if ((authmask & HTTPD_AUTH_MAGIC) &&
	(check_owning_uid(fd, 1) == getuid())) {
	authtype = HTTPD_AUTH_MAGIC;
	if (authmask != HTTPD_AUTH_MAGIC)
	    printf("Using Linux /proc/net magic authentication\n");
    } else if ((authmask & HTTPD_AUTH_BASIC)) {
	char username[128], password[128], userpassbuf[259];
	const char *userpass;
	const char *rname;
	unsigned char passbuf[10];
	int i, j, k, fd;

	authtype = HTTPD_AUTH_BASIC;

	if (authmask != HTTPD_AUTH_BASIC)
	    printf("Using HTTP Basic authentication\n");

	if (dcfg->basicauthdata) {
	    userpass = dcfg->basicauthdata;
	} else {
	    sprintf(username, "agedu");
	    rname = "/dev/urandom";
	    fd = open(rname, O_RDONLY);
	    if (fd < 0) {
		int err = errno;
		rname = "/dev/random";
		fd = open(rname, O_RDONLY);
		if (fd < 0) {
		    int err2 = errno;
		    fprintf(stderr, "/dev/urandom: open: %s\n", strerror(err));
		    fprintf(stderr, "/dev/random: open: %s\n", strerror(err2));
		    exit(1);
		}
	    }
	    for (i = 0; i < 10 ;) {
		j = read(fd, passbuf + i, 10 - i);
		if (j <= 0) {
		    fprintf(stderr, "%s: read: %s\n", rname,
			    j < 0 ? strerror(errno) : "unexpected EOF");
		    exit(1);
		}
		i += j;
	    }
	    close(fd);
	    for (i = 0; i < 16; i++) {
		/*
		 * 32 characters out of the 36 alphanumerics gives
		 * me the latitude to discard i,l,o for being too
		 * numeric-looking, and w because it has two too
		 * many syllables and one too many presidential
		 * associations.
		 */
		static const char chars[32] =
		    "0123456789abcdefghjkmnpqrstuvxyz";
		int v = 0;

		k = i / 8 * 5;
		for (j = 0; j < 5; j++)
		    v |= ((passbuf[k+j] >> (i%8)) & 1) << j;

		password[i] = chars[v];
	    }
	    password[i] = '\0';

	    sprintf(userpassbuf, "%s:%s", username, password);
	    userpass = userpassbuf;

	    printf("Username: %s\nPassword: %s\n", username, password);
	}

	k = strlen(userpass);
	authstring = snewn(k * 4 / 3 + 16, char);
	for (i = j = 0; i < k ;) {
	    int s = k-i < 3 ? k-i : 3;
	    base64_encode_atom((unsigned char *)(userpass+i), s, authstring+j);
	    i += s;
	    j += 4;
	}
	authstring[j] = '\0';
    } else if ((authmask & HTTPD_AUTH_NONE)) {
	authtype = HTTPD_AUTH_NONE;
	if (authmask != HTTPD_AUTH_NONE)
	    printf("Web server is unauthenticated\n");
    } else {
	fprintf(stderr, "agedu: authentication method not supported\n");
	exit(1);
    }
    if (!dcfg->address) {
	if (ntohs(addr.sin_port) == 80) {
	    printf("URL: http://%s/\n", inet_ntoa(addr.sin_addr));
	} else {
	    printf("URL: http://%s:%d/\n",
		   inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
	}
    }

    /*
     * Now construct an fd structure to hold it.
     */
    f = new_fdstruct(fd, FD_LISTENER);

    /*
     * Read from standard input, and treat EOF as a notification
     * to exit.
     */
    new_fdstruct(0, FD_CLIENT);

    /*
     * Now we're ready to run our main loop. Keep looping round on
     * select.
     */
    while (1) {
	fd_set rfds, wfds;
	int i, j, maxfd, ret;

#define FD_SET_MAX(fd, set, max) \
        do { FD_SET((fd),(set)); (max) = ((max)<=(fd)?(fd)+1:(max)); } while(0)

	/*
	 * Loop round the fd list putting fds into our select
	 * sets. Also in this loop we remove any that were marked
	 * as deleted in the previous loop.
	 */
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	maxfd = 0;
	for (i = j = 0; j < nfds; j++) {

	    if (fds[j].deleted) {
		sfree(fds[j].wdata);
		free_connection(fds[j].cctx);
		continue;
	    }
	    fds[i] = fds[j];

	    switch (fds[i].type) {
	      case FD_CLIENT:
	      case FD_LISTENER:
		FD_SET_MAX(fds[i].fd, &rfds, maxfd);
		break;
	      case FD_CONNECTION:
		/*
		 * Always read from a connection socket. Even
		 * after we've started writing, the peer might
		 * still be sending (e.g. because we shamefully
		 * jumped the gun before waiting for the end of
		 * the HTTP request) and so we should be prepared
		 * to read data and throw it away.
		 */
		FD_SET_MAX(fds[i].fd, &rfds, maxfd);
		/*
		 * Also attempt to write, if we have data to write.
		 */
		if (fds[i].wdatapos < fds[i].wdatalen)
		    FD_SET_MAX(fds[i].fd, &wfds, maxfd);
		break;
	    }

	    i++;
	}
	nfds = i;

        ret = select(maxfd, &rfds, &wfds, NULL, NULL);
	if (ret <= 0) {
	    if (ret < 0 && (errno != EINTR)) {
		fprintf(stderr, "select: %s", strerror(errno));
		exit(1);
	    }
	    continue;
	}

	for (i = 0; i < nfds; i++) {
	    switch (fds[i].type) {
	      case FD_CLIENT:
		if (FD_ISSET(fds[i].fd, &rfds)) {
		    char buf[4096];
		    int ret = read(fds[i].fd, buf, sizeof(buf));
		    if (ret <= 0) {
			if (ret < 0) {
			    fprintf(stderr, "standard input: read: %s\n",
				    strerror(errno));
			    exit(1);
			}
			return;
		    }
		}
		break;
	      case FD_LISTENER:
		if (FD_ISSET(fds[i].fd, &rfds)) {
		    /*
		     * New connection has come in. Accept it.
		     */
		    struct fd *f;
		    struct sockaddr_in addr;
		    socklen_t addrlen = sizeof(addr);
		    int newfd = accept(fds[i].fd, (struct sockaddr *)&addr,
				       &addrlen);
		    if (newfd < 0)
			break;	       /* not sure what happened there */

		    f = new_fdstruct(newfd, FD_CONNECTION);
		    f->cctx = new_connection(t);
		    if (authtype == HTTPD_AUTH_MAGIC)
			check_magic_access(f);
		}
		break;
	      case FD_CONNECTION:
		if (FD_ISSET(fds[i].fd, &rfds)) {
		    /*
		     * There's data to be read.
		     */
		    char readbuf[4096];
		    int ret;

		    ret = read(fds[i].fd, readbuf, sizeof(readbuf));
		    if (ret <= 0) {
			/*
			 * This shouldn't happen in a sensible
			 * HTTP connection, so we abandon the
			 * connection if it does.
			 */
			close(fds[i].fd);
			fds[i].deleted = 1;
			break;
		    } else {
			if (!fds[i].wdata) {
			    /*
			     * If we haven't got an HTTP response
			     * yet, keep processing data in the
			     * hope of acquiring one.
			     */
			    fds[i].wdata = got_data
				(fds[i].cctx, readbuf, ret,
				 (authtype == HTTPD_AUTH_NONE ||
				  fds[i].magic_access), authstring, &cfg);
			    if (fds[i].wdata) {
				fds[i].wdatalen = strlen(fds[i].wdata);
				fds[i].wdatapos = 0;
			    }
			} else {
			    /*
			     * Otherwise, just drop our read data
			     * on the floor.
			     */
			}
		    }
		}
		if (FD_ISSET(fds[i].fd, &wfds) &&
		    fds[i].wdatapos < fds[i].wdatalen) {
		    /*
		     * The socket is writable, and we have data to
		     * write. Write it.
		     */
		    int ret = write(fds[i].fd, fds[i].wdata + fds[i].wdatapos,
				    fds[i].wdatalen - fds[i].wdatapos);
		    if (ret <= 0) {
			/*
			 * Shouldn't happen; abandon the connection.
			 */
			close(fds[i].fd);
			fds[i].deleted = 1;
			break;
		    } else {
			fds[i].wdatapos += ret;
			if (fds[i].wdatapos == fds[i].wdatalen) {
			    shutdown(fds[i].fd, SHUT_WR);
			}
		    }
		}
		break;
	    }
	}

    }
}
