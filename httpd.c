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

/* --- Logic driving what the web server's responses are. --- */

struct connctx {
    const void *t;
    char *data;
    int datalen, datasize;
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
    return cctx;
}

void free_connection(struct connctx *cctx)
{
    sfree(cctx->data);
    sfree(cctx);
}

static char *http_error(char *code, char *errmsg, char *errtext, ...)
{
    return dupfmt("HTTP/1.1 %s %s\r\n"
		  "Date: %D\r\n"
		  "Server: agedu\r\n"
		  "Connection: close\r\n"
		  "Content-Type: text/html; charset=US-ASCII\r\n"
		  "\r\n"
		  "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n"
		  "<HTML><HEAD>\r\n"
		  "<TITLE>%s %s</TITLE>\r\n"
		  "</HEAD><BODY>\r\n"
		  "<H1>%s %s</H1>\r\n"
		  "<P>%s</P>\r\n"
		  "</BODY></HTML>\r\n", code, errmsg,
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
char *got_data(struct connctx *ctx, char *data, int length, int magic_access)
{
    char *line, *p, *q, *z1, *z2, c1, c2;
    unsigned long index;
    char *document, *ret;

    if (ctx->datasize < ctx->datalen + length) {
	ctx->datasize = (ctx->datalen + length) * 3 / 2 + 4096;
	ctx->data = sresize(ctx->data, ctx->datasize, char);
    }
    memcpy(ctx->data + ctx->datalen, data, length);
    ctx->datalen += length;

    /*
     * See if we have enough of an HTTP request to work out our
     * response.
     */
    line = ctx->data;
    /*
     * RFC 2616 section 4.1: `In the interest of robustness, [...]
     * if the server is reading the protocol stream at the
     * beginning of a message and receives a CRLF first, it should
     * ignore the CRLF.'
     */
    while (line - ctx->data < ctx->datalen &&
	   (*line == '\r' || *line == '\n'))
	line++;

    q = line;
    while (q - ctx->data < ctx->datalen && *q != '\r' && *q != '\n')
	q++;
    if (q - ctx->data >= ctx->datalen)
	return NULL;	       /* not got request line yet */

    /*
     * We've got the first line of the request, which is enough
     * for us to work out what to say in response and start
     * sending it. The platform side will keep reading data, but
     * it'll ignore it.
     *
     * So, zero-terminate our line and parse it.
     */
    *q = '\0';
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
    z2 = q;
    c2 = *z2;
    *z2 = '\0';

    /*
     * Now `line' points at the method name; p points at the URL,
     * if any.
     */
    
    /*
     * There should _be_ a URL, on any request type at all.
     */
    if (!*p) {
	char *ret, *text;
	*z2 = c2;
	*z1 = c1;
	text = dupfmt("<code>agedu</code> received the HTTP request"
		      " \"<code>%s</code>\", which contains no URL.",
		      line);
	ret = http_error("400", "Bad request", text);
	sfree(text);
	return ret;
    }

    if (!magic_access) {
	ret = http_error("403", "Forbidden",
			 "This is a restricted-access set of pages.");
    } else {
	p += strspn(p, "/?");
	index = strtoul(p, NULL, 10);
	document = html_query(ctx->t, index, "%lu");
	if (document) {
	    ret = http_success("text/html", 1, document);
	    sfree(document);
	} else {
	    ret = http_error("404", "Not Found",
			     "Pathname index out of range.");
	}
    }
    return ret;
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

void check_magic_access(struct fd *fd)
{
    struct sockaddr_in sock, peer;
    socklen_t addrlen;
    char linebuf[4096], matchbuf[80];
    FILE *fp;

    addrlen = sizeof(sock);
    if (getsockname(fd->fd, (struct sockaddr *)&sock, &addrlen)) {
	fprintf(stderr, "getsockname: %s\n", strerror(errno));
	exit(1);
    }
    addrlen = sizeof(peer);
    if (getpeername(fd->fd, (struct sockaddr *)&peer, &addrlen)) {
	fprintf(stderr, "getpeername: %s\n", strerror(errno));
	exit(1);
    }

    sprintf(matchbuf, "%08X:%04X %08X:%04X",
	    peer.sin_addr.s_addr, ntohs(peer.sin_port),
	    sock.sin_addr.s_addr, ntohs(sock.sin_port));
    fp = fopen("/proc/net/tcp", "r");
    if (fp) {
	while (fgets(linebuf, sizeof(linebuf), fp)) {
	    if (strlen(linebuf) >= 75 &&
		!strncmp(linebuf+6, matchbuf, strlen(matchbuf))) {
		int uid = atoi(linebuf + 75);
		if (uid == getuid())
		    fd->magic_access = 1;
	    }
	}
    }
}

void run_httpd(const void *t)
{
    int fd;
    unsigned long ipaddr;
    struct fd *f;
    struct sockaddr_in addr;
    socklen_t addrlen;

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
    srand(0L);
    ipaddr = 0x7f000000;
    ipaddr += (1 + rand() % 255) << 16;
    ipaddr += (1 + rand() % 255) << 8;
    ipaddr += (1 + rand() % 255);
    addr.sin_addr.s_addr = htonl(ipaddr);
    addr.sin_port = htons(0);
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
    printf("Server is at http://%s:%d/\n",
	   inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

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
			    fds[i].wdata = got_data(fds[i].cctx, readbuf,
						    ret, fds[i].magic_access);
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
