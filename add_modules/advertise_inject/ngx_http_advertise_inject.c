
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


#define NGX_HTTP_ADVERTISE_START     0
#define NGX_HTTP_ADVERTISE_READ      1
#define NGX_HTTP_ADVERTISE_PROCESS   2
#define NGX_HTTP_ADVERTISE_PASS      3
#define NGX_HTTP_ADVERTISE_DONE      4

#define NGX_HTTP_ADVERTISE_BUFFERED  0x30

typedef struct {
    ngx_flag_t                   inject;
    ngx_int_t                    flag;
    ngx_int_t                    index;
    size_t                       buffer_size;
    size_t                       max_advertise_len;
    ngx_str_t                    location_str;
    ngx_array_t                 *advertise_array;
}ngx_http_advertise_conf_t;


typedef struct {
    u_char                       *html;
    u_char                       *last;
    ngx_str_t                    *target;
    
    ngx_uint_t                   phase;
    ngx_uint_t                   status;
    size_t                       length;
    size_t                       alength;
} ngx_http_advertise_ctx_t;

static ngx_str_t g_advertise_inject_ctx_s = ngx_string("g_advertise_inject_ctx");
static ngx_str_t g_advertise_status_s = ngx_string("advertise_status");


static ngx_int_t ngx_http_advertise_init(ngx_conf_t *cf);
static char *ngx_http_advertise_list(ngx_conf_t *cf, 
                    ngx_command_t *cmd, void *conf);
static char *ngx_http_advertise_item(ngx_conf_t *cf, 
                    ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_advertise_process(ngx_http_request_t *r);
static ngx_int_t ngx_http_advertise_read(ngx_http_request_t *r, 
                    ngx_chain_t *in);
static ngx_int_t ngx_http_advertise_send(ngx_http_request_t *r, 
                    ngx_http_advertise_ctx_t *ctx, ngx_chain_t *in);
static ngx_int_t ngx_http_advertise_set_status(ngx_http_request_t *r, 
                    ngx_uint_t advertise_pass);
static void *ngx_http_advertise_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_advertise_merge_loc_conf(ngx_conf_t *cf, 
                    void *parent, void *child);


static ngx_command_t  ngx_http_advertise_commands[] = {
    { ngx_string("advertise"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG
                                                              |NGX_HTTP_LIF_CONF,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_advertise_conf_t, inject),
      NULL },
    { ngx_string("advertise_location"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_advertise_conf_t, location_str),
      NULL },  
    { ngx_string("advertise_flag"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_advertise_conf_t, flag),
      NULL },
    { ngx_string("advertise_buffer"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_advertise_conf_t, buffer_size),
      NULL },
    { ngx_string("advertise_list"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
                                          |NGX_CONF_BLOCK|NGX_CONF_NOARGS,
      ngx_http_advertise_list,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
      ngx_null_command
};


static ngx_http_module_t  ngx_http_advertise_module_ctx = {
    NULL,                                       /* preconfiguration */
    ngx_http_advertise_init,                    /* postconfiguration */

    NULL,                                       /* create main configuration */
    NULL,                                       /* init main configuration */

    NULL,                                       /* create server configuration */
    NULL,                                       /* merge server configuration */

    ngx_http_advertise_create_loc_conf,         /* create location configuration */
    ngx_http_advertise_merge_loc_conf           /* merge location configuration */
};


ngx_module_t  ngx_http_advertise_module = {
    NGX_MODULE_V1,
    &ngx_http_advertise_module_ctx,             /* module context */
    ngx_http_advertise_commands,                /* module directives */
    NGX_HTTP_MODULE,                            /* module type */
    NULL,                                       /* init master */
    NULL,                                       /* init module */
    NULL,                                       /* init process */
    NULL,                                       /* init thread */
    NULL,                                       /* exit thread */
    NULL,                                       /* exit process */
    NULL,                                       /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;

static char *
ngx_http_advertise_list(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_advertise_conf_t *accf = conf;
    char               *rv;
    ngx_conf_t          save;

    if (accf->advertise_array == NGX_CONF_UNSET_PTR) {
        accf->advertise_array = ngx_array_create(cf->pool, 64, sizeof(ngx_str_t));
        if (accf->advertise_array == NULL) {
            return NGX_CONF_ERROR;
        }
    }
    
    save = *cf;
    cf->handler = ngx_http_advertise_item;
    cf->handler_conf = conf;
    
    rv = ngx_conf_parse(cf, NULL);
    *cf = save;
    return rv;
}


static char *
ngx_http_advertise_item(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_advertise_conf_t  *accf = conf;
    ngx_str_t           *value;
    ngx_uint_t           i;
    ngx_str_t           *re;
    
    value = cf->args->elts;
    for (i = 0; i < cf->args->nelts; i ++) {    
        re = ngx_array_push(accf->advertise_array);
        if (re == NULL) {
            return NGX_CONF_ERROR;
        }
        re->len = value[i].len;
        re->data = value[i].data;
        if (re->len > accf->max_advertise_len) {
            accf->max_advertise_len = re->len;
        }
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_advertise_header_filter(ngx_http_request_t *r)
{
    off_t                        len;
    ngx_http_advertise_ctx_t    *ctx;
    ngx_http_advertise_conf_t   *conf;
    ngx_http_variable_value_t   *vv;

    if ((r->headers_out.status != NGX_HTTP_OK 
        && r->headers_out.status < NGX_HTTP_BAD_REQUEST)
        || r != r->main
        || r->header_only
        || r->headers_out.content_length_n == 0
        || !(r->method & NGX_HTTP_GET)) {
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_advertise_module);

    if (ctx) {
        ngx_http_set_ctx(r, NULL, ngx_http_advertise_module);
        return ngx_http_next_header_filter(r);
    }

    conf = ngx_http_get_module_loc_conf(r, ngx_http_advertise_module);
    if (conf->inject == 0 \
        || conf->advertise_array == NGX_CONF_UNSET_PTR \
        || conf->advertise_array->nelts == 0
        || conf->max_advertise_len == 0) {
        return ngx_http_next_header_filter(r);
    }
    
    vv = ngx_http_get_indexed_variable(r, conf->index);
    if (vv == NULL) {
        return NGX_ERROR;
    }
    
    ctx = (ngx_http_advertise_ctx_t*)(vv->data);
    if (ctx == NULL) {
        return NGX_ERROR;
    }
    
    if (r->headers_out.content_encoding 
        && ngx_strnstr(r->headers_out.content_encoding->value.data,
                (char *) "gzip", r->headers_out.content_encoding->value.len)) {
        ctx->status = 2;
        ctx->phase = NGX_HTTP_ADVERTISE_PASS;
        return ngx_http_next_header_filter(r);
    }
    if (r->headers_out.content_type.len < sizeof("text/html") - 1
        || !ngx_strnstr(r->headers_out.content_type.data,
                (char *)"text/html", r->headers_out.content_type.len)) {
        ctx->status = 3;
        ctx->phase = NGX_HTTP_ADVERTISE_PASS;
        return ngx_http_next_header_filter(r);
    }
    
    len = r->headers_out.content_length_n;
    if (len != -1 && len > (off_t) conf->buffer_size) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "advertise: too big response: %O", len);
        ctx->status = 4;
        ctx->phase = NGX_HTTP_ADVERTISE_PASS;
        return ngx_http_next_header_filter(r);
    }
 
    if (len == -1) {
        ctx->length = conf->buffer_size;
    } else {
        ctx->length = (size_t) len;
    }
    ctx->alength = conf->max_advertise_len;
    
    ngx_http_set_ctx(r, ctx, ngx_http_advertise_module);

    if (r->headers_out.refresh) {
        r->headers_out.refresh->hash = 0;
    }

    r->main_filter_need_in_memory = 1;
    r->allow_ranges = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_http_advertise_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_int_t                   rc;
    ngx_chain_t                 out;
    ngx_buf_t                   *b;
    ngx_http_advertise_ctx_t    *ctx;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "advertisement inject");
    if (in == NULL) {
        return ngx_http_next_body_filter(r, in);
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_advertise_module);

    if (ctx == NULL) {
        return ngx_http_next_body_filter(r, in);
    }
    
    switch (ctx->phase) {

    case NGX_HTTP_ADVERTISE_START:

        ctx->phase = NGX_HTTP_ADVERTISE_READ;

        /* fall through */

    case NGX_HTTP_ADVERTISE_READ:
        
        rc = ngx_http_advertise_read(r, in);

        if (rc == NGX_AGAIN) {
            return NGX_OK;
        }
        if (rc == NGX_ERROR) {
            ctx->status = 5;
            return ngx_http_filter_finalize_request(r,
                        &ngx_http_advertise_module,
                      NGX_HTTP_UNSUPPORTED_MEDIA_TYPE);
        }

        /* fall through */

    case NGX_HTTP_ADVERTISE_PROCESS:
        b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
        if (b == NULL) {
            ctx->status = 6;
            return ngx_http_filter_finalize_request(r,
                        &ngx_http_advertise_module,
                        NGX_HTTP_UNSUPPORTED_MEDIA_TYPE);
        }
        if (ngx_http_advertise_process(r) == NGX_OK) {
            if (r->headers_out.content_length_n > 0)
                r->headers_out.content_length_n += ctx->target->len;
            ngx_http_advertise_set_status(r, 1);
        }
        
        b->pos = ctx->html;
        b->last = ctx->last;
        b->memory = 1;
        b->last_buf = 1;
        out.buf = b;
        out.next = NULL;
        ctx->phase = NGX_HTTP_ADVERTISE_PASS;
        return ngx_http_advertise_send(r, ctx, &out);
       
    case NGX_HTTP_ADVERTISE_PASS:

        return ngx_http_next_body_filter(r, in);

    default: /* NGX_HTTP_IMAGE_DONE */

        rc = ngx_http_next_body_filter(r, NULL);

        /* NGX_ERROR resets any pending data */
        return (rc == NGX_OK) ? NGX_ERROR : rc;
    }
}

static ngx_str_t* 
ngx_http_advertise_get_random(ngx_http_request_t *r)
{
    ngx_http_advertise_conf_t *conf;
    ngx_http_advertise_ctx_t  *ctx;
    ctx = ngx_http_get_module_ctx(r, ngx_http_advertise_module);
    conf = ngx_http_get_module_loc_conf(r, ngx_http_advertise_module);

    ngx_uint_t  length = (ctx->length < 16) ? ctx->length: 16;
    ngx_uint_t  hash_value = 0;

    ngx_uint_t  index = 0;
    for (index = 0; index < length; index ++) {
        hash_value = hash_value + ctx->html[ctx->length - index - 1] * 131; 
    }
    ngx_str_t *advertise_array = conf->advertise_array->elts;
    return advertise_array + hash_value % conf->advertise_array->nelts;
}

static ngx_int_t
ngx_http_advertise_process(ngx_http_request_t *r)
{
    u_char          *index = NULL;
    ngx_int_t        res = NGX_OK;
    ngx_str_t       *adver = NULL;
    ngx_http_advertise_ctx_t  *ctx;
    ngx_http_advertise_conf_t *conf;

    r->connection->buffered &= ~NGX_HTTP_ADVERTISE_BUFFERED;  
    ctx = ngx_http_get_module_ctx(r, ngx_http_advertise_module);
    conf = ngx_http_get_module_loc_conf(r, ngx_http_advertise_module);
    
    index = ngx_strlcasestrn(ctx->html, ctx->last, conf->location_str.data, conf->location_str.len - 1);
    if (index) {
        adver = ngx_http_advertise_get_random(r);
        ngx_memmove(index + (conf->index == 0) ? conf->location_str.len: 0?, index, ctx->last - index);
        ngx_memcpy(index, adver->data, adver->len);
        ctx->last += adver->len;
        ctx->target = adver;
    } else {
        ngx_http_advertise_set_status(r, 7);
        res = NGX_ERROR; 
    }

    return res;
}



static ngx_int_t
ngx_http_advertise_read(ngx_http_request_t *r, ngx_chain_t *in)
{
    u_char                         *p;
    size_t                          size, rest;
    ngx_buf_t                      *b;
    ngx_chain_t                    *cl;
    ngx_http_advertise_ctx_t       *ctx;
    
    ctx = ngx_http_get_module_ctx(r, ngx_http_advertise_module);
 
    if (ctx->html == NULL) {
        ctx->html = ngx_palloc(r->pool, ctx->length + ctx->alength + 1);
        if (ctx->html == NULL) {
            return NGX_ERROR;
        }
    
        ctx->last = ctx->html;
    }
    
    p = ctx->last;
    
    for (cl = in; cl; cl = cl->next) {
    
        b = cl->buf;
        size = b->last - b->pos;
    
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "html buf: %uz", size);
    
        rest = ctx->html + ctx->length - p;
    
        if (size > rest) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "advertisement inject: too big response");
            return NGX_ERROR;
        }
    
        p = ngx_cpymem(p, b->pos, size);
        b->pos += size;
    
        if (b->last_buf) {
            ctx->last = p;
            *(p + 1) = '\0';
            return NGX_OK;
        }
    }
    
    ctx->last = p;
    r->connection->buffered |= NGX_HTTP_ADVERTISE_BUFFERED;
    
    return NGX_AGAIN;
}

static ngx_int_t
ngx_http_advertise_send(ngx_http_request_t *r, ngx_http_advertise_ctx_t *ctx,
    ngx_chain_t *in)
{
    ngx_int_t  rc;

    rc = ngx_http_next_header_filter(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return NGX_ERROR;
    }

    rc = ngx_http_next_body_filter(r, in);

    if (ctx->phase == NGX_HTTP_ADVERTISE_DONE) {
        /* NGX_ERROR resets any pending data */
        return (rc == NGX_OK) ? NGX_ERROR : rc;
    }

    return rc;
}

static ngx_int_t 
ngx_http_g_ctx_gethandler(ngx_http_request_t *r, 
            ngx_http_variable_value_t *v, uintptr_t data)
{
    u_char            *p;
    p = ngx_pcalloc(r->pool, sizeof(ngx_http_advertise_ctx_t));
    if (p == NULL) {
        return NGX_ERROR;
    }
    
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->data = p;
    v->len = sizeof(ngx_http_advertise_ctx_t);
    return NGX_OK;
}

static u_char *ad_status[] = {(u_char*)"PASS", (u_char*)"INJECT", (u_char*)"GZIP", 
                              (u_char*)"TYPEERR", (u_char*)"LARGE", (u_char*)"READERR", 
                              (u_char*)"MALLOCERR", (u_char*)"HTMLERR"};

static ngx_int_t 
ngx_http_status_gethandler(ngx_http_request_t *r, 
            ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_advertise_ctx_t          *ctx;
    ngx_http_variable_value_t         *vv;
    ngx_http_advertise_conf_t         *accf;
    
    accf = ngx_http_get_module_loc_conf(r, ngx_http_advertise_module);

    vv = ngx_http_get_indexed_variable(r, accf->index);
    if (vv == NULL) {
        return NGX_ERROR;
    }

    ctx = (ngx_http_advertise_ctx_t*)(vv->data);
    if (ctx == NULL) {
        return NGX_ERROR;
    }
    v->len = ngx_strlen(ad_status[ctx->status]);
    v->data = ad_status[ctx->status];
    
    v->valid = 1;
    v->not_found = 0;
    v->no_cacheable = 0;
    return NGX_OK;
}


static void *
ngx_http_advertise_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_advertise_conf_t  *accf;
    
    accf = ngx_pcalloc(cf->pool, sizeof(ngx_http_advertise_conf_t));
    if (accf == NULL) {
        return NULL;
    }
    accf->inject = NGX_CONF_UNSET;
    accf->index = NGX_CONF_UNSET_UINT;
    accf->buffer_size = NGX_CONF_UNSET_SIZE;
    accf->advertise_array = NGX_CONF_UNSET_PTR;
    accf->max_advertise_len = 0;
    accf->flag = NGX_CONF_UNSET;

    return accf;
}


static ngx_int_t
ngx_http_advertise_set_status(ngx_http_request_t *r, ngx_uint_t advertise_pass)
{
    ngx_http_advertise_ctx_t          *ctx;
    ngx_http_variable_value_t         *vv;
    ngx_http_advertise_conf_t         *accf;
        
    accf = ngx_http_get_module_loc_conf(r, ngx_http_advertise_module);

    vv = ngx_http_get_indexed_variable(r, accf->index);
    if (vv == NULL) {
        return NGX_ERROR;
    }

    ctx = (ngx_http_advertise_ctx_t*)(vv->data);
    if (ctx == NULL) {
        return NGX_ERROR;
    }
    ctx->status = advertise_pass;
        
    return NGX_OK;
}


static char *
ngx_http_advertise_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_advertise_conf_t  *prev = parent;
    ngx_http_advertise_conf_t  *conf = child;
    ngx_http_variable_t        *var;
    
    ngx_conf_merge_value(conf->inject, prev->inject, 0);    
    ngx_conf_merge_size_value(conf->buffer_size, prev->buffer_size, 1 * 1024 * 1024);
    ngx_conf_merge_ptr_value(conf->advertise_array, prev->advertise_array, NGX_CONF_UNSET_PTR);
    ngx_conf_merge_uint_value(conf->max_advertise_len, prev->max_advertise_len, 0);
    ngx_conf_merge_uint_value(conf->flag, prev->flag, 0);
    

    var = ngx_http_add_variable(cf, &g_advertise_inject_ctx_s, NGX_HTTP_VAR_CHANGEABLE);
    if (var == NULL) {
        return NGX_CONF_ERROR;
    }

    conf->index = ngx_http_get_variable_index(cf, &g_advertise_inject_ctx_s);
    if (conf->index == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }
    
    var->get_handler = ngx_http_g_ctx_gethandler;
    var->data = conf->index;

    var = ngx_http_add_variable(cf,  &g_advertise_status_s, NGX_HTTP_VAR_CHANGEABLE);
    var->get_handler = ngx_http_status_gethandler;
    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_advertise_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_advertise_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_advertise_body_filter;

    return NGX_OK;
}



