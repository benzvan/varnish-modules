/*-
 * Copyright (c) 2012-2016 Varnish Software
 *
 * Author: Lasse Karstensen <lasse.karstensen@gmail.com>
 * Author: Lasse Karstensen <lkarsten@varnish-software.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Cookie VMOD that simplifies handling of the Cookie request header.
 */

#define _GNU_SOURCE
#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>

#include "vmod_config.h"

#include "vqueue.h"
#include "vsb.h"
#include "vre.h"

#include "vcc_cookie_if.h"

#define VRE_MAX_GROUPS 8

enum filter_action {
	blacklist = 0,
	whitelist = 1
};

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

struct cookie {
	unsigned magic;
#define VMOD_COOKIE_ENTRY_MAGIC 0x3BB41543
	char *name;
	char *value;
	VTAILQ_ENTRY(cookie) list;
};

/* A structure to represent both whitelists and blacklists */
struct matchlist {
	char *name;
	VTAILQ_ENTRY(matchlist) list;
};

struct vmod_cookie {
	unsigned magic;
#define VMOD_COOKIE_MAGIC 0x4EE5FB2E
	VTAILQ_HEAD(, cookie) cookielist;
};

static void
cobj_free(void *p)
{
	struct vmod_cookie *vcp;

	CAST_OBJ_NOTNULL(vcp, p, VMOD_COOKIE_MAGIC);
	FREE_OBJ(vcp);
}

static struct vmod_cookie *
cobj_get(struct vmod_priv *priv)
{
	struct vmod_cookie *vcp;

	if (priv->priv == NULL) {
		ALLOC_OBJ(vcp, VMOD_COOKIE_MAGIC);
		AN(vcp);
		VTAILQ_INIT(&vcp->cookielist);
		priv->priv = vcp;
		priv->free = cobj_free;
	} else
		CAST_OBJ_NOTNULL(vcp, priv->priv, VMOD_COOKIE_MAGIC);

	return (vcp);
}

VCL_VOID
vmod_parse(VRT_CTX, struct vmod_priv *priv, VCL_STRING cookieheader)
{
	struct vmod_cookie *vcp = cobj_get(priv);

	char *name, *value;
	const char *p, *sep;
	int i = 0;

	if (!cookieheader || *cookieheader == '\0') {
		VSLb(ctx->vsl, SLT_Debug, "cookie: nothing to parse");
		return;
	}

	if (!VTAILQ_EMPTY(&vcp->cookielist)) {
		/*
		 * If called twice during the same request, clean out old state
		 */
		vmod_clean(ctx, priv);
	}

	p = cookieheader;
	while (*p != '\0') {
		while (isspace(*p)) p++;
		sep = strchr(p, '=');
		if (sep == NULL)
			break;
		name = strndup(p, pdiff(p, sep));
		p = sep + 1;

		sep = p;
		while (*sep != '\0' && *sep != ';')
			sep++;
		value = strndup(p, pdiff(p, sep));

		vmod_set(ctx, priv, name, value);
		free(name);
		free(value);
		i++;
		if (*sep == '\0')
			break;
		p = sep + 1;
	}
	VSLb(ctx->vsl, SLT_Debug, "cookie: parsed %i cookies.", i);
}

static struct cookie *
find_cookie(struct vmod_cookie *vcp, VCL_STRING name)
{
	struct cookie *cookie;
	VTAILQ_FOREACH(cookie, &vcp->cookielist, list) {
		CHECK_OBJ_NOTNULL(cookie, VMOD_COOKIE_ENTRY_MAGIC);
		if (!strcmp(cookie->name, name))
			break;
	}
	return(cookie);
}

VCL_VOID
vmod_set(VRT_CTX, struct vmod_priv *priv, VCL_STRING name, VCL_STRING value)
{
	struct vmod_cookie *vcp = cobj_get(priv);

	/* Empty cookies should be ignored. */
	if (name == NULL || *name == '\0')
		return;
	if (value == NULL || *value == '\0')
		return;

	char *p;
	struct cookie *cookie = find_cookie(vcp, name);
	if (cookie != NULL) {
		p = WS_Printf(ctx->ws, "%s", value);
		if (p == NULL) {
			VSLb(ctx->vsl, SLT_Error,
			    "cookie: Workspace overflow in set()");
		} else
			cookie->value = p;

		return;
	}

	cookie = WS_Alloc(ctx->ws, sizeof(struct cookie));
	if (cookie == NULL) {
		VSLb(ctx->vsl, SLT_Error,
		    "cookie: unable to get storage for cookie");
		return;
	}
	cookie->magic = VMOD_COOKIE_ENTRY_MAGIC;
	cookie->name = WS_Printf(ctx->ws, "%s", name);
	cookie->value = WS_Printf(ctx->ws, "%s", value);
	if (cookie->name == NULL || cookie->value == NULL) {
		VSLb(ctx->vsl, SLT_Error,
		    "cookie: unable to get storage for cookie");
		return;
	}
	VTAILQ_INSERT_TAIL(&vcp->cookielist, cookie, list);
}

VCL_BOOL
vmod_isset(VRT_CTX, struct vmod_priv *priv, const char *name)
{
	struct vmod_cookie *vcp = cobj_get(priv);
	(void)ctx;

	if (name == NULL || *name == '\0')
		return(0);

	struct cookie *cookie = find_cookie(vcp, name);

	return (cookie ? 1 : 0);
}

VCL_STRING
vmod_get(VRT_CTX, struct vmod_priv *priv, VCL_STRING name)
{
	struct vmod_cookie *vcp = cobj_get(priv);
	(void)ctx;

	if (name == NULL || *name == '\0')
		return(NULL);

	struct cookie *cookie = find_cookie(vcp, name);

	return (cookie ? cookie->value : NULL);
}


static vre_t *
compile_re(VRT_CTX, VCL_STRING expression) {
	vre_t *vre;
	const char *error;
	int erroroffset;

	vre = VRE_compile(expression, 0, &error, &erroroffset);
	if (vre == NULL) {
		VSLb(ctx->vsl, SLT_Error, "cookie: PCRE compile error at char %i: %s",
		    erroroffset, error);
		return(NULL);
	}
	return(vre);
}

static void
free_re(void *priv)
{
	vre_t *vre;

	AN(priv);
	vre = priv;
	VRE_free(&vre);
	AZ(vre);
}

VCL_STRING
vmod_get_re(VRT_CTX, struct vmod_priv *priv, struct vmod_priv *priv_call,
    VCL_STRING expression)
{
	struct vmod_cookie *vcp = cobj_get(priv);
	(void)ctx;
	int i, ovector[VRE_MAX_GROUPS];
	struct cookie *cookie = NULL;
	struct cookie *current;
	vre_t *vre = NULL;

	if (expression == NULL || *expression == '\0')
		return(NULL);

	if (priv_call->priv == NULL) {
		AZ(pthread_mutex_lock(&mtx));
		vre = compile_re(ctx, expression);
		if (!vre) {
			AZ(pthread_mutex_unlock(&mtx));
			return(NULL);
		}

		priv_call->priv = vre;
		priv_call->free = free_re;
		AZ(pthread_mutex_unlock(&mtx));
	}

	VTAILQ_FOREACH(current, &vcp->cookielist, list) {
		CHECK_OBJ_NOTNULL(current, VMOD_COOKIE_ENTRY_MAGIC);
		VSLb(ctx->vsl, SLT_Debug, "cookie: checking %s", current->name);
		i = VRE_exec(vre, current->name, strlen(current->name), 0, 0,
		    ovector, VRE_MAX_GROUPS, NULL);
		if (i < 0)
			continue;

		VSLb(ctx->vsl, SLT_Debug, "cookie: %s is a match for regex '%s'",
		    current->name, expression);
		cookie = current;
		break;
	}

	return (cookie ? cookie->value : NULL);
}


VCL_VOID
vmod_delete(VRT_CTX, struct vmod_priv *priv, VCL_STRING name)
{
	struct vmod_cookie *vcp = cobj_get(priv);
	(void)ctx;

	if (name == NULL || *name == '\0')
		return;

	struct cookie *cookie = find_cookie(vcp, name);

	if (cookie != NULL)
		VTAILQ_REMOVE(&vcp->cookielist, cookie, list);
}

VCL_VOID
vmod_clean(VRT_CTX, struct vmod_priv *priv)
{
	struct vmod_cookie *vcp = cobj_get(priv);
	(void)ctx;

	AN(&vcp->cookielist);
	VTAILQ_INIT(&vcp->cookielist);
}

static void
filter_cookies(struct vmod_priv *priv, VCL_STRING list_s,
    enum filter_action mode)
{
	struct cookie *cookieptr, *safeptr;
	struct vmod_cookie *vcp = cobj_get(priv);
	struct matchlist *mlentry, *mlsafe;
	char const *p = list_s, *q;
	int matched = 0;

	VTAILQ_HEAD(, matchlist) matchlist_head;
	VTAILQ_INIT(&matchlist_head);

	/* Parse the supplied list. */
	while (p && *p != '\0') {
		while (isspace(*p))
			p++;
		if (*p == '\0')
			break;

		q = p;
		while (*q != '\0' && *q != ',')
			q++;

		if (q == p) {
			p++;
			continue;
		}

		mlentry = malloc(sizeof(struct matchlist));
		AN(mlentry);
		mlentry->name = strndup(p, q - p);
		AN(mlentry->name);

		VTAILQ_INSERT_TAIL(&matchlist_head, mlentry, list);

		p = q;
		if (*p != '\0')
			p++;
	}

	/* Filter existing cookies that either aren't in the whitelist or
	 * are in the blacklist (depending on the filter_action) */
	VTAILQ_FOREACH_SAFE(cookieptr, &vcp->cookielist, list, safeptr) {
		CHECK_OBJ_NOTNULL(cookieptr, VMOD_COOKIE_ENTRY_MAGIC);
		matched = 0;

		VTAILQ_FOREACH(mlentry, &matchlist_head, list) {
			if (strcmp(cookieptr->name, mlentry->name) == 0) {
				matched = 1;
				break;
			}
		}
		if (matched != mode)
			VTAILQ_REMOVE(&vcp->cookielist, cookieptr, list);
	}

	VTAILQ_FOREACH_SAFE(mlentry, &matchlist_head, list, mlsafe) {
		VTAILQ_REMOVE(&matchlist_head, mlentry, list);
		free(mlentry->name);
		free(mlentry);
	}
}

VCL_VOID
vmod_filter_except(VRT_CTX, struct vmod_priv *priv, VCL_STRING whitelist_s)
{
	VSLb(ctx->vsl, SLT_VCL_Log,
	    "cookie.filter_except() is DEPRECATED, use keep() instead");
	vmod_keep(ctx, priv, whitelist_s);
}

VCL_VOID
vmod_keep(VRT_CTX, struct vmod_priv *priv, VCL_STRING whitelist_s)
{
	(void)ctx;
	filter_cookies(priv, whitelist_s, whitelist);
}


VCL_VOID
vmod_filter(VRT_CTX, struct vmod_priv *priv, VCL_STRING blacklist_s)
{
	(void)ctx;
	filter_cookies(priv, blacklist_s, blacklist);
}


static void
re_filter(VRT_CTX, struct vmod_priv *priv, struct vmod_priv *priv_call,
    VCL_STRING expression, enum filter_action mode)
{
	struct vmod_cookie *vcp = cobj_get(priv);
	struct cookie *current, *safeptr;
	(void)ctx;
	int i, ovector[VRE_MAX_GROUPS];
	vre_t *vre = NULL;

	if (priv_call->priv == NULL) {
		AZ(pthread_mutex_lock(&mtx));
		vre = compile_re(ctx, expression);
		if (!vre) {
			AZ(pthread_mutex_unlock(&mtx));
			return;   // Not much else to do, error already logged.
		}

		priv_call->priv = vre;
		priv_call->free = free_re;
		AZ(pthread_mutex_unlock(&mtx));
	}

	VTAILQ_FOREACH_SAFE(current, &vcp->cookielist, list, safeptr) {
		CHECK_OBJ_NOTNULL(current, VMOD_COOKIE_ENTRY_MAGIC);

		i = VRE_exec(priv_call->priv, current->name,
		    strlen(current->name), 0, 0, ovector, VRE_MAX_GROUPS, NULL);

		switch (mode) {
		case blacklist:
			if (i < 0)
				continue;
			VSLb(ctx->vsl, SLT_Debug, "Removing matching cookie %s (value: %s)",
			    current->name, current->value);
			VTAILQ_REMOVE(&vcp->cookielist, current, list);
			break;
		case whitelist:
			if (i >= 0) {
				VSLb(ctx->vsl, SLT_Debug, "Cookie %s matches expression '%s'",
				    current->name, expression);
				continue;
			}

			VSLb(ctx->vsl, SLT_Debug, "Removing cookie %s (value: %s)",
			    current->name, current->value);
			VTAILQ_REMOVE(&vcp->cookielist, current, list);
			break;
		default:
			WRONG("invalid mode");
		}
	}
}


VCL_VOID
vmod_keep_re(VRT_CTX, struct vmod_priv *priv, struct vmod_priv *priv_call,
    VCL_STRING expression)
{
	re_filter(ctx, priv, priv_call, expression, whitelist);
}


VCL_VOID
vmod_filter_re(VRT_CTX, struct vmod_priv *priv, struct vmod_priv *priv_call,
    VCL_STRING expression)
{
	re_filter(ctx, priv, priv_call, expression, blacklist);
}


VCL_STRING
vmod_get_string(VRT_CTX, struct vmod_priv *priv)
{
	struct cookie *curr;
	struct vsb *output;
	void *u;
	struct vmod_cookie *vcp = cobj_get(priv);

	output = VSB_new_auto();
	AN(output);

	VTAILQ_FOREACH(curr, &vcp->cookielist, list) {
		CHECK_OBJ_NOTNULL(curr, VMOD_COOKIE_ENTRY_MAGIC);
		AN(curr->name);
		AN(curr->value);
		VSB_printf(output, "%s%s=%s;",
		    (curr == VTAILQ_FIRST(&vcp->cookielist)) ? "" : " ",
		    curr->name, curr->value);
	}
	VSB_finish(output);

	u = WS_Alloc(ctx->ws, VSB_len(output) + 1);
	if (!u) {
		VSLb(ctx->vsl, SLT_Error, "cookie: Workspace overflow");
		VSB_delete(output);
		return(NULL);
	}

	strcpy(u, VSB_data(output));
	VSB_delete(output);
	return (u);
}

VCL_STRING
vmod_format_rfc1123(VRT_CTX, VCL_TIME ts, VCL_DURATION duration)
{
	return VRT_TIME_string(ctx, ts + duration);
}
