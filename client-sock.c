/******************************************************************************\
 *  $Id: client-sock.c,v 1.10 2001/05/31 18:22:41 dun Exp $
 *    by Chris Dunlap <cdunlap@llnl.gov>
\******************************************************************************/


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#include "common.h"
#include "client.h"
#include "errors.h"
#include "lex.h"
#include "util.h"


static void parse_rsp_ok(Lex l, client_conf_t *conf);
static void parse_rsp_err(Lex l, client_conf_t *conf);


void connect_to_server(client_conf_t *conf)
{
    int sd;
    struct sockaddr_in addr;
    struct hostent *hostp;

    assert(conf->req->host);
    assert(conf->req->port > 0);

    if ((sd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        err_msg(errno, "socket() failed");

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(conf->req->port);

    /*  Note: gethostbyname() is not thread-safe, but that's OK here.
     */
    if (!(hostp = gethostbyname(conf->req->host)))
        err_msg(0, "Unable to resolve host [%s]: %s",
            conf->req->host, hstrerror(h_errno));
    memcpy(&addr.sin_addr.s_addr, hostp->h_addr, hostp->h_length);

    if (connect(sd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
        err_msg(errno, "Unable to connect to [%s:%d]",
            conf->req->host, conf->req->port);

    conf->req->sd = sd;
    return;
}


int send_greeting(client_conf_t *conf)
{
    char buf[MAX_SOCK_LINE];
    int n;

    assert(conf->req->sd >= 0);
    assert(conf->req->user && *conf->req->user);
    assert(conf->req->tty && *conf->req->tty);

    n = snprintf(buf, sizeof(buf), "%s %s='%s' %s='%s'\n",
        proto_strs[LEX_UNTOK(CONMAN_TOK_HELLO)],
        proto_strs[LEX_UNTOK(CONMAN_TOK_USER)], lex_encode(conf->req->user),
        proto_strs[LEX_UNTOK(CONMAN_TOK_TTY)], lex_encode(conf->req->tty));

    if (n < 0 || n >= sizeof(buf)) {
        conf->errnum = CONMAN_ERR_LOCAL;
        conf->errmsg = create_string("Buffer overflow during send_greeting()");
        return(-1);
    }

    if (write_n(conf->req->sd, buf, strlen(buf)) < 0) {
        conf->errnum = CONMAN_ERR_LOCAL;
        conf->errmsg = create_fmt_string("Cannot send greeting to [%s:%d]: %s",
            conf->req->host, conf->req->port, strerror(errno));
        return(-1);
    }

    if (recv_rsp(conf) < 0) {
        if (conf->errnum == CONMAN_ERR_AUTHENTICATE) {
            /*
             *  FIX_ME: NOT_IMPLEMENTED_YET
             */
            if (close(conf->req->sd) < 0)
                err_msg(errno, "close(%d) failed", conf->req->sd);
            conf->req->sd = -1;
        }
        return(-1);
    }
    return(0);
}


int send_req(client_conf_t *conf)
{
    char buf[MAX_SOCK_LINE];
    int n;
    char *ptr;
    int len;
    char *cmd;
    char *str;

    assert(conf->req->sd >= 0);

    ptr = buf;
    len = sizeof(buf) - 1;		/* reserve space for terminating \n */

    switch(conf->req->command) {
    case QUERY:
        cmd = proto_strs[LEX_UNTOK(CONMAN_TOK_QUERY)];
        break;
    case MONITOR:
        cmd = proto_strs[LEX_UNTOK(CONMAN_TOK_MONITOR)];
        break;
    case CONNECT:
        cmd = proto_strs[LEX_UNTOK(CONMAN_TOK_CONNECT)];
        break;
    case EXECUTE:
        cmd = proto_strs[LEX_UNTOK(CONMAN_TOK_EXECUTE)];
        break;
    default:
        err_msg(0, "Invalid command (%d) at %s:%d",
            conf->req->command, __FILE__, __LINE__);
        break;
    }

    n = snprintf(ptr, len, "%s", cmd);
    if (n < 0 || n >= len)
        goto overflow;
    ptr += n;
    len -= n;

    if ((conf->req->command == EXECUTE) && (conf->req->program != NULL)) {
        n = snprintf(ptr, len, " %s='%s'",
            proto_strs[LEX_UNTOK(CONMAN_TOK_PROGRAM)],
            lex_encode(conf->req->program));
        if (n < 0 || n >= len)
            goto overflow;
        ptr += n;
        len -= n;
    }

    if ((conf->req->command == CONNECT) || (conf->req->command == EXECUTE)) {
        if (conf->req->enableForce) {
            n = snprintf(ptr, len, " %s=%s",
                proto_strs[LEX_UNTOK(CONMAN_TOK_OPTION)],
                proto_strs[LEX_UNTOK(CONMAN_TOK_FORCE)]);
            if (n < 0 || n >= len)
                goto overflow;
            ptr += n;
            len -= n;
        }
        if (conf->req->enableJoin) {
            n = snprintf(ptr, len, " %s=%s",
                proto_strs[LEX_UNTOK(CONMAN_TOK_OPTION)],
                proto_strs[LEX_UNTOK(CONMAN_TOK_JOIN)]);
            if (n < 0 || n >= len)
                goto overflow;
            ptr += n;
            len -= n;
        }
        if (conf->req->enableBroadcast) {
            n = snprintf(ptr, len, " %s=%s",
                proto_strs[LEX_UNTOK(CONMAN_TOK_OPTION)],
                proto_strs[LEX_UNTOK(CONMAN_TOK_BROADCAST)]);
            if (n < 0 || n >= len)
                goto overflow;
            ptr += n;
            len -= n;
        }
    }

    /*  Empty the consoles list here because it will be filled in
     *    with the actual console names in recv_rsp().
     */
    while ((str = list_pop(conf->req->consoles))) {
        n = snprintf(ptr, len, " %s='%s'",
            proto_strs[LEX_UNTOK(CONMAN_TOK_CONSOLE)], lex_encode(str));
        free(str);
        if (n < 0 || n >= len) {
            n = -1;
            break;
        }
        ptr += n;
        len -= n;
    }
    if (n < 0)
        goto overflow;

    *ptr++ = '\n';
    *ptr++ = '\0';

    if (write_n(conf->req->sd, buf, strlen(buf)) < 0) {
        conf->errnum = CONMAN_ERR_LOCAL;
        conf->errmsg = create_fmt_string(
            "Unable to send greeting to [%s:%d]: %s",
            conf->req->host, conf->req->port, strerror(errno));
        return(-1);
    }

    /*  For both QUERY and EXECUTE commands, the write-half of the
     *    socket connection can be closed once the request is sent.
     */
    if ((conf->req->command == QUERY) || (conf->req->command == EXECUTE)) {
        if (shutdown(conf->req->sd, SHUT_WR) < 0) {
            conf->errnum = CONMAN_ERR_LOCAL;
            conf->errmsg = create_fmt_string(
                "Unable to close write-half of connection to [%s:%d]: %s",
                conf->req->host, conf->req->port, strerror(errno));
            return(-1);
        }
    }

    return(0);

overflow:
    conf->errnum = CONMAN_ERR_LOCAL;
    conf->errmsg = create_string("Overran request buffer");
    return(-1);
}


int recv_rsp(client_conf_t *conf)
{
    char buf[MAX_SOCK_LINE];
    int n;
    Lex l;
    int done = 0;
    int tok;

    assert(conf->req->sd >= 0);

    if ((n = read_line(conf->req->sd, buf, sizeof(buf))) < 0) {
        conf->errnum = CONMAN_ERR_LOCAL;
        conf->errmsg = create_fmt_string(
            "Cannot read response from [%s:%d]: %s",
            conf->req->host, conf->req->port, strerror(errno));
        return(-1);
    }
    else if (n == 0) {
        conf->errnum = CONMAN_ERR_LOCAL;
        conf->errmsg = create_fmt_string("Connection terminated by [%s:%d]",
            conf->req->host, conf->req->port);
        return(-1);
    }

    if (!(l = lex_create(buf, proto_strs)))
        err_msg(0, "Out of memory");

    while (!done) {
        tok = lex_next(l);
        switch(tok) {
        case CONMAN_TOK_OK:		/* OK, so ignore rest of line */
            parse_rsp_ok(l, conf);
            done = 1;
            break;
        case CONMAN_TOK_ERROR:
            parse_rsp_err(l, conf);
            done = -1;
            break;
        case LEX_EOF:
        case LEX_EOL:
            done = -1;
            break;
        default:			/* ignore unrecognized tokens */
            break;
        }
    }

    lex_destroy(l);

    if (done == 1)
        return(0);
    if (conf->errnum == CONMAN_ERR_NONE) {
        conf->errnum = CONMAN_ERR_LOCAL;
        conf->errmsg = create_fmt_string("Received invalid reponse from"
            " [%s:%d]", conf->req->host, conf->req->port);
    }
    return(-1);
}


static void parse_rsp_ok(Lex l, client_conf_t *conf)
{
    int tok;
    int done = 0;
    char *str;

    while (!done) {
        tok = lex_next(l);
        switch (tok) {
        case CONMAN_TOK_CONSOLE:
            if ((lex_next(l) == '=') && (lex_next(l) == LEX_STR)) {
                if ((str = lex_decode(create_string(lex_text(l)))))
                    if (!list_append(conf->req->consoles, str))
                        err_msg(0, "Out of memory");
            }
            break;
        case LEX_EOF:
        case LEX_EOL:
            done = 1;
            break;
        default:
            break;			/* ignore unrecognized tokens */
        }
    }
    return;
}


static void parse_rsp_err(Lex l, client_conf_t *conf)
{
    int tok;
    int done = 0;
    int err = 0;
    char buf[MAX_LINE] = "";

    while (!done) {
        tok = lex_next(l);
        switch (tok) {
        case CONMAN_TOK_CODE:
            if ((lex_next(l) == '=') && (lex_next(l) == LEX_INT))
                err = atoi(lex_text(l));
            break;
        case CONMAN_TOK_MESSAGE:
            if ((lex_next(l) == '=') && (lex_next(l) == LEX_STR))
                strlcpy(buf, lex_text(l), sizeof(buf));
            break;
        case LEX_EOF:
        case LEX_EOL:
            done = 1;
            break;
        default:
            break;			/* ignore unrecognized tokens */
        }
    }
    conf->errnum = err;
    if (*buf)
        conf->errmsg = lex_decode(create_string(buf));
    return;
}


void display_error(client_conf_t *conf)
{
    char *p;

    assert(conf->errnum > 0);

    p = create_fmt_string("ERROR: %s\n\n",
        (conf->errmsg ? conf->errmsg : "Unspecified"));
    if (write_n(STDERR_FILENO, p, strlen(p)) < 0)
        err_msg(errno, "write(%d) failed", STDERR_FILENO);
    if (conf->logd >= 0)
        if (write_n(conf->logd, p, strlen(p)) < 0)
            err_msg(errno, "write(%d) failed", conf->logd);
    free(p);

    if (conf->errnum != CONMAN_ERR_LOCAL)
        display_data(conf, STDERR_FILENO);

    if ((conf->errnum == CONMAN_ERR_TOO_MANY_CONSOLES)
      && (!conf->req->enableBroadcast))
        p = "\nDo you want to broadcast (option -b)?\n\n";
    else if ((conf->errnum == CONMAN_ERR_BUSY_CONSOLES)
      && ((!conf->req->enableForce) && (!conf->req->enableJoin)))
        p = "\nDo you want to force (option -f) or join (option -j)"
          " the connection?\n\n";
    else
        p = NULL;

    if (p) {
        if (write_n(STDERR_FILENO, p, strlen(p)) < 0)
            err_msg(errno, "write(%d) failed", STDERR_FILENO);
        if (conf->logd >= 0)
            if (write_n(conf->logd, p, strlen(p)) < 0)
                err_msg(errno, "write(%d) failed", conf->logd);
    }

    exit(2);
}


void display_data(client_conf_t *conf, int fd)
{
    char buf[MAX_BUF_SIZE];
    int n;

    assert(fd >= 0);

    if (conf->req->sd < 0)
        return;

    for (;;) {
        n = read(conf->req->sd, buf, sizeof(buf));
        if (n < 0)
            err_msg(errno, "Unable to read data from socket");
        if (n == 0)
            break;
        if (write_n(fd, buf, n) < 0)
            err_msg(errno, "write(%d) failed", fd);
        if (conf->logd >= 0)
            if (write_n(conf->logd, buf, n) < 0)
                err_msg(errno, "write(%d) failed", conf->logd);
    }
    return;
}


void display_consoles(client_conf_t *conf, int fd)
{
    ListIterator i;
    char *p;
    char buf[MAX_LINE];
    int n;

    if (!(i = list_iterator_create(conf->req->consoles)))
        err_msg(0, "Out of memory");
    while ((p = list_next(i))) {
        n = snprintf(buf, sizeof(buf), "%s\n", p);
        if (n < 0 || n >= sizeof(buf))
            err_msg(0, "Buffer overflow");
        if (write_n(fd, buf, n) < 0)
            err_msg(errno, "write(%d) failed", fd);
        if (conf->logd >= 0)
            if (write_n(conf->logd, buf, n) < 0)
                err_msg(errno, "write(%d) failed", conf->logd);
    }
    list_iterator_destroy(i);
    return;
}
