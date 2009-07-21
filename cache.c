/*
 * Ticket cache initialization.
 *
 * Provides functions for creating ticket caches, used by pam_authenticate,
 * pam_setcred, and pam_chauthtok after changing an expired password.
 *
 * Copyright 2005, 2006, 2007, 2008 Russ Allbery <rra@debian.org>
 * Copyright 2005 Andres Salomon <dilinger@debian.org>
 * Copyright 1999, 2000 Frank Cusack <fcusack@fcusack.com>
 *
 * See LICENSE for licensing terms.
 */

#include "config.h"

#include <errno.h>
#include <krb5.h>
#ifdef HAVE_SECURITY_PAM_APPL_H
# include <security/pam_appl.h>
# include <security/pam_modules.h>
#elif HAVE_PAM_PAM_APPL_H
# include <pam/pam_appl.h>
# include <pam/pam_modules.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"

/*
 * Get the name of a cache.  Takes the name of the environment variable that
 * should be set to indicate which cache to use, either the permanent cache
 * (KRB5CCNAME) or the temporary cache (PAM_KRB5CCNAME).
 *
 * Treat an empty environment variable setting the same as if the variable
 * was not set, since on FreeBSD we can't delete the environment variable,
 * only set it to an empty value.
 */
const char *
pamk5_get_krb5ccname(struct pam_args *args, const char *key)
{
    const char *name;

    /* When refreshing a cache, we need to try the regular environment. */
    name = pam_getenv(args->pamh, key);
    if (name == NULL || *name == '\0')
        name = getenv(key);
    if (name == NULL || *name == '\0')
        return NULL;
    else
        return name;
}


/*
 * Put the ticket cache information into the environment.  Takes the path and
 * the environment variable to set, since this is used both for the permanent
 * cache (KRB5CCNAME) and the temporary cache (PAM_KRB5CCNAME).  Returns a PAM
 * status code.
 */
int
pamk5_set_krb5ccname(struct pam_args *args, const char *name, const char *key)
{
    char *env_name = NULL;
    int pamret;

    env_name = malloc(strlen(key) + 1 + strlen(name) + 1);
    if (env_name == NULL) {
        pamk5_error(args, "malloc failure: %s", strerror(errno));
        pamret = PAM_BUF_ERR;
        goto done;
    }
    sprintf(env_name, "%s=%s", key, name);
    pamret = pam_putenv(args->pamh, env_name);
    if (pamret != PAM_SUCCESS) {
        pamk5_error(args, "pam_putenv: %s", pam_strerror(args->pamh, pamret));
        pamret = PAM_SERVICE_ERR;
        goto done;
    }
    pamret = PAM_SUCCESS;

done:
    if (env_name != NULL)
        free(env_name);
    return pamret;
}


/*
 * Given the template for a ticket cache name, initialize that file securely
 * mkstemp.  Returns a PAM success or error code.
 */
int
pamk5_cache_mkstemp(struct pam_args *args, char *template)
{
    int ccfd;

    ccfd = mkstemp(template);
    if (ccfd < 0) {
        pamk5_error(args, "mkstemp(\"%s\") failed: %s", template,
                    strerror(errno));
        return PAM_SERVICE_ERR;
    }
    close(ccfd);
    return PAM_SUCCESS;
}


/*
 * Given a cache name and the initial credentials, initialize the cache, store
 * the credentials in that cache, and return a pointer to the new cache in the
 * cache argument.  Returns a PAM success or error code.
 */
int
pamk5_cache_init(struct pam_args *args, const char *ccname, krb5_creds *creds,
                 krb5_ccache *cache)
{
    struct context *ctx;
    int retval;

    if (args == NULL || args->ctx == NULL || args->ctx->context == NULL)
        return PAM_SERVICE_ERR;
    ctx = args->ctx;
    retval = krb5_cc_resolve(ctx->context, ccname, cache);
    if (retval != 0) {
        pamk5_debug_krb5(args, "krb5_cc_resolve", retval);
        retval = PAM_SERVICE_ERR;
        goto done;
    }
    retval = krb5_cc_initialize(ctx->context, *cache, ctx->princ);
    if (retval != 0) {
        pamk5_debug_krb5(args, "krb5_cc_initialize", retval);
        retval = PAM_SERVICE_ERR;
        goto done;
    }
    retval = krb5_cc_store_cred(ctx->context, *cache, creds);
    if (retval != 0) {
        pamk5_debug_krb5(args, "krb5_cc_store_cred", retval);
        retval = PAM_SERVICE_ERR;
        goto done;
    }

done:
    if (retval != PAM_SUCCESS && *cache != NULL) {
        krb5_cc_destroy(ctx->context, *cache);
        *cache = NULL;
    }
    return retval;
}


/*
 * Initialize an internal ticket cache with a random name, store the given
 * credentials in the cache, and store the cache in the context.  Put the path
 * in PAM_KRB5CCNAME where it can be picked up later by pam_setcred.  Returns
 * a PAM success or error code.
 */
int
pamk5_cache_init_random(struct pam_args *args, krb5_creds *creds)
{
    char cache_name[] = "/tmp/krb5cc_pam_XXXXXX";
    int pamret;

    /* Store the obtained credentials in a temporary cache. */
    pamret = pamk5_cache_mkstemp(args, cache_name);
    if (pamret != PAM_SUCCESS)
        return pamret;
    pamret = pamk5_cache_init(args, cache_name, creds, &args->ctx->cache);
    if (pamret != PAM_SUCCESS)
        return pamret;
    pamret = pamk5_set_krb5ccname(args, cache_name, "PAM_KRB5CCNAME");
    return pamret;
}