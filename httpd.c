/*
 * httpd.c: implementation of httpd.h.
 */

#include "agedu.h"
#include "alloc.h"
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
		  "Server: " PNAME "\r\n"
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
		  "Server: " PNAME "\r\n"
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
    int auth_correct = 0;
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
	    text = dupfmt("<code>" PNAME "</code> received the HTTP request"
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
	    if (auth_string) {
		ret = http_error("401", "Unauthorized",
				 "WWW-Authenticate: Basic realm=\""PNAME"\"\r\n",
				 "\nYou must authenticate to view these pages.");
	    } else {
		ret = http_error("403", "Forbidden", NULL,
				 "This is a restricted-access set of pages.");
	    }
	} else {
	    p = ctx->url;
	    if (!html_parse_path(ctx->t, p, cfg, &index)) {
		ret = http_error("404", "Not Found", NULL,
				 "This is not a valid pathname.");
	    } else {
                char *canonpath = html_format_path(ctx->t, cfg, index);
                if (!strcmp(canonpath, p)) {
                    /*
                     * This is a canonical path. Return the document.
                     */
                    document = html_query(ctx->t, index, cfg, 1);
                    if (document) {
                        ret = http_success("text/html", 1, document);
                        sfree(document);
                    } else {
                        ret = http_error("404", "Not Found", NULL,
                                         "This is not a valid pathname.");
                    }
                } else {
                    /*
                     * This is a non-canonical path. Return a redirect
                     * to the right one.
                     *
                     * To do this, we must search the request headers
                     * for Host:, to see what the client thought it
                     * was calling our server.
                     */

                    char *host = NULL;
                    q = ctx->data + ctx->datalen;
                    for (p = ctx->headers; p < q; p++) {
                        const char *hdr = "Host:";
                        int i;
                        for (i = 0; hdr[i]; i++) {
                            if (p >= q || tolower((unsigned char)*p) !=
                                tolower((unsigned char)hdr[i]))
                                break;
                            p++;
                        }
                        if (!hdr[i])
                            break;     /* found our header */
                        p = memchr(p, '\n', q - p);
                        if (!p)
                            p = q;
                    }
                    if (p < q) {
                        while (p < q && isspace((unsigned char)*p))
                            p++;
                        r = p;
                        while (p < q) {
                            if (*p == '\r' && (p+1 >= q || p[1] == '\n'))
                                break;
                            p++;
                        }
                        host = snewn(p-r+1, char);
                        memcpy(host, r, p-r);
                        host[p-r] = '\0';
                    }
                    if (host) {
                        char *header = dupfmt("Location: http://%s%s\r\n",
                                              host, canonpath);
                        ret = http_error("301", "Moved", header,
                                         "This is not the canonical form of"
                                         " this pathname.");
                        sfree(header);
                    } else {
                        ret = http_error("400", "Bad Request", NULL,
                                         "Needed a Host: header to return"
                                         " the intended redirection.");
                    }
                }
                sfree(canonpath);
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
    struct sockaddr_storage sock, peer;
    int connected;
    socklen_t addrlen;
    char linebuf[4096], matchbuf[128];
    char *filename;
    int matchcol, uidcol;
    FILE *fp;

    addrlen = sizeof(sock);
    if (getsockname(fd, (struct sockaddr *)&sock, &addrlen)) {
	fprintf(stderr, "getsockname: %s\n", strerror(errno));
	exit(1);
    }
    addrlen = sizeof(peer);
    connected = 1;
    if (getpeername(fd, (struct sockaddr *)&peer, &addrlen)) {
	if (errno == ENOTCONN) {
            connected = 0;
            memset(&peer, 0, sizeof(peer));
            peer.ss_family = sock.ss_family;
	} else {
	    fprintf(stderr, "getpeername: %s\n", strerror(errno));
	    exit(1);
	}
    }

    if (flip) {
	struct sockaddr_storage tmp = sock;
	sock = peer;
	peer = tmp;
    }

#ifndef NO_IPV4
    if (peer.ss_family == AF_INET) {
        struct sockaddr_in *sock4 = (struct sockaddr_in *)&sock;
        struct sockaddr_in *peer4 = (struct sockaddr_in *)&peer;

        assert(peer4->sin_family == AF_INET);

        sprintf(matchbuf, "%08X:%04X %08X:%04X",
                peer4->sin_addr.s_addr, ntohs(peer4->sin_port),
                sock4->sin_addr.s_addr, ntohs(sock4->sin_port));
        filename = "/proc/net/tcp";
        matchcol = 6;
        uidcol = 75;
    } else
#endif
#ifndef NO_IPV6
    if (peer.ss_family == AF_INET6) {
        struct sockaddr_in6 *sock6 = (struct sockaddr_in6 *)&sock;
        struct sockaddr_in6 *peer6 = (struct sockaddr_in6 *)&peer;
        char *p;

        assert(peer6->sin6_family == AF_INET6);

        p = matchbuf;
        for (int i = 0; i < 4; i++)
            p += sprintf(p, "%08X",
                         ((uint32_t *)peer6->sin6_addr.s6_addr)[i]);
        p += sprintf(p, ":%04X ", ntohs(peer6->sin6_port));
        for (int i = 0; i < 4; i++)
            p += sprintf(p, "%08X",
                         ((uint32_t *)sock6->sin6_addr.s6_addr)[i]);
        p += sprintf(p, ":%04X", ntohs(sock6->sin6_port));

        filename = "/proc/net/tcp6";
        matchcol = 6;
        uidcol = 123;
    } else
#endif
    {
        return -1;                     /* unidentified family */
    }

    fp = fopen(filename, "r");
    if (fp) {
	while (fgets(linebuf, sizeof(linebuf), fp)) {
	    if (strlen(linebuf) >= uidcol &&
		!strncmp(linebuf+matchcol, matchbuf, strlen(matchbuf))) {
		fclose(fp);
		return atoi(linebuf + uidcol);
	    }
	}
	fclose(fp);
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

struct listenfds {
    int v4, v6;
};

static int make_listening_sockets(struct listenfds *fds, const char *address,
                                  const char *portstr, char **outhostname)
{
    /*
     * Establish up to 2 listening sockets, for IPv4 and IPv6, on the
     * same arbitrarily selected port. Return them in fds.v4 and
     * fds.v6, with each entry being -1 if that socket was not
     * established at all. Main return value is the port chosen, or <0
     * if the whole process failed.
     */
    struct sockaddr_in6 addr6;
    struct sockaddr_in addr4;
    int got_v6, got_v4;
    socklen_t addrlen;
    int ret, port = 0;

    /*
     * Special case of the address parameter: if it's "0.0.0.0", treat
     * it like NULL, because that was how you specified listen-on-any-
     * address in versions before the IPv6 revamp.
     */
    {
        int u,v,w,x;
        if (address && 
            4 == sscanf(address, "%d.%d.%d.%d", &u, &v, &w, &x) &&
            u==0 && v==0 && w==0 && x==0)
            address = NULL;
    }

    if (portstr && !*portstr)
        portstr = NULL;                /* normalise NULL and empty string */

    if (!address) {
        char hostname[HOST_NAME_MAX];
        if (gethostname(hostname, sizeof(hostname)) < 0) {
            perror("hostname");
            return -1;
        }
        *outhostname = dupstr(hostname);
    } else {
        *outhostname = dupstr(address);
    }

    fds->v6 = fds->v4 = -1;
    got_v6 = got_v4 = 0;

#if defined HAVE_GETADDRINFO

    /*
     * Resolve the given address using getaddrinfo, yielding an IPv6
     * address or an IPv4 one or both.
     */

    struct addrinfo hints;
    struct addrinfo *addrs, *ai;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;
    hints.ai_flags = AI_PASSIVE;
    ret = getaddrinfo(address, portstr, &hints, &addrs);
    if (ret) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }
    for (ai = addrs; ai; ai = ai->ai_next) {
#ifndef NO_IPV6
        if (!got_v6 && ai->ai_family == AF_INET6) {
            memcpy(&addr6, ai->ai_addr, ai->ai_addrlen);
            if (portstr && !port)
                port = ntohs(addr6.sin6_port);
            got_v6 = 1;
        }
#endif
#ifndef NO_IPV4
        if (!got_v4 && ai->ai_family == AF_INET) {
            memcpy(&addr4, ai->ai_addr, ai->ai_addrlen);
            if (portstr && !port)
                port = ntohs(addr4.sin_port);
            got_v4 = 1;
        }
#endif
    }

#elif defined HAVE_GETHOSTBYNAME

    /*
     * IPv4-only setup using inet_addr and gethostbyname.
     */
    struct hostent *h;

    memset(&addr4, 0, sizeof(addr4));
    addr4.sin_family = AF_INET;

    if (!address) {
        addr4.sin_addr.s_addr = htons(INADDR_ANY);
        got_v4 = 1;
    } else if (inet_aton(address, &addr4.sin_addr)) {
        got_v4 = 1;                    /* numeric address */
    } else if ((h = gethostbyname(address)) != NULL) {
        memcpy(&addr4.sin_addr, h->h_addr, sizeof(addr4.sin_addr));
        got_v4 = 1;
    } else {
        fprintf(stderr, "gethostbyname: %s\n", hstrerror(h_errno));
        return -1;
    }

    if (portstr) {
        struct servent *s;
        if (!portstr[strspn(portstr, "0123456789")]) {
            port = atoi(portstr);
        } else if ((s = getservbyname(portstr, NULL)) != NULL) {
            port = ntohs(s->s_port);
        } else {
            fprintf(stderr, "getservbyname: port '%s' not understood\n",
                    portstr);
            return -1;
        }
    }

#endif

#ifndef NO_IPV6
#ifndef NO_IPV4
  retry:
#endif
    if (got_v6) {
        fds->v6 = socket(PF_INET6, SOCK_STREAM, 0);
        if (fds->v6 < 0) {
            fprintf(stderr, "socket(PF_INET6): %s\n", strerror(errno));
            goto done_v6;
        }
#ifdef IPV6_V6ONLY
        {
            int i = 1;
            if (setsockopt(fds->v6, IPPROTO_IPV6, IPV6_V6ONLY,
                           (char *)&i, sizeof(i)) < 0) {
                fprintf(stderr, "setsockopt(IPV6_V6ONLY): %s\n",
                        strerror(errno));
                close(fds->v6);
                fds->v6 = -1;
                goto done_v6;
            }
        }
#endif /* IPV6_V6ONLY */
        addr6.sin6_port = htons(port);
        addrlen = sizeof(addr6);
        if (bind(fds->v6, (const struct sockaddr *)&addr6, addrlen) < 0) {
            fprintf(stderr, "bind: %s\n", strerror(errno));
            close(fds->v6);
            fds->v6 = -1;
            goto done_v6;
        }
        if (listen(fds->v6, 5) < 0) {
            fprintf(stderr, "listen: %s\n", strerror(errno));
            close(fds->v6);
            fds->v6 = -1;
            goto done_v6;
        }
        if (port == 0) {
            addrlen = sizeof(addr6);
            if (getsockname(fds->v6, (struct sockaddr *)&addr6,
                            &addrlen) < 0) {
                fprintf(stderr, "getsockname: %s\n", strerror(errno));
                close(fds->v6);
                fds->v6 = -1;
                goto done_v6;
            }
            port = ntohs(addr6.sin6_port);
        }
    }
  done_v6:
#endif

#ifndef NO_IPV4
    if (got_v4) {
        fds->v4 = socket(PF_INET, SOCK_STREAM, 0);
        if (fds->v4 < 0) {
            fprintf(stderr, "socket(PF_INET): %s\n", strerror(errno));
            goto done_v4;
        }
        addr4.sin_port = htons(port);
        addrlen = sizeof(addr4);
        if (bind(fds->v4, (const struct sockaddr *)&addr4, addrlen) < 0) {
#ifndef NO_IPV6
            if (fds->v6 >= 0) {
                /*
                 * If we support both v6 and v4, it's a failure
                 * condition if we didn't manage to bind to both. If
                 * the port number was arbitrary, we go round and try
                 * again. Otherwise, give up.
                 */
                close(fds->v6);
                close(fds->v4);
                fds->v6 = fds->v4 = -1;
                port = 0;
                if (!portstr)
                    goto retry;
            }
#endif
            fprintf(stderr, "bind: %s\n", strerror(errno));
            close(fds->v4);
            fds->v4 = -1;
            goto done_v4;
        }
        if (listen(fds->v4, 5) < 0) {
            fprintf(stderr, "listen: %s\n", strerror(errno));
            close(fds->v4);
            fds->v4 = -1;
            goto done_v4;
        }
        if (port == 0) {
            addrlen = sizeof(addr4);
            if (getsockname(fds->v4, (struct sockaddr *)&addr4,
                            &addrlen) < 0) {
                fprintf(stderr, "getsockname: %s\n", strerror(errno));
                close(fds->v4);
                fds->v4 = -1;
                goto done_v4;
            }
            port = ntohs(addr4.sin_port);
        }
    }
  done_v4:
#endif

    if (fds->v6 >= 0 || fds->v4 >= 0)
        return port;
    else
        return -1;
}

void run_httpd(const void *t, int authmask, const struct httpd_config *dcfg,
	       const struct html_config *incfg)
{
    struct listenfds lfds;
    int ret, port;
    int authtype;
    char *authstring = NULL;
    char *hostname;
    const char *openbracket, *closebracket;
    struct sockaddr_in addr;
    socklen_t addrlen;
    struct html_config cfg = *incfg;

    /*
     * Establish the listening socket(s) and retrieve its port
     * number.
     */
    port = make_listening_sockets(&lfds, dcfg->address, dcfg->port, &hostname);
    if (port < 0)
        exit(1);                       /* already reported an error */

    if ((authmask & HTTPD_AUTH_MAGIC) &&
	(lfds.v4 < 0 || check_owning_uid(lfds.v4, 1) == getuid()) &&
        (lfds.v6 < 0 || check_owning_uid(lfds.v6, 1) == getuid())) {
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
	    strcpy(username, PNAME);
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
	fprintf(stderr, PNAME ": authentication method not supported\n");
	exit(1);
    }
    if (strchr(hostname, ':')) {
        /* If the hostname is an IPv6 address literal, enclose it in
         * square brackets to prevent misinterpretation of the
         * colons. */
        openbracket = "[";
        closebracket = "]";
    } else {
        openbracket = closebracket = "";
    }
    if (port == 80) {
	printf("URL: http://%s%s%s/\n", openbracket, hostname, closebracket);
    } else {
	printf("URL: http://%s%s%s:%d/\n", openbracket, hostname, closebracket,
               port);
    }
    fflush(stdout);

    /*
     * Now construct fd structure(s) to hold the listening sockets.
     */
    if (lfds.v4 >= 0)
        new_fdstruct(lfds.v4, FD_LISTENER);
    if (lfds.v6 >= 0)
        new_fdstruct(lfds.v6, FD_LISTENER);

    if (dcfg->closeoneof) {
        /*
         * Read from standard input, and treat EOF as a notification
         * to exit.
         */
       new_fdstruct(0, FD_CLIENT);
    }

    /*
     * Now we're ready to run our main loop. Keep looping round on
     * select.
     */
    while (1) {
	fd_set rfds, wfds;
	int i, j;
	SELECT_TYPE_ARG1 maxfd;
	int ret;

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
		FD_SET_MAX(fds[i].fd, &rfds, maxfd);
		break;
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

        ret = select(maxfd, SELECT_TYPE_ARG234 &rfds,
		     SELECT_TYPE_ARG234 &wfds, SELECT_TYPE_ARG234 NULL,
		     SELECT_TYPE_ARG5 NULL);
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
