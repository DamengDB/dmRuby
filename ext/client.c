#include <dm_ext.h>

#include <time.h>
#include <errno.h>
#ifndef _WIN32
#include <sys/types.h>
#include <sys/socket.h>
#endif
#ifndef _MSC_VER
#include <unistd.h>
#endif
#include <fcntl.h>
#include "dm_enc_name_to_ruby.h"

VALUE cdmClient;
extern VALUE mdm, cdmError;
static VALUE sym_id, sym_version, sym_header_version, sym_async, sym_symbolize_keys, sym_as, sym_array, sym_stream;
static VALUE sym_no_good_index_used, sym_no_index_used, sym_query_was_slow;
static ID intern_brackets, intern_merge, intern_merge_bang, intern_new_with_args,
  intern_current_query_options, intern_read_timeout;

#define REQUIRE_INITIALIZED(wrapper) \
  if (!wrapper->initialized) { \
    rb_raise(cdmError, "DM client is not initialized"); \
  }

#define CONNECTED(wrapper) (wrapper->active == 1 && wrapper->closed != 1)

#define REQUIRE_CONNECTED(wrapper) \
  REQUIRE_INITIALIZED(wrapper) \
  if ((wrapper->closed) && !wrapper->reconnect_enabled) { \
    rb_raise(cdmError, "DM client is not connected"); \
  }

#define REQUIRE_NOT_CONNECTED(wrapper) \
  REQUIRE_INITIALIZED(wrapper) \
  if ((wrapper->active)) { \
    rb_raise(cdmError, "DM connection is already open"); \
  }

#define DM_LINK_VERSION 8

/*
 * used to pass all arguments to dm_real_connect while inside
 * rb_thread_call_without_gvl
 */
struct nogvl_connect_args {
  dm_client_wrapper *wrapper;
  const char *server;
  const char *user;
  const char *passwd;
};

/*
 * used to pass all arguments to dm_send_query while inside
 * rb_thread_call_without_gvl
 */
struct nogvl_send_query_args {
  dhcon *con;
  VALUE sql;
  const char *sql_ptr;
  long sql_len;
  dm_client_wrapper *wrapper;
};


/*
 * non-blocking dm_*() functions that we won't be wrapping since
 * they do not appear to hit the network nor issue any interruptible
 * or blocking system calls.
 *
 * - dm_affected_rows()
 * - dm_error()
 * - dm_fetch_fields()
 * - dm_fetch_lengths() - calls cli_fetch_lengths or emb_fetch_lengths
 * - dm_field_count()
 * - dm_get_client_info()
 * - dm_get_client_version()
 * - dm_get_server_info()
 * - dm_get_server_version()
 * - dm_insert_id()
 * - dm_num_fields()
 * - dm_num_rows()
 * - dm_options()
 * - dm_real_escape_string()
 * - dm_ssl_set()
 */

static void rb_dm_client_mark(void * wrapper) 
{
  dm_client_wrapper * w = wrapper;
  if (w) {
    rb_gc_mark_movable(w->encoding);
    rb_gc_mark_movable(w->active_fiber);
  }
}

/* this is called during GC */
static void rb_dm_client_free(void *ptr) {
  dm_client_wrapper *wrapper = ptr;
  decr_dm_client(wrapper);
}

static size_t rb_dm_client_memsize(const void * wrapper) {
  const dm_client_wrapper * w = wrapper;
  return sizeof(*w);
}

static void rb_dm_client_compact(void * wrapper) {
  dm_client_wrapper * w = wrapper;
  if (w) {
    rb_dm_gc_location(w->encoding);
    rb_dm_gc_location(w->active_fiber);
  }
}

const rb_data_type_t rb_dm_client_type = {
  "rb_dm_client",
  {
    rb_dm_client_mark,
    rb_dm_client_free,
    rb_dm_client_memsize,
#ifdef HAVE_RB_GC_MARK_MOVABLE
    rb_dm_client_compact,
#endif
  },
  0,
  0,
#ifdef RUBY_TYPED_FREE_IMMEDIATELY
  RUBY_TYPED_FREE_IMMEDIATELY,
#endif
};

struct nogvl_errors_args {
  void* hndl;
  sdbyte errormsg[4096];
  sdint4 errorCode;
  sdint2 hndl_type;
};

static void *nogvl_get_error(void *ptr) {
  struct nogvl_errors_args *args = ptr;
  sdbyte error_buf[4096];
  dpi_get_diag_rec(args->hndl_type, args->hndl, 1, &args->errorCode, error_buf, sizeof(error_buf), NULL);
  sprintf(args->errormsg,"[CODE:%d]%s",args->errorCode, error_buf);
  return (void*)Qtrue;
}


static VALUE rb_raise_dm_error(dm_client_wrapper *wrapper, void* hndl, sdint2 hndl_type) {
  VALUE rb_error_msg;
  VALUE e;
  const struct dm_enc_name_to_rb_map *dmrb;
  struct nogvl_errors_args err;

  err.hndl = hndl;
  err.hndl_type = hndl_type;

  rb_thread_call_without_gvl(nogvl_get_error, &err, RUBY_UBF_IO, 0);
  rb_error_msg = rb_str_new2(err.errormsg);

  dmrb = dm_enc_name_to_rb(wrapper->encode_code);
  rb_enc_associate(rb_error_msg, rb_enc_find(dmrb->rb_name));
  e = rb_funcall(cdmError, intern_new_with_args, 2,
                 rb_error_msg,
                 UINT2NUM(err.errorCode));
  rb_exc_raise(e);
}

static void *nogvl_connect(void *ptr) {
  struct nogvl_connect_args *args = ptr;
  DPIRETURN rt;
  rt = dpi_alloc_env(&args->wrapper->env);
  if(!(DSQL_SUCCEEDED(rt)))
    return (void*)Qfalse;
  rt = dpi_alloc_con(args->wrapper->env,&args->wrapper->client);
  if(!(DSQL_SUCCEEDED(rt)))
    return (void*)Qfalse;
  rt = dpi_set_con_attr(args->wrapper->client,DSQL_ATTR_LOCAL_CODE,(dpointer)args->wrapper->encode_code,0);
  if(!(DSQL_SUCCEEDED(rt)))
      return (void*)Qfalse;
  rt = dpi_login(args->wrapper->client,args->server,args->user,args->passwd);
  if(!(DSQL_SUCCEEDED(rt)))
      return (void*)Qfalse;
  rt = dpi_set_con_attr(args->wrapper->client,DSQL_ATTR_AUTOCOMMIT,(dpointer)1,0);
  if(!(DSQL_SUCCEEDED(rt)))
      return (void*)Qfalse;

  return(void*)Qtrue;
}

static void *nogvl_close(void *ptr) {
  dm_client_wrapper *wrapper = ptr;

  if (wrapper->initialized && !wrapper->closed) {
    dpi_logout(wrapper->client);
    dpi_free_con(wrapper->client);
    dpi_free_env(wrapper->env);
    wrapper->closed = 1;
    wrapper->active = 0;
    wrapper->reconnect_enabled = 0;
    wrapper->active_fiber = Qnil;
  }

  return NULL;
}

void decr_dm_client(dm_client_wrapper *wrapper)
{
  wrapper->refcount--;

  if (wrapper->refcount == 0) {
    xfree(wrapper);
  }
}

static VALUE allocate(VALUE klass) {
  VALUE obj;
  dm_client_wrapper * wrapper;
#ifdef NEW_TYPEDDATA_WRAPPER
  obj = TypedData_Make_Struct(klass, dm_client_wrapper, &rb_dm_client_type, wrapper);
#else
  obj = Data_Make_Struct(klass, dm_client_wrapper, rb_dm_client_mark, rb_dm_client_free, wrapper);
#endif
  wrapper->encoding = Qnil;
  wrapper->active_fiber = Qnil;
  wrapper->server_version = 0;
  wrapper->reconnect_enabled = 0;
  wrapper->connect_timeout = 0;
  wrapper->initialized = 0; /* will be set true after calling dm_init */
  wrapper->closed = 1; /* will be set false after calling dm_real_connect */
  wrapper->refcount = 1;
  wrapper->active = 0;

  return obj;
}

static VALUE rb_dm_connect(VALUE self, VALUE user, VALUE pass, VALUE servers) {
  struct nogvl_connect_args args;
  time_t start_time, end_time, elapsed_time, connect_timeout;
  VALUE rv;
  GET_CLIENT(self);

  args.server     = NIL_P(servers)  ? NULL : StringValueCStr(servers);
  args.user        = NIL_P(user)     ? NULL : StringValueCStr(user);
  args.passwd      = NIL_P(pass)     ? NULL : StringValueCStr(pass);
  args.wrapper    = wrapper;

  rv = (VALUE) rb_thread_call_without_gvl(nogvl_connect, &args, RUBY_UBF_IO, 0);

  if (rv == Qfalse)
    rb_raise_dm_error(wrapper, wrapper->client, DSQL_HANDLE_DBC);
  wrapper->closed = 0;
  wrapper->server_version = 8;
  wrapper->active = 1;
  wrapper->initialized = 1;
  return self;
}

/* call-seq:
 *    client.closed
 *
 * @return [Boolean]
 */
static VALUE rb_dm_client_close(VALUE self) {
  GET_CLIENT(self);

  if (wrapper->client) {
    rb_thread_call_without_gvl(nogvl_close, wrapper, RUBY_UBF_IO, 0);
  }

  return Qnil;
}

/* call-seq:
 *    client.closed?
 *
 * @return [Boolean]
 */
static VALUE rb_dm_client_closed(VALUE self) {
  GET_CLIENT(self);
  return CONNECTED(wrapper) ? Qfalse : Qtrue;
}


static void *nogvl_send_query(void *ptr) {
  struct nogvl_send_query_args *args = ptr;
  DPIRETURN rt;
  dhstmt hstmt;
  udint2 col_num;
  sdint4 nStmtType;
  sdbyte lastrowid[12];
  udint4 len;

  args->wrapper->stmt = NULL;
  rt = dpi_alloc_stmt(args->con,&hstmt);
  if(!DSQL_SUCCEEDED(rt))
  {
    return (void*)Qfalse;
  }
  args->wrapper->stmt = hstmt;

  rt = dpi_set_stmt_attr(hstmt,DSQL_ATTR_CURSOR_TYPE,(dpointer)DSQL_CURSOR_DYNAMIC,0);
  if(!DSQL_SUCCEEDED(rt))
  {
    return (void*)Qfalse;
  }
  rt = dpi_exec_direct(hstmt,args->sql_ptr);
  if(!DSQL_SUCCEEDED(rt))
  {
    return (void*)Qfalse;
  }
  rt = dpi_get_diag_field(DSQL_HANDLE_STMT, hstmt, 0,
                            DSQL_DIAG_DYNAMIC_FUNCTION_CODE,
                            (dpointer)&nStmtType, 0, NULL);
  if(!DSQL_SUCCEEDED(rt))
  {
    return (void*)Qfalse;
  }
  //lastrowid
  if(nStmtType == DSQL_DIAG_FUNC_CODE_INSERT ||
     nStmtType == DSQL_DIAG_FUNC_CODE_UPDATE ||
     nStmtType == DSQL_DIAG_FUNC_CODE_DELETE)
  {
    rt = dpi_get_diag_field(DSQL_HANDLE_STMT, hstmt, 0, DSQL_DIAG_ROWID, &lastrowid, sizeof(lastrowid), NULL);
    rt = dpi_rowid_to_char(args->con,lastrowid,sizeof(lastrowid),args->wrapper->lastrowid,sizeof(args->wrapper->lastrowid),&len);
  }
  else
  {
    strncpy(args->wrapper->lastrowid, "", 20);
  }
  
  // affected_rows
  if(nStmtType == DSQL_DIAG_FUNC_CODE_INSERT ||
     nStmtType == DSQL_DIAG_FUNC_CODE_UPDATE ||
     nStmtType == DSQL_DIAG_FUNC_CODE_DELETE ||
     nStmtType == DSQL_DIAG_FUNC_CODE_CALL)
  {
    rt = dpi_row_count(hstmt,&args->wrapper->affected_rows);
  }
  else{
    args->wrapper->affected_rows = 0;
  }
  //if have a result
   if(nStmtType == DSQL_DIAG_FUNC_CODE_SELECT ||
     nStmtType == DSQL_DIAG_FUNC_CODE_CALL)
  {
    args->wrapper->is_select = 1;
  }
 
  return (void*)Qtrue;
}

static VALUE do_send_query(VALUE args) {
  struct nogvl_send_query_args *query_args = (void *)args;
  dm_client_wrapper *wrapper = query_args->wrapper;
  wrapper->is_select = 0;
  if ((VALUE)rb_thread_call_without_gvl(nogvl_send_query, query_args, RUBY_UBF_IO, 0) == Qfalse) {
    if(wrapper->stmt == NULL)
       rb_raise(cdmError, "failed to alloc statement");
    else
       rb_raise_dm_error(wrapper, wrapper->stmt, DSQL_HANDLE_STMT);
  }
  return Qnil;
}

/* call-seq:
 *    client.query(sql)
 *
 * Query the database with +sql+, with optional +options+.  For the possible
 * options, see default_query_options on the dm::Client class.
 */
static VALUE rb_dm_query(VALUE self, VALUE sql, VALUE options) {
  struct nogvl_send_query_args args;
  VALUE resultobj;
  GET_CLIENT(self);

  REQUIRE_CONNECTED(wrapper);
  args.con = wrapper->client;
 
  Check_Type(sql, T_STRING);
  /* ensure the string is in the encoding the connection is expecting */
  args.sql = rb_str_export_to_enc(sql, rb_to_encoding(wrapper->encoding));
  args.sql_ptr = RSTRING_PTR(args.sql);
  args.sql_len = RSTRING_LEN(args.sql);
  args.wrapper = wrapper;

  do_send_query((VALUE)&args);
  (void)RB_GC_GUARD(sql);

  if(wrapper->is_select == 1)
  {
    resultobj = rb_dm_result_to_obj(self, wrapper->encoding, wrapper->stmt, 0 , options);
    wrapper->stmt = NULL;
    return resultobj;
  }
  else
  {
    dpi_free_stmt(wrapper->stmt);
    wrapper->stmt = NULL;
  }

  /* this will just block until the result is ready */
  return Qnil;
}


/* call-seq:
 *    client.last_id
 *
 * Returns the value generated for an AUTO_INCREMENT column by the previous INSERT or UPDATE
 * statement.
 */
static VALUE rb_dm_client_last_id(VALUE self) {
  GET_CLIENT(self);
  REQUIRE_CONNECTED(wrapper);
  return rb_str_new2(wrapper->lastrowid);
}


/* call-seq:
 *    client.affected_rows
 *
 * returns the number of rows changed, deleted, or inserted by the last statement
 * if it was an UPDATE, DELETE, or INSERT.
 */
static VALUE rb_dm_client_affected_rows(VALUE self) {
  GET_CLIENT(self);
  REQUIRE_CONNECTED(wrapper);
  return ULL2NUM(wrapper->affected_rows);
}


/* call-seq:
 *    client.encoding
 *
 * Returns the encoding set on the client.
 */
static VALUE rb_dm_client_encoding(VALUE self) {
  GET_CLIENT(self);
  return wrapper->encoding;
}

static VALUE set_charset_name(VALUE self, VALUE value) {
  int code;
  DPIRETURN rt;
  const struct dm_enc_name_to_rb_map *dmrb;
  rb_encoding *enc;
  VALUE rb_enc;
  GET_CLIENT(self);

  code = NUM2INT(value);
  if(code<1 || code>11)
    code = 1;
  dmrb = dm_enc_name_to_rb(code);
  if (dmrb == NULL || dmrb->rb_name == NULL) {
    rb_raise(cdmError, "Unsupported charset");
  } else {
    enc = rb_enc_find(dmrb->rb_name);
    rb_enc = rb_enc_from_encoding(enc);
    wrapper->encoding = rb_enc;
  }

  wrapper->encode_code = code;
  return value;
}

/* call-seq: client.prepare # => dm::Statement
 *
 * Create a new prepared statement.
 */
static VALUE rb_dm_client_prepare_statement(VALUE self, VALUE sql) {
  GET_CLIENT(self);
  REQUIRE_CONNECTED(wrapper);
  VALUE obj = rb_dm_stmt_new(self,sql);
  return obj;
}

void init_dm_client() {
#if 0
  mdm      = rb_define_module("dm"); Teach RDoc about dm constant.
#endif
  cdmClient = rb_define_class_under(mdm, "Client", rb_cObject);
  rb_global_variable(&cdmClient);
  rb_define_alloc_func(cdmClient, allocate);

  rb_define_method(cdmClient, "close", rb_dm_client_close, 0);
  rb_define_method(cdmClient, "closed?", rb_dm_client_closed, 0);
  rb_define_method(cdmClient, "last_id", rb_dm_client_last_id, 0);
  rb_define_method(cdmClient, "affected_rows", rb_dm_client_affected_rows, 0);
  rb_define_method(cdmClient, "prepare", rb_dm_client_prepare_statement, 1);
  rb_define_method(cdmClient, "encoding", rb_dm_client_encoding, 0);

  rb_define_private_method(cdmClient, "charset_name=", set_charset_name, 1);
  rb_define_private_method(cdmClient, "connect", rb_dm_connect, 3);
  rb_define_private_method(cdmClient, "_query", rb_dm_query, 2);

  sym_id              = ID2SYM(rb_intern("id"));
  sym_version         = ID2SYM(rb_intern("version"));
  sym_header_version  = ID2SYM(rb_intern("header_version"));
  sym_async           = ID2SYM(rb_intern("async"));
  sym_symbolize_keys  = ID2SYM(rb_intern("symbolize_keys"));
  sym_as              = ID2SYM(rb_intern("as"));
  sym_array           = ID2SYM(rb_intern("array"));
  sym_stream          = ID2SYM(rb_intern("stream"));

  sym_no_good_index_used = ID2SYM(rb_intern("no_good_index_used"));
  sym_no_index_used      = ID2SYM(rb_intern("no_index_used"));
  sym_query_was_slow     = ID2SYM(rb_intern("query_was_slow"));

  intern_brackets = rb_intern("[]");
  intern_merge = rb_intern("merge");
  intern_merge_bang = rb_intern("merge!");
  intern_new_with_args = rb_intern("new_with_args");
  intern_current_query_options = rb_intern("@current_query_options");
  intern_read_timeout = rb_intern("@read_timeout");

  rb_const_set(cdmClient, rb_intern("SECURE_CONNECTION"), LONG2NUM(0));

}
