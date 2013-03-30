/*	$OpenBSD: getaddrinfo_async.c,v 1.12 2013/03/28 11:47:23 eric Exp $	*/
/*
 * Copyright (c) 2012 Eric Faurot <eric@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "includes.h"

#include <sys/types.h>
#include <sys/uio.h>
#include <arpa/nameser.h>
#ifdef YP
#include <rpc/rpc.h>
#include <rpcsvc/yp.h>
#include <rpcsvc/ypclnt.h>
#include "ypinternal.h"
#endif

#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "asr.h"
#include "asr_private.h"

struct match {
	int family;
	int socktype;
	int protocol;
};

static int getaddrinfo_async_run(struct async *, struct async_res *);
static int get_port(const char *, const char *, int);
static int iter_family(struct async *, int);
static int addrinfo_add(struct async *, const struct sockaddr *, const char *);
static int addrinfo_from_file(struct async *, int,  FILE *);
static int addrinfo_from_pkt(struct async *, char *, size_t);
#ifdef YP
static int addrinfo_from_yp(struct async *, int, char *);
#endif

static const struct match matches[] = {
	{ PF_INET,	SOCK_DGRAM,	IPPROTO_UDP	},
	{ PF_INET,	SOCK_STREAM,	IPPROTO_TCP	},
	{ PF_INET,	SOCK_RAW,	0		},
	{ PF_INET6,	SOCK_DGRAM,	IPPROTO_UDP	},
	{ PF_INET6,	SOCK_STREAM,	IPPROTO_TCP	},
	{ PF_INET6,	SOCK_RAW,	0		},
	{ -1,		0,		0,		},
};

#define MATCH_FAMILY(a, b) ((a) == matches[(b)].family || (a) == PF_UNSPEC)
#define MATCH_PROTO(a, b) ((a) == matches[(b)].protocol || (a) == 0 || matches[(b)].protocol == 0)
/* Do not match SOCK_RAW unless explicitely specified */
#define MATCH_SOCKTYPE(a, b) ((a) == matches[(b)].socktype || ((a) == 0 && \
				matches[(b)].socktype != SOCK_RAW))

struct async *
getaddrinfo_async(const char *hostname, const char *servname,
	const struct addrinfo *hints, struct asr *asr)
{
	struct asr_ctx	*ac;
	struct async	*as;

	ac = asr_use_resolver(asr);
	if ((as = async_new(ac, ASR_GETADDRINFO)) == NULL)
		goto abort; /* errno set */
	as->as_run = getaddrinfo_async_run;

	if (hostname && (as->as.ai.hostname = strdup(hostname)) == NULL)
		goto abort; /* errno set */
	if (servname && (as->as.ai.servname = strdup(servname)) == NULL)
		goto abort; /* errno set */
	if (hints)
		memmove(&as->as.ai.hints, hints, sizeof *hints);
	else {
		memset(&as->as.ai.hints, 0, sizeof as->as.ai.hints);
		as->as.ai.hints.ai_family = PF_UNSPEC;
	}

	asr_ctx_unref(ac);
	return (as);
    abort:
	if (as)
		async_free(as);
	asr_ctx_unref(ac);
	return (NULL);
}

static int
getaddrinfo_async_run(struct async *as, struct async_res *ar)
{
#ifdef YP
	static char	*domain = NULL;
	char		*res;
	int		 len;
#endif
	const char	*str;
	struct addrinfo	*ai;
	int		 i, family, r;
	FILE		*f;
	union {
		struct sockaddr		sa;
		struct sockaddr_in	sain;
		struct sockaddr_in6	sain6;
	} sa;

    next:
	switch (as->as_state) {

	case ASR_STATE_INIT:

		/*
		 * First, make sure the parameters are valid.
		 */

		as->as_count = 0;

		if (as->as.ai.hostname == NULL &&
		    as->as.ai.servname == NULL) {
			ar->ar_gai_errno = EAI_NONAME;
			async_set_state(as, ASR_STATE_HALT);
			break;
		}

		ai = &as->as.ai.hints;

#ifdef EAI_BADHINTS
		if (ai->ai_addrlen ||
		    ai->ai_canonname ||
		    ai->ai_addr ||
		    ai->ai_next) {
			ar->ar_gai_errno = EAI_BADHINTS;
			async_set_state(as, ASR_STATE_HALT);
			break;
		}
#endif

		if (ai->ai_flags & ~AI_MASK ||
		    (ai->ai_flags & AI_CANONNAME && ai->ai_flags & AI_FQDN)) {
			ar->ar_gai_errno = EAI_BADFLAGS;
			async_set_state(as, ASR_STATE_HALT);
			break;
		}

		if (ai->ai_family != PF_UNSPEC &&
		    ai->ai_family != PF_INET &&
		    ai->ai_family != PF_INET6) {
			ar->ar_gai_errno = EAI_FAMILY;
			async_set_state(as, ASR_STATE_HALT);
			break;
		}

		if (ai->ai_socktype &&
		    ai->ai_socktype != SOCK_DGRAM  &&
		    ai->ai_socktype != SOCK_STREAM &&
		    ai->ai_socktype != SOCK_RAW) {
			ar->ar_gai_errno = EAI_SOCKTYPE;
			async_set_state(as, ASR_STATE_HALT);
			break;
		}

		if (ai->ai_socktype == SOCK_RAW &&
		    as->as.ai.servname != NULL) {
			ar->ar_gai_errno = EAI_SERVICE;
			async_set_state(as, ASR_STATE_HALT);
			break;
		}

		/* Make sure there is at least a valid combination */
		for (i = 0; matches[i].family != -1; i++)
			if (MATCH_FAMILY(ai->ai_family, i) &&
			    MATCH_SOCKTYPE(ai->ai_socktype, i) &&
			    MATCH_PROTO(ai->ai_protocol, i))
				break;
		if (matches[i].family == -1) {
#ifdef EAI_BADHINTS
			ar->ar_gai_errno = EAI_BADHINTS;
#else
			ar->ar_gai_errno = EAI_FAIL;
#endif
			async_set_state(as, ASR_STATE_HALT);
			break;
		}

		if (ai->ai_protocol == 0 || ai->ai_protocol == IPPROTO_UDP)
			as->as.ai.port_udp = get_port(as->as.ai.servname, "udp",
			    as->as.ai.hints.ai_flags & AI_NUMERICSERV);
		if (ai->ai_protocol == 0 || ai->ai_protocol == IPPROTO_TCP)
			as->as.ai.port_tcp = get_port(as->as.ai.servname, "tcp",
			    as->as.ai.hints.ai_flags & AI_NUMERICSERV);
		if (as->as.ai.port_tcp == -2 || as->as.ai.port_udp == -2 ||
		    (as->as.ai.port_tcp == -1 && as->as.ai.port_udp == -1) ||
		    (ai->ai_protocol && (as->as.ai.port_udp == -1 ||
					 as->as.ai.port_tcp == -1))) {
			ar->ar_gai_errno = EAI_SERVICE;
			async_set_state(as, ASR_STATE_HALT);
			break;
		}

		ar->ar_gai_errno = 0;

		/* If hostname is NULL, use local address */
		if (as->as.ai.hostname == NULL) {
			for (family = iter_family(as, 1);
			    family != -1;
			    family = iter_family(as, 0)) {
				/*
				 * We could use statically built sockaddrs for
				 * those, rather than parsing over and over.
				 */
				if (family == PF_INET)
					str = (ai->ai_flags & AI_PASSIVE) ? \
						"0.0.0.0" : "127.0.0.1";
				else /* PF_INET6 */
					str = (ai->ai_flags & AI_PASSIVE) ? \
						"::" : "::1";
				 /* This can't fail */
				sockaddr_from_str(&sa.sa, family, str);
				if ((r = addrinfo_add(as, &sa.sa, NULL))) {
					ar->ar_gai_errno = r;
					break;
				}
			}
			if (ar->ar_gai_errno == 0 && as->as_count == 0) {
				ar->ar_gai_errno = EAI_NODATA;
			}
			async_set_state(as, ASR_STATE_HALT);
			break;
		}

		/* Try numeric addresses first */
		for (family = iter_family(as, 1);
		    family != -1;
		    family = iter_family(as, 0)) {

			if (sockaddr_from_str(&sa.sa, family,
			    as->as.ai.hostname) == -1)
				continue;

			if ((r = addrinfo_add(as, &sa.sa, NULL)))
				ar->ar_gai_errno = r;
			break;
		}
		if (ar->ar_gai_errno || as->as_count) {
			async_set_state(as, ASR_STATE_HALT);
			break;
		}

		if (ai->ai_flags & AI_NUMERICHOST) {
			ar->ar_gai_errno = EAI_FAIL;
			async_set_state(as, ASR_STATE_HALT);
			break;
		}

		async_set_state(as, ASR_STATE_NEXT_DB);
		break;

	case ASR_STATE_NEXT_DB:
		if (asr_iter_db(as) == -1) {
			async_set_state(as, ASR_STATE_NOT_FOUND);
			break;
		}
		as->as_family_idx = 0;
		async_set_state(as, ASR_STATE_SAME_DB);
		break;

	case ASR_STATE_NEXT_FAMILY:
		as->as_family_idx += 1;
		if (as->as.ai.hints.ai_family != AF_UNSPEC ||
		    AS_FAMILY(as) == -1) {
			/* The family was specified, or we have tried all
			 * families with this DB.
			 */
			if (as->as_count) {
				ar->ar_gai_errno = 0;
				async_set_state(as, ASR_STATE_HALT);
			} else
				async_set_state(as, ASR_STATE_NEXT_DB);
			break;
		}
		async_set_state(as, ASR_STATE_SAME_DB);
		break;

	case ASR_STATE_SAME_DB:
		/* query the current DB again */
		switch (AS_DB(as)) {
		case ASR_DB_DNS:
			family = (as->as.ai.hints.ai_family == AF_UNSPEC) ?
			    AS_FAMILY(as) : as->as.ai.hints.ai_family;
			if (as->as.ai.fqdn) {
				as->as.ai.subq = res_query_async_ctx(
				    as->as.ai.fqdn, C_IN,
				    (family == AF_INET6) ? T_AAAA : T_A, NULL, 0,
				    as->as_ctx);
			}
			else {
				as->as.ai.subq = res_search_async_ctx(
				    as->as.ai.hostname, C_IN,
				    (family == AF_INET6) ? T_AAAA : T_A, NULL, 0,
				    as->as_ctx);
			}
			if (as->as.ai.subq == NULL) {
				if (errno == ENOMEM)
					ar->ar_gai_errno = EAI_MEMORY;
				else
					ar->ar_gai_errno = EAI_FAIL;
				async_set_state(as, ASR_STATE_HALT);
				break;
			}
			async_set_state(as, ASR_STATE_SUBQUERY);
			break;

		case ASR_DB_FILE:
			f = fopen(as->as_ctx->ac_hostfile, "r");
			if (f == NULL) {
				async_set_state(as, ASR_STATE_NEXT_DB);
				break;
			}
			family = (as->as.ai.hints.ai_family == AF_UNSPEC) ?
			    AS_FAMILY(as) : as->as.ai.hints.ai_family;

			r = addrinfo_from_file(as, family, f);
			if (r == -1) {
				if (errno == ENOMEM)
					ar->ar_gai_errno = EAI_MEMORY;
				else
					ar->ar_gai_errno = EAI_FAIL;
				async_set_state(as, ASR_STATE_HALT);
			} else
				async_set_state(as, ASR_STATE_NEXT_FAMILY);
			fclose(f);
			break;

#ifdef YP
		case ASR_DB_YP:
			if (!domain && _yp_check(&domain) == 0) {
				async_set_state(as, ASR_STATE_NEXT_DB);
				break;
			}
			family = (as->as.ai.hints.ai_family == AF_UNSPEC) ?
			    AS_FAMILY(as) : as->as.ai.hints.ai_family;
			/* XXX
			 * ipnodes.byname could also contain IPv4 address
			 */
			r = yp_match(domain, (family == AF_INET6) ?
			    "ipnodes.byname" : "hosts.byname",
			    as->as.ai.hostname, strlen(as->as.ai.hostname),
			    &res, &len);
			if (r == 0) {
				r = addrinfo_from_yp(as, family, res);
				free(res);
				if (r == -1) {
					if (errno == ENOMEM)
						ar->ar_gai_errno = EAI_MEMORY;
					else
						ar->ar_gai_errno = EAI_FAIL;
					async_set_state(as, ASR_STATE_HALT);
					break;
				}
			}
			async_set_state(as, ASR_STATE_NEXT_FAMILY);
			break;
#endif
		default:
			async_set_state(as, ASR_STATE_NEXT_DB);
		}
		break;

	case ASR_STATE_SUBQUERY:
		if ((r = async_run(as->as.ai.subq, ar)) == ASYNC_COND)
			return (ASYNC_COND);
		as->as.ai.subq = NULL;

		if (ar->ar_datalen == -1) {
			async_set_state(as, ASR_STATE_NEXT_FAMILY);
			break;
		}

		r = addrinfo_from_pkt(as, ar->ar_data, ar->ar_datalen);
		if (r == -1) {
			if (errno == ENOMEM)
				ar->ar_gai_errno = EAI_MEMORY;
			else
				ar->ar_gai_errno = EAI_FAIL;
			async_set_state(as, ASR_STATE_HALT);
		} else
			async_set_state(as, ASR_STATE_NEXT_FAMILY);
		free(ar->ar_data);
		break;

	case ASR_STATE_NOT_FOUND:
		/* No result found. Maybe we can try again. */
		if (as->as.ai.flags & ASYNC_AGAIN)
			ar->ar_gai_errno = EAI_AGAIN;
		else
			ar->ar_gai_errno = EAI_NODATA;
		async_set_state(as, ASR_STATE_HALT);
		break;

	case ASR_STATE_HALT:
		if (ar->ar_gai_errno == 0) {
			ar->ar_count = as->as_count;
			ar->ar_addrinfo = as->as.ai.aifirst;
			as->as.ai.aifirst = NULL;
		} else {
			ar->ar_count = 0;
			ar->ar_addrinfo = NULL;
		}
		return (ASYNC_DONE);

	default:
		ar->ar_errno = EOPNOTSUPP;
		ar->ar_gai_errno = EAI_SYSTEM;
		async_set_state(as, ASR_STATE_HALT);
		break;
	}
	goto next;
}

/*
 * Retreive the port number for the service name "servname" and
 * the protocol "proto".
 */
static int
get_port(const char *servname, const char *proto, int numonly)
{
	struct servent		se;
#ifdef HAVE_STRUCT_SERVENT_DATA
	struct servent_data     sed;
#endif
	int			port, r;
	const char*		e;

	if (servname == NULL)
		return (0);

	e = NULL;
	port = strtonum(servname, 0, USHRT_MAX, &e);
	if (e == NULL)
		return (port);
	if (errno == ERANGE)
		return (-2); /* invalid */
	if (numonly)
		return (-2);

#ifdef HAVE_STRUCT_SERVENT_DATA
	memset(&sed, 0, sizeof(sed));
#endif
#ifdef HAVE_GETSERVBYNAME_R_4_ARGS
	r = getservbyname_r(servname, proto, &se, &sed);
#else
	r = -1;
#endif
	port = ntohs(se.s_port);
#ifdef HAVE_ENDSERVENT_R
	endservent_r(&sed);
#endif

	if (r == -1)
		return (-1); /* not found */

	return (port);
}

/*
 * Iterate over the address families that are to be queried. Use the
 * list on the async context, unless a specific family was given in hints.
 */
static int
iter_family(struct async *as, int first)
{
	if (first) {
		as->as_family_idx = 0;
		if (as->as.ai.hints.ai_family != PF_UNSPEC)
			return as->as.ai.hints.ai_family;
		return AS_FAMILY(as);
	}

	if (as->as.ai.hints.ai_family != PF_UNSPEC)
		return (-1);

	as->as_family_idx++;

	return AS_FAMILY(as);
}

/*
 * Use the sockaddr at "sa" to extend the result list on the "as" context,
 * with the specified canonical name "cname". This function adds one
 * entry per protocol/socktype match.
 */
static int
addrinfo_add(struct async *as, const struct sockaddr *sa, const char *cname)
{
	struct addrinfo		*ai;
	int			 i, port, proto;

	for (i = 0; matches[i].family != -1; i++) {
		if (matches[i].family != sa->sa_family ||
		    !MATCH_SOCKTYPE(as->as.ai.hints.ai_socktype, i) ||
		    !MATCH_PROTO(as->as.ai.hints.ai_protocol, i))
			continue;

		proto = as->as.ai.hints.ai_protocol;
		if (!proto)
			proto = matches[i].protocol;

		if (proto == IPPROTO_TCP)
			port = as->as.ai.port_tcp;
		else if (proto == IPPROTO_UDP)
			port = as->as.ai.port_udp;
		else
			port = 0;

		/* servname specified, but not defined for this protocol */
		if (port == -1)
			continue;

		ai = calloc(1, sizeof(*ai) + SA_LEN(sa));
		if (ai == NULL)
			return (EAI_MEMORY);
		ai->ai_family = sa->sa_family;
		ai->ai_socktype = matches[i].socktype;
		ai->ai_protocol = proto;
		ai->ai_addrlen = SA_LEN(sa);
		ai->ai_addr = (void*)(ai + 1);
		if (cname &&
		    as->as.ai.hints.ai_flags & (AI_CANONNAME | AI_FQDN)) {
			if ((ai->ai_canonname = strdup(cname)) == NULL) {
				free(ai);
				return (EAI_MEMORY);
			}
		}
		memmove(ai->ai_addr, sa, SA_LEN(sa));
		if (sa->sa_family == PF_INET)
			((struct sockaddr_in *)ai->ai_addr)->sin_port =
			    htons(port);
		else if (sa->sa_family == PF_INET6)
			((struct sockaddr_in6 *)ai->ai_addr)->sin6_port =
			    htons(port);

		if (as->as.ai.aifirst == NULL)
			as->as.ai.aifirst = ai;
		if (as->as.ai.ailast)
			as->as.ai.ailast->ai_next = ai;
		as->as.ai.ailast = ai;
		as->as_count += 1;
	}

	return (0);
}

void
asr_freeaddrinfo(struct addrinfo *ai)
{
	struct addrinfo	*ai_next;

	while (ai) {
		ai_next = ai->ai_next;
		if (ai->ai_canonname)
			free(ai->ai_canonname);
		free(ai);
		ai = ai_next;
	}
}

static int
addrinfo_from_file(struct async *as, int family, FILE *f)
{
	char		*tokens[MAXTOKEN], *c;
	int		 n, i;
	union {
		struct sockaddr		sa;
		struct sockaddr_in	sain;
		struct sockaddr_in6	sain6;
	} u;

	for (;;) {
		n = asr_parse_namedb_line(f, tokens, MAXTOKEN);
		if (n == -1)
			break; /* ignore errors reading the file */

		for (i = 1; i < n; i++) {
			if (strcasecmp(as->as.ai.hostname, tokens[i]))
				continue;
			if (sockaddr_from_str(&u.sa, family, tokens[0]) == -1)
				continue;
			break;
		}
		if (i == n)
			continue;

		if (as->as.ai.hints.ai_flags & (AI_CANONNAME | AI_FQDN))
			c = tokens[1];
		else
			c = NULL;

		if (addrinfo_add(as, &u.sa, c))
			return (-1); /* errno set */
	}
	return (0);
}

static int
addrinfo_from_pkt(struct async *as, char *pkt, size_t pktlen)
{
	struct unpack	 p;
	struct header	 h;
	struct query	 q;
	struct rr	 rr;
	int		 i;
	union {
		struct sockaddr		sa;
		struct sockaddr_in	sain;
		struct sockaddr_in6	sain6;
	} u;
	char		 buf[MAXDNAME], *c;

	unpack_init(&p, pkt, pktlen);
	unpack_header(&p, &h);
	for (; h.qdcount; h.qdcount--)
		unpack_query(&p, &q);

	for (i = 0; i < h.ancount; i++) {
		unpack_rr(&p, &rr);
		if (rr.rr_type != q.q_type ||
		    rr.rr_class != q.q_class)
			continue;

		if (as->as.ai.fqdn == NULL) {
			asr_strdname(q.q_dname, buf, sizeof buf);
			buf[strlen(buf) - 1] = '\0';
			as->as.ai.fqdn = strdup(buf);
			if (as->as.ai.fqdn == NULL)
				return (-1); /* errno set */
		}

		memset(&u, 0, sizeof u);
		if (rr.rr_type == T_A) {
#ifdef HAVE_STRUCT_SOCKADDR_IN_SIN_LEN
			u.sain.sin_len = sizeof u.sain;
#endif
			u.sain.sin_family = AF_INET;
			u.sain.sin_addr = rr.rr.in_a.addr;
			u.sain.sin_port = 0;
		} else if (rr.rr_type == T_AAAA) {
#ifdef HAVE_STRUCT_SOCKADDR_IN6_SIN6_LEN
			u.sain6.sin6_len = sizeof u.sain6;
#endif
			u.sain6.sin6_family = AF_INET6;
			u.sain6.sin6_addr = rr.rr.in_aaaa.addr6;
			u.sain6.sin6_port = 0;
		} else
			continue;

		if (as->as.ai.hints.ai_flags & AI_CANONNAME) {
			asr_strdname(rr.rr_dname, buf, sizeof buf);
			buf[strlen(buf) - 1] = '\0';
			c = buf;
		} else if (as->as.ai.hints.ai_flags & AI_FQDN)
			c = as->as.ai.fqdn;
		else
			c = NULL;

		if (addrinfo_add(as, &u.sa, c))
			return (-1); /* errno set */
	}
	return (0);
}

#ifdef YP
static int
strsplit(char *line, char **tokens, int ntokens)
{
	int	ntok;
	char	*cp, **tp;

	for (cp = line, tp = tokens, ntok = 0;
	    ntok < ntokens && (*tp = strsep(&cp, " \t")) != NULL; )
		if (**tp != '\0') {
			tp++;
			ntok++;
		}

	return (ntok);
}

static int
addrinfo_from_yp(struct async *as, int family, char *line)
{
	char		*next, *tokens[MAXTOKEN], *c;
	int		 ntok;
	union {
		struct sockaddr		sa;
		struct sockaddr_in	sain;
		struct sockaddr_in6	sain6;
	} u;

	for (next = line; line; line = next) {
		if ((next = strchr(line, '\n'))) {
			*next = '\0';
			next += 1;
		}
		ntok = strsplit(line, tokens, MAXTOKEN);
		if (ntok < 2)
			continue;

		if (sockaddr_from_str(&u.sa, family, tokens[0]) == -1)
			continue;

		if (as->as.ai.hints.ai_flags & (AI_CANONNAME | AI_FQDN))
			c = tokens[1];
		else
			c = NULL;

		if (addrinfo_add(as, &u.sa, c))
			return (-1); /* errno set */
	}
	return (0);
}
#endif
