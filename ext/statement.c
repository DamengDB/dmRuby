#include <dm_ext.h>

extern VALUE mdm, cdmError;
static VALUE cdmStatement, cBigDecimal, cDateTime, cDate;
static VALUE sym_stream, intern_new_with_args, intern_each, intern_to_s, intern_merge_bang;
static VALUE intern_sec_fraction, intern_usec, intern_sec, intern_min, intern_hour, intern_day, intern_month, intern_year,
  intern_query_options;

#ifndef NEW_TYPEDDATA_WRAPPER
#define TypedData_Get_Struct(obj, type, ignore, sval) Data_Get_Struct(obj, type, sval)
#endif

#define GET_STATEMENT(self) \
  dm_stmt_wrapper *stmt_wrapper; \
  TypedData_Get_Struct(self, dm_stmt_wrapper, &rb_dm_statement_type, stmt_wrapper); \
  if (!stmt_wrapper->stmt) { rb_raise(cdmError, "Invalid statement handle"); } \
  if (stmt_wrapper->closed) { rb_raise(cdmError, "Statement handle already closed"); }

static void rb_dm_stmt_mark(void * ptr) {
  dm_stmt_wrapper *stmt_wrapper = ptr;
  if (!stmt_wrapper) return;

  rb_gc_mark_movable(stmt_wrapper->client);
}

static void rb_dm_stmt_free(void *ptr) {
  dm_stmt_wrapper *stmt_wrapper = ptr;
  decr_dm_stmt(stmt_wrapper);
}
    
static size_t rb_dm_stmt_memsize(const void * ptr) {
  const dm_stmt_wrapper *stmt_wrapper = ptr;
  return sizeof(*stmt_wrapper);
}

#ifdef HAVE_RB_GC_MARK_MOVABLE
static void rb_dm_stmt_compact(void * ptr) {
  dm_stmt_wrapper *stmt_wrapper = ptr;
  if (!stmt_wrapper) return;

  rb_dm_gc_location(stmt_wrapper->client);
}
#endif

static const rb_data_type_t rb_dm_statement_type = {
  "rb_dm_statement",
  {
    rb_dm_stmt_mark,
    rb_dm_stmt_free,
    rb_dm_stmt_memsize,
#ifdef HAVE_RB_GC_MARK_MOVABLE
    rb_dm_stmt_compact,
#endif
  },
  0,
  0,
#ifdef RUBY_TYPED_FREE_IMMEDIATELY
  RUBY_TYPED_FREE_IMMEDIATELY,
#endif
};

static void *nogvl_stmt_close(void *ptr) {
  dm_stmt_wrapper *stmt_wrapper = ptr;
  {
    GET_CLIENT(stmt_wrapper->client);
    if (stmt_wrapper->stmt && wrapper->closed != 1) {
      dpi_free_stmt(stmt_wrapper->stmt);
      stmt_wrapper->stmt = NULL;
    }
    return NULL;
  }
}

void decr_dm_stmt(dm_stmt_wrapper *stmt_wrapper) {
  stmt_wrapper->refcount--;

  if (stmt_wrapper->refcount == 0) 
  {
    if (stmt_wrapper->paramdesc != NULL)
    {
      xfree(stmt_wrapper->paramdesc);
      stmt_wrapper->paramdesc = NULL;
    }

    if (stmt_wrapper->stmt) 
    {
      dpi_free_stmt(stmt_wrapper->stmt);
      stmt_wrapper->stmt = NULL;
    }
    xfree(stmt_wrapper);
  }
}

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


static VALUE rb_raise_dm_stmt_error(dm_stmt_wrapper *wrapper, void* hndl, sdint2 hndl_type) {
  VALUE rb_error_msg;
  VALUE e;
  struct nogvl_errors_args err;
  err.hndl = hndl;
  err.hndl_type = hndl_type;

  rb_thread_call_without_gvl(nogvl_get_error, &err, RUBY_UBF_IO, 0);
  rb_error_msg = rb_str_new2(err.errormsg);

  rb_enc_associate(rb_error_msg, rb_utf8_encoding());
  e = rb_funcall(cdmError, intern_new_with_args, 2,
                 rb_error_msg,
                 UINT2NUM(err.errorCode));
  rb_exc_raise(e);
}

/*
 * used to pass all arguments to dm_stmt_prepare while inside
 * nogvl_prepare_statement_args
 */
struct nogvl_prepare_statement_args {
  dhstmt stmt;
  VALUE sql;
  const char *sql_ptr;
  unsigned long sql_len;
};

static void *nogvl_prepare_statement(void *ptr) {
  struct nogvl_prepare_statement_args *args = ptr;
  DPIRETURN rt;
  rt = dpi_prepare(args->stmt,args->sql_ptr);
  if (!(DSQL_SUCCEEDED(rt))) {
    return (void*)Qfalse;
  } else {
    return (void*)Qtrue;
  }
}

static void *nogvl_alloc_stmt(void *ptr) {
  dm_stmt_wrapper *stmt_wrapper = ptr;
  DPIRETURN rt;
  GET_CLIENT(stmt_wrapper->client);
  rt = dpi_alloc_stmt(wrapper->client,&stmt_wrapper->stmt);
  if(!DSQL_SUCCEEDED(rt))
  {
    return (void*)Qfalse;
  }
  rt = dpi_set_stmt_attr(stmt_wrapper->stmt,DSQL_ATTR_CURSOR_TYPE,(dpointer)DSQL_CURSOR_DYNAMIC,0);
  if(!DSQL_SUCCEEDED(rt))
  {
    return (void*)Qfalse;
  }
  return (void*)Qtrue;
}

static void *nogvl_get_param_desc(void *ptr) {
  dm_stmt_wrapper *stmt_wrapper = ptr;
  DPIRETURN rt;
  udint2 i;
  rt = dpi_number_params(stmt_wrapper->stmt,&stmt_wrapper->param_num);
  if(!DSQL_SUCCEEDED(rt))
  {
    return (void*)Qfalse;
  }

  stmt_wrapper->paramdesc = (ParamDesc*)xcalloc(stmt_wrapper->param_num,sizeof(ParamDesc));
  for(i = 0; i < stmt_wrapper->param_num; i++)
  {
      rt = dpi_desc_param(
                stmt_wrapper->stmt, i + 1, &stmt_wrapper->paramdesc[i].sql_type,
                &stmt_wrapper->paramdesc[i].prec, &stmt_wrapper->paramdesc[i].scale,
                &stmt_wrapper->paramdesc[i].nullable);
      if(!DSQL_SUCCEEDED(rt))
      {
        return (void*)Qfalse;
      }
  }

  rt = dpi_number_columns(stmt_wrapper->stmt,&stmt_wrapper->col_num);
  if(!DSQL_SUCCEEDED(rt))
  {
    return (void*)Qfalse;
  }
  return (void*)Qtrue;
}

VALUE rb_dm_stmt_new(VALUE rb_client, VALUE sql) {
  dm_stmt_wrapper *stmt_wrapper;
  VALUE rb_stmt;
  rb_encoding *conn_enc;

  Check_Type(sql, T_STRING);

#ifdef NEW_TYPEDDATA_WRAPPER
  rb_stmt = TypedData_Make_Struct(cdmStatement, dm_stmt_wrapper, &rb_dm_statement_type, stmt_wrapper);
#else
  rb_stmt = Data_Make_Struct(cdmStatement, dm_stmt_wrapper, rb_dm_stmt_mark, rb_dm_stmt_free, stmt_wrapper);
#endif
  {
    stmt_wrapper->client = rb_client;
    stmt_wrapper->refcount = 1;
    stmt_wrapper->closed = 0;
    stmt_wrapper->stmt = NULL;
    stmt_wrapper->param_num = 0;
    stmt_wrapper->paramdesc = NULL;
    stmt_wrapper->affected_rows = 0;
    stmt_wrapper->is_select = 0;
  }

  // instantiate stmt
  {
    GET_CLIENT(rb_client);
    rb_thread_call_without_gvl(nogvl_alloc_stmt, stmt_wrapper, RUBY_UBF_IO, 0);
    conn_enc = rb_to_encoding(wrapper->encoding);
  }
  if (stmt_wrapper->stmt == NULL) {
    rb_raise(cdmError, "Unable to initialize prepared statement: out of memory");
  }

  // call dm_stmt_prepare w/o gvl
  {
    struct nogvl_prepare_statement_args args;
    args.stmt = stmt_wrapper->stmt;
    // ensure the string is in the encoding the connection is expecting
    args.sql = rb_str_export_to_enc(sql, conn_enc);
    args.sql_ptr = RSTRING_PTR(sql);
    args.sql_len = RSTRING_LEN(sql);

    if ((VALUE)rb_thread_call_without_gvl(nogvl_prepare_statement, &args, RUBY_UBF_IO, 0) == Qfalse) {
      rb_raise_dm_stmt_error(stmt_wrapper, stmt_wrapper->stmt, DSQL_HANDLE_STMT);
    }

    if ((VALUE)rb_thread_call_without_gvl(nogvl_get_param_desc, stmt_wrapper, RUBY_UBF_IO, 0) == Qfalse) {
      rb_raise(cdmError, "failed to get param desc");
    }
  }

  return rb_stmt;
}

/* call-seq: stmt.param_count # => Numeric
 *
 * Returns the number of parameters the prepared statement expects.
 */
static VALUE rb_dm_stmt_param_count(VALUE self) {
  GET_STATEMENT(self);

  return UINT2NUM(stmt_wrapper->param_num);
}

/* call-seq: stmt.field_count # => Numeric
 *
 * Returns the number of fields the prepared statement returns.
 */
static VALUE rb_dm_stmt_field_count(VALUE self) {
  GET_STATEMENT(self);

  return UINT2NUM(stmt_wrapper->col_num);
}

static void *nogvl_stmt_execute(void *ptr) {
  dm_stmt_wrapper *stmt_wrapper = ptr;
  GET_CLIENT(stmt_wrapper->client);
  DPIRETURN rt;
  sdint4 nStmtType;
  udint2 col_num;
  sdbyte lastrowid[12];
  udint4 len;

  rt = dpi_close_cursor(stmt_wrapper->stmt);

  rt = dpi_exec(stmt_wrapper->stmt);

  if (!DSQL_SUCCEEDED(rt)) {
    return (void*)Qfalse;
  } 
  rt = dpi_get_diag_field(DSQL_HANDLE_STMT, stmt_wrapper->stmt, 0,
                            DSQL_DIAG_DYNAMIC_FUNCTION_CODE,
                            (dpointer)&nStmtType, 0, NULL);

  if(nStmtType == DSQL_DIAG_FUNC_CODE_INSERT ||
     nStmtType == DSQL_DIAG_FUNC_CODE_UPDATE ||
     nStmtType == DSQL_DIAG_FUNC_CODE_DELETE)
  {
    rt = dpi_get_diag_field(DSQL_HANDLE_STMT, stmt_wrapper->stmt, 0, DSQL_DIAG_ROWID, &lastrowid, sizeof(lastrowid), NULL);
    if (!DSQL_SUCCEEDED(rt)) {
      return (void*)Qfalse;
    } 
    rt = dpi_rowid_to_char(wrapper->client,lastrowid,sizeof(lastrowid),stmt_wrapper->lastrowid,sizeof(stmt_wrapper->lastrowid),&len);
     if (!DSQL_SUCCEEDED(rt)) {
      return (void*)Qfalse;
    }
     strncpy(wrapper->lastrowid, stmt_wrapper->lastrowid, 20);
  }
  else
  {
    strncpy(stmt_wrapper->lastrowid, "", 20);
    strncpy(wrapper->lastrowid, "", 20);
  }
  
  // affected_rows
    if(nStmtType == DSQL_DIAG_FUNC_CODE_INSERT ||
     nStmtType == DSQL_DIAG_FUNC_CODE_UPDATE ||
     nStmtType == DSQL_DIAG_FUNC_CODE_DELETE ||
     nStmtType == DSQL_DIAG_FUNC_CODE_CALL)
  {
    rt = dpi_row_count(stmt_wrapper->stmt,&stmt_wrapper->affected_rows);
     if (!DSQL_SUCCEEDED(rt)) {
      return (void*)Qfalse;
    }
     wrapper->affected_rows = stmt_wrapper->affected_rows;
  }
  else
  {
    stmt_wrapper->affected_rows = 0;
    wrapper->affected_rows = 0;
  }      

  rt = dpi_number_columns(stmt_wrapper->stmt, &stmt_wrapper->col_num);   
   if (!DSQL_SUCCEEDED(rt)) {
    return (void*)Qfalse;
  } 
  if(stmt_wrapper->col_num > 0)
    stmt_wrapper->is_select = 1;
  
  return (void*)Qtrue;
}

static void set_buffer_for_string(dm_BIND* bind_buffer, unsigned long *length_buffer, VALUE string) {
  unsigned long length;

  bind_buffer->buffer = RSTRING_PTR(string);

  length = RSTRING_LEN(string);
  bind_buffer->buffer_length = length;
  *length_buffer = length;
}

/* Free each bind_buffer[i].buffer except when params_enc is non-nil, this means
 * the buffer is a Ruby string pointer and not our memory to manage.
 */
#define FREE_BINDS                                          \
  for (i = 0; i < bind_count; i++) {                        \
    if (bind_buffers[i].buffer && NIL_P(params_enc[i])) {   \
      xfree(bind_buffers[i].buffer);                        \
    }                                                       \
  }                                                         \
  if (argc > 0) {                                           \
    xfree(bind_buffers);                                    \
    xfree(length_buffers);                                  \
  }

/* return 0 if the given bignum can cast as LONG_LONG, otherwise 1 */
static int my_big2ll(VALUE bignum, sdint8  *ptr)
{
  sdint8 num;
  size_t len;
// rb_absint_size was added in 2.1.0. See:
// https://github.com/ruby/ruby/commit/9fea875
  int nlz_bits = 0;
  len = rb_absint_size(bignum, &nlz_bits);

  if (len > sizeof(sdint8)) goto overflow;
  if (RBIGNUM_POSITIVE_P(bignum)) {
    goto overflow;
  }
  else {
    if (len == 8 &&
#ifdef HAVE_RB_ABSINT_SIZE
        nlz_bits == 0 &&
#endif
// rb_absint_singlebit_p was added in 2.1.0. See:
// https://github.com/ruby/ruby/commit/e5ff9d5
#if defined(HAVE_RB_ABSINT_SIZE) && defined(HAVE_RB_ABSINT_SINGLEBIT_P)
        /* Optimized to avoid object allocation for Ruby 2.1+
         * only -0x8000000000000000 is safe if `len == 8 && nlz_bits == 0`
         */
        !rb_absint_singlebit_p(bignum)
#else
        rb_big_cmp(bignum, LL2NUM(LLONG_MIN)) == INT2FIX(-1)
#endif
       ) {
      goto overflow;
    }
    *ptr = rb_big2ll(bignum);
  }
  return 0;
overflow:
  return 1;
}

/* call-seq: stmt.execute
 *
 * Executes the current prepared statement, returns +result+.
 */
static VALUE rb_dm_stmt_execute(int argc, VALUE *argv, VALUE self) {
  dm_BIND *bind_buffers = NULL;
  unsigned long *length_buffers = NULL;
  udint2 bind_count;
  udint2 i;
  dhstmt stmt;
  VALUE opts;
  VALUE current;
  VALUE resultObj;
  VALUE *params_enc = NULL;
  rb_encoding *conn_enc;
  DPIRETURN rt;
  rb_encoding *enc;

  GET_STATEMENT(self);
  GET_CLIENT(stmt_wrapper->client);

  conn_enc = rb_to_encoding(wrapper->encoding);

  if(wrapper->closed == 1)
    rb_raise(cdmError, "Client is closed");

  stmt = stmt_wrapper->stmt;
  bind_count = stmt_wrapper->param_num;

  // Get count of ordinary arguments, and extract hash opts/keyword arguments
  // Use a local scope to avoid leaking the temporary count variable
  {
    int c = rb_scan_args(argc, argv, "*:", NULL, &opts);
    if (c != (long)bind_count) {
      rb_raise(cdmError, "Bind parameter count (%ld) doesn't match number of arguments (%d)", bind_count, c);
    }
  }

  // setup any bind variables in the query
  if (bind_count > 0) {
    // Scratch space for string encoding exports, allocate on the stack
    params_enc = alloca(sizeof(VALUE) * bind_count);
    bind_buffers = xcalloc(bind_count, sizeof(dm_BIND));
    length_buffers = xcalloc(bind_count, sizeof(unsigned long));

    for (i = 0; i < bind_count; i++) {
      bind_buffers[i].buffer = NULL;
      params_enc[i] = Qnil;

      switch (TYPE(argv[i])) {
        case T_NIL:
          bind_buffers[i].buffer = xmalloc(8192);
          strncpy(bind_buffers[i].buffer, "", 8192);
          rt = dpi_bind_param(stmt_wrapper->stmt, i + 1, DSQL_PARAM_INPUT, DSQL_C_NCHAR, stmt_wrapper->paramdesc[i].sql_type, stmt_wrapper->paramdesc[i].prec, stmt_wrapper->paramdesc[i].scale, bind_buffers[i].buffer, 8192, NULL);
          if(!DSQL_SUCCEEDED(rt))
            rb_raise(cdmError, "failed to bind param %d", i);
          break;
        case T_FIXNUM:
        #if SIZEOF_INT < SIZEOF_LONG
          bind_buffers[i].buffer = xmalloc(sizeof(sdint8));
          *(sdint8*)(bind_buffers[i].buffer) =(sdint8)FIX2LONG(argv[i]);
          rt = dpi_bind_param(stmt_wrapper->stmt, i + 1, DSQL_PARAM_INPUT, DSQL_C_SBIGINT, stmt_wrapper->paramdesc[i].sql_type, stmt_wrapper->paramdesc[i].prec, stmt_wrapper->paramdesc[i].scale, bind_buffers[i].buffer, sizeof(sdint8), NULL);
          if(!DSQL_SUCCEEDED(rt))
            rb_raise(cdmError, "failed to bind param %d", i);
          break;
        #else
          bind_buffers[i].buffer = xmalloc(sizeof(sdint8));
          *(sdint8*)(bind_buffers[i].buffer) =(sdint8)FIX2INT(argv[i]);
          rt = dpi_bind_param(stmt_wrapper->stmt, i + 1, DSQL_PARAM_INPUT, DSQL_C_SBIGINT, stmt_wrapper->paramdesc[i].sql_type, stmt_wrapper->paramdesc[i].prec, stmt_wrapper->paramdesc[i].scale, bind_buffers[i].buffer, sizeof(sdint8), NULL);
          if(!DSQL_SUCCEEDED(rt))
            rb_raise(cdmError, "failed to bind param %d", i);
          break;
        #endif
        case T_BIGNUM:
          {
            sdint8 num;
            if (my_big2ll(argv[i], &num) == 0) {
              bind_buffers[i].buffer = xmalloc(sizeof(sdint8));
              *(sdint8*)(bind_buffers[i].buffer) = num;
              rt = dpi_bind_param(stmt_wrapper->stmt, i + 1, DSQL_PARAM_INPUT, DSQL_C_SBIGINT, stmt_wrapper->paramdesc[i].sql_type, stmt_wrapper->paramdesc[i].prec, stmt_wrapper->paramdesc[i].scale, bind_buffers[i].buffer, sizeof(sdint8), NULL);
            } else {
              /* The bignum was larger than we can fit in LONG_LONG, send it as a string */
              params_enc[i] = rb_str_export_to_enc(rb_big2str(argv[i], 10), conn_enc);
              set_buffer_for_string(&bind_buffers[i], &length_buffers[i], params_enc[i]);
              rt = dpi_bind_param(stmt_wrapper->stmt, i + 1, DSQL_PARAM_INPUT, DSQL_C_NCHAR, stmt_wrapper->paramdesc[i].sql_type, stmt_wrapper->paramdesc[i].prec, stmt_wrapper->paramdesc[i].scale, bind_buffers[i].buffer, bind_buffers[i].buffer_length, NULL);
            }
          }
          if(!DSQL_SUCCEEDED(rt))
            rb_raise(cdmError, "failed to bind param %d", i);
          break;
        case T_FLOAT:
          bind_buffers[i].buffer = xmalloc(sizeof(double));
          *(double*)(bind_buffers[i].buffer) = NUM2DBL(argv[i]);
          rt = dpi_bind_param(stmt_wrapper->stmt, i + 1, DSQL_PARAM_INPUT, DSQL_C_DOUBLE, stmt_wrapper->paramdesc[i].sql_type, stmt_wrapper->paramdesc[i].prec, stmt_wrapper->paramdesc[i].scale, bind_buffers[i].buffer, sizeof(double), NULL);
          if(!DSQL_SUCCEEDED(rt))
            rb_raise(cdmError, "failed to bind param %d", i);
          break;
        case T_STRING:
          params_enc[i] = argv[i];
          enc = rb_enc_get(params_enc[i]);
          if (enc == rb_ascii8bit_encoding())
          {
            set_buffer_for_string(&bind_buffers[i], &length_buffers[i], params_enc[i]);
            rt = dpi_bind_param(stmt_wrapper->stmt, i + 1, DSQL_PARAM_INPUT, DSQL_C_BINARY, stmt_wrapper->paramdesc[i].sql_type, stmt_wrapper->paramdesc[i].prec, stmt_wrapper->paramdesc[i].scale, bind_buffers[i].buffer, bind_buffers[i].buffer_length, &(bind_buffers[i].buffer_length));
            if(!DSQL_SUCCEEDED(rt))
              rb_raise(cdmError, "failed to bind param %d", i);
          }
          else
          {
            params_enc[i] = rb_str_export_to_enc(params_enc[i], conn_enc);
            set_buffer_for_string(&bind_buffers[i], &length_buffers[i], params_enc[i]);
            rt = dpi_bind_param(stmt_wrapper->stmt, i + 1, DSQL_PARAM_INPUT, DSQL_C_NCHAR, stmt_wrapper->paramdesc[i].sql_type, stmt_wrapper->paramdesc[i].prec, stmt_wrapper->paramdesc[i].scale, bind_buffers[i].buffer, bind_buffers[i].buffer_length, &(bind_buffers[i].buffer_length));
            if(!DSQL_SUCCEEDED(rt))
              rb_raise(cdmError, "failed to bind param %d", i);
          }
          break;
        case T_TRUE:
          bind_buffers[i].buffer = xmalloc(sizeof(int));
          *(int*)(bind_buffers[i].buffer) = 1;
          rt = dpi_bind_param(stmt_wrapper->stmt, i + 1, DSQL_PARAM_INPUT, DSQL_C_SLONG, stmt_wrapper->paramdesc[i].sql_type, stmt_wrapper->paramdesc[i].prec, stmt_wrapper->paramdesc[i].scale, bind_buffers[i].buffer, sizeof(int), NULL);
          if(!DSQL_SUCCEEDED(rt))
            rb_raise(cdmError, "failed to bind param %d", i);
          break;
        case T_FALSE:
          bind_buffers[i].buffer = xmalloc(sizeof(int));
          *(int*)(bind_buffers[i].buffer) = 0;
          rt = dpi_bind_param(stmt_wrapper->stmt, i + 1, DSQL_PARAM_INPUT, DSQL_C_SLONG, stmt_wrapper->paramdesc[i].sql_type, stmt_wrapper->paramdesc[i].prec, stmt_wrapper->paramdesc[i].scale, bind_buffers[i].buffer, sizeof(int), NULL);
          if(!DSQL_SUCCEEDED(rt))
            rb_raise(cdmError, "failed to bind param %d", i);
          break;
        default:
          // TODO: what Ruby type should support dm_TYPE_TIME
          if (CLASS_OF(argv[i]) == rb_cTime || CLASS_OF(argv[i]) == cDateTime) {
            dpi_timestamp_t t;
            VALUE rb_time = argv[i];

            bind_buffers[i].buffer = xmalloc(sizeof(dpi_timestamp_t));

            memset(&t, 0, sizeof(dpi_timestamp_t));

            if (CLASS_OF(argv[i]) == rb_cTime) {
              t.fraction = FIX2INT(rb_funcall(rb_time, intern_usec, 0));
            } else if (CLASS_OF(argv[i]) == cDateTime) {
              t.fraction = NUM2DBL(rb_funcall(rb_time, intern_sec_fraction, 0)) * 1000000;
            }

            t.second = FIX2INT(rb_funcall(rb_time, intern_sec, 0));
            t.minute = FIX2INT(rb_funcall(rb_time, intern_min, 0));
            t.hour = FIX2INT(rb_funcall(rb_time, intern_hour, 0));
            t.day = FIX2INT(rb_funcall(rb_time, intern_day, 0));
            t.month = FIX2INT(rb_funcall(rb_time, intern_month, 0));
            t.year = FIX2INT(rb_funcall(rb_time, intern_year, 0));

            *(dpi_timestamp_t*)(bind_buffers[i].buffer) = t;
            rt = dpi_bind_param(stmt_wrapper->stmt, i + 1, DSQL_PARAM_INPUT, DSQL_C_TIMESTAMP, stmt_wrapper->paramdesc[i].sql_type, stmt_wrapper->paramdesc[i].prec, stmt_wrapper->paramdesc[i].scale, bind_buffers[i].buffer, sizeof(dpi_timestamp_t), NULL);
          } else if (CLASS_OF(argv[i]) == cDate) {
            dpi_date_t t;
            VALUE rb_time = argv[i];

            bind_buffers[i].buffer = xmalloc(sizeof(dpi_date_t));

            memset(&t, 0, sizeof(dpi_date_t));
            t.day = FIX2INT(rb_funcall(rb_time, intern_day, 0));
            t.month = FIX2INT(rb_funcall(rb_time, intern_month, 0));
            t.year = FIX2INT(rb_funcall(rb_time, intern_year, 0));

            *(dpi_date_t*)(bind_buffers[i].buffer) = t;
            rt = dpi_bind_param(stmt_wrapper->stmt, i + 1, DSQL_PARAM_INPUT, DSQL_C_DATE, stmt_wrapper->paramdesc[i].sql_type, stmt_wrapper->paramdesc[i].prec, stmt_wrapper->paramdesc[i].scale, bind_buffers[i].buffer, sizeof(dpi_date_t), NULL);
          } else if (CLASS_OF(argv[i]) == cBigDecimal) {
            VALUE rb_val_as_string = rb_funcall(argv[i], intern_to_s, 0);

            params_enc[i] = rb_val_as_string;
            params_enc[i] = rb_str_export_to_enc(params_enc[i], conn_enc);
            set_buffer_for_string(&bind_buffers[i], &length_buffers[i], params_enc[i]);
            rt = dpi_bind_param(stmt_wrapper->stmt, i + 1, DSQL_PARAM_INPUT, DSQL_C_NCHAR, stmt_wrapper->paramdesc[i].sql_type, stmt_wrapper->paramdesc[i].prec, stmt_wrapper->paramdesc[i].scale, bind_buffers[i].buffer, bind_buffers[i].buffer_length, NULL);
          }
          if(!DSQL_SUCCEEDED(rt))
            rb_raise(cdmError, "failed to bind param %d", i);
          break;
      }
    }
  }


  // From stmt_execute to dm_stmt_result_metadata to stmt_store_result, no
  // Ruby API calls are allowed so that GC is not invoked. If the connection is
  // in results-streaming-mode for Statement A, and in the middle Statement B
  // gets garbage collected, a message will be sent to the server notifying it
  // to release Statement B, resulting in the following error:
  //   Commands out of sync; you can't run this command now
  //
  // In streaming mode, statement execute must return a cursor because we
  // cannot prevent other Statement objects from being garbage collected
  // between fetches of each row of the result set. The following error
  // occurs if cursor mode is not set:
  //   Row retrieval was canceled by dm_stmt_close

  if ((VALUE)rb_thread_call_without_gvl(nogvl_stmt_execute, stmt_wrapper, RUBY_UBF_IO, 0) == Qfalse) {
    FREE_BINDS;
    rb_raise_dm_stmt_error(stmt_wrapper, stmt_wrapper->stmt, DSQL_HANDLE_STMT);
  }

  FREE_BINDS;


  if (stmt_wrapper->is_select != 1) {
    return Qnil;
  }

  resultObj = rb_dm_result_to_obj(stmt_wrapper->client, wrapper->encoding, stmt_wrapper->stmt, 1 , Qnil);
  return resultObj;
}


/* call-seq:
 *    stmt.last_id
 *
 * Returns the AUTO_INCREMENT value from the executed INSERT or UPDATE.
 */
static VALUE rb_dm_stmt_last_id(VALUE self) {
  GET_STATEMENT(self);
  return rb_str_new2(stmt_wrapper->lastrowid);
}

/* call-seq:
 *    stmt.affected_rows
 *
 * Returns the number of rows changed, deleted, or inserted.
 */
static VALUE rb_dm_stmt_affected_rows(VALUE self) {
  GET_STATEMENT(self);
  return ULL2NUM(stmt_wrapper->affected_rows);
}

/* call-seq:
 *    stmt.close
 *
 * Explicitly closing this will free up server resources immediately rather
 * than waiting for the garbage collector. Useful if you're managing your
 * own prepared statement cache.
 */
static VALUE rb_dm_stmt_close(VALUE self) {
  GET_STATEMENT(self);
  stmt_wrapper->closed = 1;
  rb_thread_call_without_gvl(nogvl_stmt_close, stmt_wrapper, RUBY_UBF_IO, 0);
  return Qnil;
}

void init_dm_statement() {
  cDate = rb_const_get(rb_cObject, rb_intern("Date"));
  rb_global_variable(&cDate);

  cDateTime = rb_const_get(rb_cObject, rb_intern("DateTime"));
  rb_global_variable(&cDateTime);

  cBigDecimal = rb_const_get(rb_cObject, rb_intern("BigDecimal"));
  rb_global_variable(&cBigDecimal);

  cdmStatement = rb_define_class_under(mdm, "Statement", rb_cObject);
  rb_undef_alloc_func(cdmStatement);
  rb_global_variable(&cdmStatement);

  rb_define_method(cdmStatement, "param_count", rb_dm_stmt_param_count, 0);
  rb_define_method(cdmStatement, "field_count", rb_dm_stmt_field_count, 0);
  rb_define_method(cdmStatement, "_execute", rb_dm_stmt_execute, -1);
  rb_define_method(cdmStatement, "last_id", rb_dm_stmt_last_id, 0);
  rb_define_method(cdmStatement, "affected_rows", rb_dm_stmt_affected_rows, 0);
  rb_define_method(cdmStatement, "close", rb_dm_stmt_close, 0);

  sym_stream = ID2SYM(rb_intern("stream"));

  intern_new_with_args = rb_intern("new_with_args");
  intern_each = rb_intern("each");

  intern_sec_fraction = rb_intern("sec_fraction");
  intern_usec = rb_intern("usec");
  intern_sec = rb_intern("sec");
  intern_min = rb_intern("min");
  intern_hour = rb_intern("hour");
  intern_day = rb_intern("day");
  intern_month = rb_intern("month");
  intern_year = rb_intern("year");

  intern_to_s = rb_intern("to_s");
  intern_merge_bang = rb_intern("merge!");
  intern_query_options = rb_intern("@query_options");
}
