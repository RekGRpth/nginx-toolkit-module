/*
 * Copyright (C) AlexWoo(Wu Jie) wj19840501@gmail.com
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include "ngx_event_timer_module.h"
#include "ngx_event_resolver.h"
#include "ngx_dynamic_resolver.h"


static ngx_int_t ngx_dynamic_resolver_process_init(ngx_cycle_t *cycle);

static void *ngx_dynamic_resolver_create_conf(ngx_cycle_t *cycle);
static char *ngx_dynamic_resolver_init_conf(ngx_cycle_t *cycle, void *conf);


#define MAX_DOMAIN_LEN      128
#define MAX_ADDRS           8


typedef struct ngx_dynamic_resolver_ctx_s       ngx_dynamic_resolver_ctx_t;
typedef struct ngx_dynamic_resolver_domain_s    ngx_dynamic_resolver_domain_t;

struct ngx_dynamic_resolver_ctx_s {
    ngx_dynamic_resolver_handler_pt     h;
    void                               *data;

    ngx_dynamic_resolver_ctx_t         *next;
};

typedef struct {
    struct sockaddr                     sockaddr;
    socklen_t                           socklen;
    u_short                             priority;
    u_short                             weight;
} ngx_dynamic_resolver_addr_t;

struct ngx_dynamic_resolver_domain_s {
    ngx_str_t                           domain;
    u_char                              domain_cstr[MAX_DOMAIN_LEN];

    ngx_uint_t                          naddrs;
    ngx_dynamic_resolver_addr_t         addrs[MAX_ADDRS];

    ngx_dynamic_resolver_ctx_t         *ctx;

    ngx_dynamic_resolver_domain_t      *next;
};

typedef struct {
    ngx_msec_t                          refresh_interval;

    size_t                              domain_buckets;
    ngx_dynamic_resolver_domain_t     **resolver_hash;

    ngx_dynamic_resolver_ctx_t         *free_ctx;
    ngx_dynamic_resolver_domain_t      *free_domain;
} ngx_dynamic_resolver_conf_t;


static ngx_str_t dynamic_resolver_name = ngx_string("dynamic_resolver");


static ngx_command_t ngx_dynamic_resolver_commands[] = {

    { ngx_string("dynamic_refresh_interval"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      0,
      offsetof(ngx_dynamic_resolver_conf_t, refresh_interval),
      NULL },

    { ngx_string("dynamic_domain_buckets"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      0,
      offsetof(ngx_dynamic_resolver_conf_t, domain_buckets),
      NULL },

      ngx_null_command
};


ngx_event_module_t  ngx_dynamic_resolver_module_ctx = {
    &dynamic_resolver_name,
    ngx_dynamic_resolver_create_conf,         /* create configuration */
    ngx_dynamic_resolver_init_conf,           /* init configuration */

    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};


/* this module use ngx_cycle->log */
ngx_module_t  ngx_dynamic_resolver_module = {
    NGX_MODULE_V1,
    &ngx_dynamic_resolver_module_ctx,         /* module context */
    ngx_dynamic_resolver_commands,            /* module directives */
    NGX_EVENT_MODULE,                       /* module type */
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    ngx_dynamic_resolver_process_init,        /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    NULL,                                   /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_dynamic_resolver_create_conf(ngx_cycle_t *cycle)
{
    ngx_dynamic_resolver_conf_t      *conf;

    conf = ngx_pcalloc(cycle->pool, sizeof(ngx_dynamic_resolver_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->refresh_interval = NGX_CONF_UNSET_MSEC;
    conf->domain_buckets = NGX_CONF_UNSET_UINT;

    return conf;
}

static char *
ngx_dynamic_resolver_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_dynamic_resolver_conf_t      *drcf = conf;

    ngx_conf_init_msec_value(drcf->refresh_interval, 0);
    ngx_conf_init_uint_value(drcf->domain_buckets, 101);

    if (drcf->refresh_interval > 0 && drcf->domain_buckets > 0) {
        drcf->resolver_hash = ngx_pcalloc(cycle->pool,
            sizeof(ngx_dynamic_resolver_domain_t *) * drcf->domain_buckets);
    }

    return NGX_CONF_OK;
}

/* reuse for ngx_dynamic_resolver_ctx_t */
static ngx_dynamic_resolver_ctx_t *
ngx_dynamic_resolver_get_ctx()
{
    ngx_dynamic_resolver_conf_t    *drcf;
    ngx_dynamic_resolver_ctx_t     *ctx;

    drcf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_dynamic_resolver_module);

    ctx = drcf->free_ctx;

    if (ctx == NULL) {
        ctx = ngx_pcalloc(ngx_cycle->pool, sizeof(ngx_dynamic_resolver_ctx_t));

        if (ctx == NULL) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "dynamic resolver, "
                    "alloc memory dynamic resolver ctx failed");
            return NULL;
        }
    } else {
        drcf->free_ctx = drcf->free_ctx->next;
        ngx_memzero(ctx, sizeof(ngx_dynamic_resolver_ctx_t));
    }

    return ctx;
}

static void
ngx_dynamic_resolver_put_ctx(ngx_dynamic_resolver_ctx_t *ctx)
{
    ngx_dynamic_resolver_conf_t    *drcf;

    drcf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_dynamic_resolver_module);

    ctx->next = drcf->free_ctx;
    drcf->free_ctx = ctx;
}

/* reuse for ngx_dynamic_resolver_domain_t */
static ngx_dynamic_resolver_domain_t *
ngx_dynamic_resolver_get_domain()
{
    ngx_dynamic_resolver_conf_t    *drcf;
    ngx_dynamic_resolver_domain_t  *domain;

    drcf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_dynamic_resolver_module);

    domain = drcf->free_domain;

    if (domain == NULL) {
        domain = ngx_pcalloc(ngx_cycle->pool,
                             sizeof(ngx_dynamic_resolver_domain_t));

        if (domain == NULL) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "dynamic resolver, "
                    "alloc memory dynamic resolver domain failed");
            return NULL;
        }
    } else {
        drcf->free_domain = drcf->free_domain->next;
        ngx_memzero(domain, sizeof(ngx_dynamic_resolver_domain_t));
    }

    return domain;
}

static void
ngx_dynamic_resolver_put_domain(ngx_dynamic_resolver_domain_t *domain)
{
    ngx_dynamic_resolver_conf_t    *drcf;

    drcf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_dynamic_resolver_module);

    domain->next = drcf->free_domain;
    drcf->free_domain = domain;
}


static void
ngx_dynamic_resolver_on_result(void *data, ngx_resolver_addr_t *addrs,
        ngx_uint_t naddrs)
{
    ngx_dynamic_resolver_domain_t  *domain;
    ngx_dynamic_resolver_ctx_t     *ctx;
    ngx_uint_t                      i, n;

    domain = data;

    if (domain == NULL) {
        ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "dynamic resolver, "
                "%V has been deleted", &domain->domain);
        return;
    }

    if (naddrs == 0) {
        ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "dynamic resolver, "
                "resolver failed");

        while (domain->ctx) {
            ctx = domain->ctx;
            domain->ctx = ctx->next;

            ctx->h(ctx->data, NULL, 0);
            ngx_dynamic_resolver_put_ctx(ctx);
        }

        return;
    }

    domain->naddrs = ngx_min(naddrs, MAX_ADDRS);
    for (i = 0; i < domain->naddrs; ++i) {
        ngx_memcpy(&domain->addrs[i].sockaddr, addrs[i].sockaddr,
                   addrs[i].socklen);
        domain->addrs[i].socklen = addrs[i].socklen;
        domain->addrs[i].priority = addrs[i].priority;
        domain->addrs[i].weight = addrs[i].weight;

        n = ngx_random() % domain->naddrs;

        while (domain->ctx) {
            ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "dynamic resolver, "
                    "resolver successd");
            ctx = domain->ctx;
            domain->ctx = ctx->next;

            ctx->h(ctx->data, &domain->addrs[n].sockaddr,
                   domain->addrs[n].socklen);
            ngx_dynamic_resolver_put_ctx(ctx);

            ++n;
            n %= domain->naddrs;
        }
    }
}

static void
ngx_dynamic_resolver_on_timer(void *data)
{
    ngx_dynamic_resolver_conf_t    *drcf;
    ngx_dynamic_resolver_domain_t  *domain;
    ngx_uint_t                      i;

    if (ngx_exiting) {
        return;
    }

    drcf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_dynamic_resolver_module);

    for (i = 0; i < drcf->domain_buckets; ++i) {
        domain = drcf->resolver_hash[i];
        while (domain) {
            ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "dynamic resolver, "
                "on timer start resolver %V", &domain->domain);
            ngx_event_resolver_start_resolver(&domain->domain,
                    ngx_dynamic_resolver_on_result, domain);
            domain = domain->next;
        }
    }

    ngx_event_timer_add_timer(drcf->refresh_interval,
            ngx_dynamic_resolver_on_timer, NULL);
}

static ngx_int_t
ngx_dynamic_resolver_process_init(ngx_cycle_t *cycle)
{
    ngx_dynamic_resolver_conf_t      *drcf;

    drcf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_dynamic_resolver_module);
    if (drcf->refresh_interval == 0) {
        return NGX_OK;
    }

    ngx_event_timer_add_timer(drcf->refresh_interval,
            ngx_dynamic_resolver_on_timer, NULL);

    return NGX_OK;
}


void
ngx_dynamic_resolver_del_domain(ngx_str_t *domain)
{
    ngx_dynamic_resolver_conf_t    *drcf;
    ngx_dynamic_resolver_domain_t **pd, *d;
    ngx_dynamic_resolver_ctx_t     *ctx;
    ngx_uint_t                      idx;
    u_char                          temp[MAX_DOMAIN_LEN];

    drcf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_dynamic_resolver_module);
    if (drcf->refresh_interval == 0) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "dynamic resolver, "
                "dynamic resolver closed when del domain");
        return;
    }

    if (domain->len > MAX_DOMAIN_LEN) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "dynamic resolver, "
                "domain length(%z) is too long", domain->len);
        return;
    }

    ngx_memzero(temp, MAX_DOMAIN_LEN);
    idx = ngx_hash_strlow(temp, domain->data, domain->len);
    idx %= drcf->domain_buckets;

    ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "dynamic resolver, "
            "prepare del %V in %d slot", domain, idx);

    for (pd = &drcf->resolver_hash[idx]; *pd; pd = &(*pd)->next) {

        if ((*pd)->domain.len == domain->len &&
            ngx_memcmp((*pd)->domain.data, temp, domain->len) == 0)
        {
            d= *pd;
            *pd = (*pd)->next;

            while (d->ctx) {
                ctx = d->ctx;
                d->ctx = ctx->next;

                ctx->h(ctx->data, NULL, 0);
                ngx_dynamic_resolver_put_ctx(ctx);
            }

            ngx_dynamic_resolver_put_domain(d);

            return;
        }
    }

    ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "dynamic resolver, "
                  "%V is not in dynamic resolv hash table", domain);
}

void
ngx_dynamic_resolver_start_resolver(ngx_str_t *domain,
    ngx_dynamic_resolver_handler_pt h, void *data)
{
    ngx_dynamic_resolver_conf_t    *drcf;
    ngx_dynamic_resolver_domain_t  *d;
    ngx_dynamic_resolver_ctx_t     *ctx;
    ngx_uint_t                      idx, n;
    struct sockaddr                 sa;
    struct sockaddr_in             *sin;
    in_addr_t                       addr;
    socklen_t                       len;
    u_char                          temp[MAX_DOMAIN_LEN];

    if (domain == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "dynamic resolver, "
                "domain is NULL");
        return;
    }

    addr = ngx_inet_addr(domain->data, domain->len);
    /* addr is IP address */
    if (addr != INADDR_NONE) {
        sin = (struct sockaddr_in *) &sa;

        len = sizeof(struct sockaddr_in);
        ngx_memzero(sin, sizeof(struct sockaddr_in));
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = addr;

        ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "dynamic resolver, "
                "domain is IP address");

        h(data, &sa, len);

        return;
    }

    drcf = ngx_event_get_conf(ngx_cycle->conf_ctx, ngx_dynamic_resolver_module);
    if (drcf->refresh_interval == 0) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "dynamic resolver, "
                "dynamic resolver closed when start resolver");
        goto failed;
    }

    if (domain->len > MAX_DOMAIN_LEN) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "dynamic resolver, "
                "domain length(%z) is too long", domain->len);
        goto failed;
    }

    ngx_memzero(temp, MAX_DOMAIN_LEN);
    idx = ngx_hash_strlow(temp, domain->data, domain->len);
    idx %= drcf->domain_buckets;

    d = drcf->resolver_hash[idx];
    while (d) {
        if (d->domain.len == domain->len &&
            ngx_memcmp(d->domain.data, temp, domain->len) == 0)
        {
            break;
        }

        ctx = ctx->next;
    }

    if (d == NULL) { /* not found */
        d = ngx_dynamic_resolver_get_domain();
        if (d == NULL) {
            goto failed;
        }

        /* add domain in dynamic resolver */
        d->next = drcf->resolver_hash[idx];
        drcf->resolver_hash[idx] = d;

        ngx_memcpy(d->domain_cstr, temp, MAX_DOMAIN_LEN);
        d->domain.data = d->domain_cstr;
        d->domain.len = domain->len;
    }

    /* domain is not resolved */
    if (d->naddrs == 0) {

        /* add call back in resolver list */
        ctx = ngx_dynamic_resolver_get_ctx();
        if (ctx == NULL) {
            goto failed;
        }

        ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "dynamic resolver, "
                "domain is not resolved");
        ctx->h = h;
        ctx->data = data;

        ctx->next = d->ctx;
        d->ctx = ctx;

        ngx_event_resolver_start_resolver(&d->domain,
                ngx_dynamic_resolver_on_result, d);

        return;
    }

    ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0, "dynamic resolver, "
            "domain is resolved, call directly");

    /* call callback */
    n = ngx_random() % d->naddrs;
    h(data, &d->addrs[n].sockaddr, d->addrs[n].socklen);

    return;

failed:

    h(data, NULL, 0);
}
