#include <dm_ext.h>

static rb_encoding *binaryEncoding;

/* on 64bit platforms we can handle dates way outside 2038-01-19T03:14:07
 *
 * (9999*31557600) + (12*2592000) + (31*86400) + (11*3600) + (59*60) + 59
 */
#define dm_MAX_TIME 315578267999ULL

/* 0000-1-1 00:00:00 UTC
 *
 * (0*31557600) + (1*2592000) + (1*86400) + (0*3600) + (0*60) + 0
 */
#define dm_MIN_TIME 2678400ULL

#define dm_MAX_BYTES_PER_CHAR 3

/* From dm documentations:
 *   To distinguish between binary and nonbinary data for string data types,
 *   check whether the charsetnr value is 63. If so, the character set is binary,
 *   which indicates binary rather than nonbinary data. This enables you to distinguish BINARY
 *   from CHAR, VARBINARY from VARCHAR, and the BLOB types from the TEXT types.
 */
#define dm_BINARY_CHARSET 63

#ifndef DSQL_JSON
#define DSQL_JSON 245
#endif

#ifndef NEW_TYPEDDATA_WRAPPER
#define TypedData_Get_Struct(obj, type, ignore, sval) Data_Get_Struct(obj, type, sval)
#endif

#define GET_RESULT(self) \
  dm_result_wrapper *wrapper; \
  TypedData_Get_Struct(self, dm_result_wrapper, &rb_dm_result_type, wrapper);

typedef struct {
  int symbolizeKeys;
  int asArray;
  int castBool;
  int cacheRows;
  int cast;
  int streaming;
  ID db_timezone;
  ID app_timezone;
  int block_given; /* boolean */
} result_each_args;

extern VALUE mdm, cdmClient, cdmError;
static VALUE cdmResult, cDateTime, cDate;
static VALUE opt_decimal_zero, opt_float_zero, opt_time_year, opt_time_month, opt_utc_offset;
static ID intern_new, intern_utc, intern_local, intern_localtime, intern_local_offset,
  intern_civil, intern_new_offset, intern_merge, intern_BigDecimal,
  intern_query_options;
static VALUE sym_symbolize_keys, sym_as, sym_array, sym_database_timezone,
  sym_application_timezone, sym_local, sym_utc, sym_cast_booleans,
  sym_cache_rows, sym_cast, sym_stream, sym_name;

struct nogvl_get_col_args
{
  dm_result_wrapper *wrapper;
};

static void *nogvl_get_col_desc(void *ptr) {
  struct nogvl_get_col_args* ptr1 = ptr;
  dm_result_wrapper *wrapper = ptr1->wrapper;
  DPIRETURN rt;
  dhdesc hdesc_col;
  int iParam;

  rt = dpi_number_columns(wrapper->statement, &(wrapper->numberOfFields));
  if(!DSQL_SUCCEEDED(rt))
    return (void*)Qfalse;

  rt = dpi_row_count(wrapper->statement, &(wrapper->numberOfRows));
  if(!DSQL_SUCCEEDED(rt))
    return (void*)Qfalse;

  wrapper->lobs = (dhloblctr*)xcalloc(wrapper->numberOfFields, sizeof(dhloblctr));
  wrapper->length = (slength*)xcalloc(wrapper->numberOfFields, sizeof(slength));
  wrapper->col_desc = (DmColDesc*)xcalloc(wrapper->numberOfFields, sizeof(DmColDesc));
  wrapper->result = (char**)xcalloc(wrapper->numberOfFields, sizeof(char*));

  rt = dpi_get_stmt_attr(wrapper->statement, DSQL_ATTR_IMP_ROW_DESC,
                           (dpointer)&hdesc_col, 0, NULL);
  if(!DSQL_SUCCEEDED(rt))
    return (void*)Qfalse;
  
  for (iParam = 0; iParam < wrapper->numberOfFields; iParam++)
  {
    rt = dpi_desc_column(wrapper->statement, (sdint2)iParam + 1, wrapper->col_desc[iParam].name,
                             sizeof(wrapper->col_desc[iParam].name), &wrapper->col_desc[iParam].name_length,
                             &wrapper->col_desc[iParam].sql_type, &wrapper->col_desc[iParam].prec, &wrapper->col_desc[iParam].scale,
                             &wrapper->col_desc[iParam].nullable);
    if(!DSQL_SUCCEEDED(rt))
      return (void*)Qfalse;
    if (wrapper->col_desc[iParam].sql_type == DSQL_BLOB || wrapper->col_desc[iParam].sql_type == DSQL_CLOB)
    {
      rt = dpi_alloc_lob_locator(wrapper->statement, &(wrapper->lobs[iParam]));
      if(!DSQL_SUCCEEDED(rt))
        return (void*)Qfalse;
      rt = dpi_bind_col(wrapper->statement, (udint2)iParam + 1, DSQL_C_LOB_HANDLE,
                              &(wrapper->lobs[iParam]),sizeof(wrapper->lobs[iParam]), &wrapper->length[iParam]);
    }
    else
    {
      rt = dpi_get_desc_field(
                hdesc_col, (sdint2)iParam + 1, DSQL_DESC_DISPLAY_SIZE,
                (dpointer)&(wrapper->col_desc[iParam].length), 0, NULL);
      if(!DSQL_SUCCEEDED(rt))
        return (void*)Qfalse;
      wrapper->result[iParam] = (char*)xmalloc(wrapper->col_desc[iParam].length+2);
      if(wrapper->col_desc[iParam].sql_type == DSQL_BINARY || wrapper->col_desc[iParam].sql_type == DSQL_VARBINARY)
      {
        rt = dpi_bind_col(wrapper->statement, (udint2)iParam + 1, DSQL_C_BINARY,
                              (dpointer)wrapper->result[iParam],
                              wrapper->col_desc[iParam].length + 1, &wrapper->length[iParam]);
      }
      else
      {
        rt = dpi_bind_col(wrapper->statement, (udint2)iParam + 1, DSQL_C_NCHAR,
                                (dpointer)wrapper->result[iParam],
                                wrapper->col_desc[iParam].length + 1, &wrapper->length[iParam]);
      }                  
    }
  }
  wrapper->is_bind = 1;
  return (void*)Qtrue;
}

/* Mark any VALUEs that are only referenced in C, so the GC won't get them. */
static void rb_dm_result_mark(void * wrapper) {
  dm_result_wrapper * w = wrapper;
  if (w) {
    rb_gc_mark_movable(w->fields);
    rb_gc_mark_movable(w->rows);
    rb_gc_mark_movable(w->encoding);
    rb_gc_mark_movable(w->client);
  }
}

/* this may be called manually or during GC */
static void rb_dm_result_free_result(dm_result_wrapper * wrapper) {
  if (!wrapper) return;
  int i = 0;
  if (wrapper->resultFreed != 1 && wrapper->is_bind == 1) 
  {
   
    for(i = 0; i < wrapper->numberOfFields; i++)
    {
      if(wrapper->col_desc[i].sql_type == DSQL_BLOB || wrapper->col_desc[i].sql_type == DSQL_CLOB)
        dpi_free_lob_locator(wrapper->lobs[i]);
      if(wrapper->result[i])
      {
        xfree(wrapper->result[i]);
        wrapper->result[i] = NULL;
      }
    }

    xfree(wrapper->result);
    xfree(wrapper->length);
    xfree(wrapper->lobs);
    xfree(wrapper->col_desc);
    /* FIXME: this may call flush_use_result, which can hit the socket */
    /* For prepared statements, wrapper->result is the result metadata */
  }
  wrapper->resultFreed = 1;
}

/* this is called during GC */
static void rb_dm_result_free(void *ptr) {
  dm_result_wrapper *wrapper = ptr;
  if (wrapper->statement != NULL && wrapper->is_prepare == 0) {
      dpi_free_stmt(wrapper->statement);
      wrapper->statement = NULL;
  }
  rb_dm_result_free_result(wrapper);

  // If the GC gets to client first it will be nil
  if (wrapper->client != Qnil) {
    decr_dm_client(wrapper->client_wrapper);
  }

  xfree(wrapper);
}

static size_t rb_dm_result_memsize(const void * wrapper) {
  const dm_result_wrapper * w = wrapper;
  size_t memsize = sizeof(*w);
  if (w->client_wrapper) {
    memsize += sizeof(*w->client_wrapper);
  }
  return memsize;
}

#ifdef HAVE_RB_GC_MARK_MOVABLE
static void rb_dm_result_compact(void * wrapper) {
  dm_result_wrapper * w = wrapper;
  if (w) {
    rb_dm_gc_location(w->fields);
    rb_dm_gc_location(w->rows);
    rb_dm_gc_location(w->encoding);
    rb_dm_gc_location(w->client);
  }
}
#endif

static const rb_data_type_t rb_dm_result_type = {
  "rb_dm_result",
  {
    rb_dm_result_mark,
    rb_dm_result_free,
    rb_dm_result_memsize,
#ifdef HAVE_RB_GC_MARK_MOVABLE
    rb_dm_result_compact,
#endif
  },
  0,
  0,
#ifdef RUBY_TYPED_FREE_IMMEDIATELY
  RUBY_TYPED_FREE_IMMEDIATELY,
#endif
};

static VALUE rb_dm_result_free_(VALUE self) {
  GET_RESULT(self);

  if (wrapper->statement != NULL && wrapper->is_prepare == 0) {
      dpi_free_stmt(wrapper->statement);
      wrapper->statement = NULL;
  }

  rb_dm_result_free_result(wrapper);
  wrapper->resultFreed = 1;
  return Qnil;
}

/*
 * for small results, this won't hit the network, but there's no
 * reliable way for us to tell this so we'll always release the GVL
 * to be safe
 */
static void* nogvl_stmt_fetch(void *ptr) {
  dm_result_wrapper* wrapper = ptr;
  DPIRETURN rt = DSQL_SUCCESS;
  ulength  row_num;
  slength  data_get = 0;
  int iparam;
  rt = dpi_fetch(wrapper->statement, &row_num);
  if(rt == DSQL_NO_DATA || row_num == 0)
    return (void*)Qnil;
  if(!DSQL_SUCCEEDED(rt))
    return (void*)Qfalse;
  for(iparam = 0; iparam < wrapper->numberOfFields; iparam++)
  {
    if(wrapper->col_desc[iparam].sql_type == DSQL_BLOB || wrapper->col_desc[iparam].sql_type == DSQL_CLOB)
    {
      rt = dpi_lob_get_length((dhloblctr)(wrapper->lobs[iparam]), &wrapper->length[iparam]);
      if(wrapper->result[iparam])
        xfree(wrapper->result[iparam]);
      if(!DSQL_SUCCEEDED(rt) || wrapper->length[iparam] == -1)
      {
        wrapper->result[iparam] = NULL;
        continue;
      }
      if(wrapper->col_desc[iparam].sql_type == DSQL_CLOB)
        wrapper->result[iparam] = xmalloc(wrapper->length[iparam] * 4 + 2);
      else
        wrapper->result[iparam] = xmalloc(wrapper->length[iparam] + 2);
      if(wrapper->col_desc[iparam].sql_type == DSQL_BLOB)
      {
        rt = dpi_lob_read((dhloblctr)(wrapper->lobs[iparam]), 1, DSQL_C_BINARY,
                                      0, wrapper->result[iparam], wrapper->length[iparam] + 1, NULL);
      }
      else
      {
         rt = dpi_lob_read((dhloblctr)(wrapper->lobs[iparam]), 1, DSQL_C_NCHAR,
                                     0, wrapper->result[iparam], wrapper->length[iparam] * 4 + 1, &data_get);
      }
    }
  }

  return (void*)Qtrue;
}

static VALUE rb_dm_result_fetch_field(VALUE self, unsigned int idx, int symbolize_keys) {
  VALUE rb_field;
  VALUE rt;
  GET_RESULT(self);
  if(wrapper->is_bind == 0)
  {
    struct nogvl_get_col_args args;
    args.wrapper = wrapper;
    rt = (VALUE)rb_thread_call_without_gvl(nogvl_get_col_desc, &args, RUBY_UBF_IO, 0);
    if (rt == Qfalse)
      rb_raise(cdmError, "failed to get col_desc");
  }

  if (wrapper->fields == Qnil) {
    wrapper->fields = rb_ary_new2(wrapper->numberOfFields);
  }

  rb_field = rb_ary_entry(wrapper->fields, idx);
  if (rb_field == Qnil) {
    DmColDesc field;
    rb_encoding *default_internal_enc = rb_default_internal_encoding();
    rb_encoding *conn_enc = rb_to_encoding(wrapper->encoding);

    field = wrapper->col_desc[idx];
    if (symbolize_keys) {
      rb_field = rb_intern3(field.name, field.name_length, rb_utf8_encoding());
      rb_field = ID2SYM(rb_field);
    } else {
#ifdef HAVE_RB_ENC_INTERNED_STR
      rb_field = rb_enc_interned_str(field.name, field.name_length, conn_enc);
      if (default_internal_enc && default_internal_enc != conn_enc) {
        rb_field = rb_str_to_interned_str(rb_str_export_to_enc(rb_field, default_internal_enc));
      }
#else
      rb_field = rb_enc_str_new(field.name, field.name_length, conn_enc);
      if (default_internal_enc && default_internal_enc != conn_enc) {
        rb_field = rb_str_export_to_enc(rb_field, default_internal_enc);
      }
      rb_obj_freeze(rb_field);
#endif
    }
    rb_ary_store(wrapper->fields, idx, rb_field);
  }

  return rb_field;
}

static VALUE rb_dm_result_fetch_field_type(VALUE self, unsigned int idx) {
  VALUE rb_field_type;
  VALUE rv;
  GET_RESULT(self);
  struct nogvl_get_col_args args;
  args.wrapper = wrapper;

  if(wrapper->is_bind == 0)
  {
    struct nogvl_get_col_args args;
    args.wrapper = wrapper;
    rv = (VALUE)rb_thread_call_without_gvl(nogvl_get_col_desc, &args, RUBY_UBF_IO, 0);
    if (rv == Qfalse)
      rb_raise(cdmError, "failed to get col_desc");
  }

  if (wrapper->fieldTypes == Qnil) { 
    wrapper->fieldTypes = rb_ary_new2(wrapper->numberOfFields);
  }

  rb_field_type = rb_ary_entry(wrapper->fieldTypes, idx);
  if (rb_field_type == Qnil) {
    DmColDesc field ;
    rb_encoding *default_internal_enc = rb_default_internal_encoding();
    rb_encoding *conn_enc = rb_to_encoding(wrapper->encoding);
    int precision;

    field = wrapper->col_desc[idx];

    switch(field.sql_type) {
      case DSQL_TINYINT:         // signed char
        rb_field_type = rb_sprintf("tinyint(%ld)", field.length);
        break;
      case DSQL_SMALLINT:        // short int
        rb_field_type = rb_sprintf("smallint(%ld)", field.length);
        break;
      case DSQL_INTERVAL_YEAR:         // short int
        rb_field_type = rb_str_new_cstr("interval year");
        break;
      case DSQL_INTERVAL_MONTH:         // short int
        rb_field_type = rb_str_new_cstr("interval month");
        break;
      case DSQL_INTERVAL_DAY:         // short int
        rb_field_type = rb_str_new_cstr("interval day");
        break;
      case DSQL_INTERVAL_HOUR:         // short int
        rb_field_type = rb_str_new_cstr("interval hour");
        break;
      case DSQL_INTERVAL_MINUTE:         // short int
        rb_field_type = rb_str_new_cstr("interval minute");
        break;
      case DSQL_INTERVAL_SECOND:         // short int
        rb_field_type = rb_str_new_cstr("interval second");
        break;
      case DSQL_INTERVAL_YEAR_TO_MONTH:         // short int
        rb_field_type = rb_str_new_cstr("interval year to month");
        break;
      case DSQL_INTERVAL_DAY_TO_HOUR:         // short int
        rb_field_type = rb_str_new_cstr("interval day to hour");
        break;
      case DSQL_INTERVAL_DAY_TO_MINUTE:         // short int
        rb_field_type = rb_str_new_cstr("interval day to minute");
        break;
      case DSQL_INTERVAL_DAY_TO_SECOND:         // short int
        rb_field_type = rb_str_new_cstr("interval day to second");
        break;
      case DSQL_INTERVAL_HOUR_TO_MINUTE:         // short int
        rb_field_type = rb_str_new_cstr("interval hour to minute");
        break;
      case DSQL_INTERVAL_HOUR_TO_SECOND:         // short int
        rb_field_type = rb_str_new_cstr("interval hour to second");
        break;
      case DSQL_INTERVAL_MINUTE_TO_SECOND:         // short int
        rb_field_type = rb_str_new_cstr("interval minute to second");
        break;
      case DSQL_INT:        // int
        rb_field_type = rb_sprintf("int(%ld)", field.length);
        break;
      case DSQL_BIGINT:     // long long int
        rb_field_type = rb_sprintf("bigint(%ld)", field.length);
        break;
      case DSQL_FLOAT:        // float
        rb_field_type = rb_sprintf("float(%ld,%d)", field.prec, field.scale);
        break;
      case DSQL_DOUBLE:       // double
        rb_field_type = rb_sprintf("double(%ld,%d)", field.prec, field.scale);
        break;
      case DSQL_TIME:         // dm_TIME
        rb_field_type = rb_str_new_cstr("time");
        break;
      case DSQL_DATE:         // dm_TIME
        rb_field_type = rb_str_new_cstr("date");
        break;
      case DSQL_TIMESTAMP:    // dm_TIME
        rb_field_type = rb_str_new_cstr("timestamp");
        break;
      case DSQL_TIME_TZ:    // dm_TIME
        rb_field_type = rb_str_new_cstr("time with time zone");
        break;
      case DSQL_TIMESTAMP_TZ:    // dm_TIME
        rb_field_type = rb_str_new_cstr("timestamp with time zone");
        break;
      case DSQL_DEC:      // char[]
        rb_field_type = rb_sprintf("decimal(%d,%d)", field.prec, field.scale);
        break;
      case DSQL_BINARY:       // char[]
       rb_field_type = rb_str_new_cstr("binary");
        break;
      case DSQL_VARBINARY:   // char[]
       rb_field_type = rb_str_new_cstr("varbinary");
        break;
      case DSQL_VARCHAR:      // char[]
        rb_field_type = rb_sprintf("varchar(%ld)", field.length);
        break;
      case DSQL_CHAR:      // char[]
        rb_field_type = rb_sprintf("char(%ld)", field.length);
        break;
      case DSQL_BLOB:         // char[]
        rb_field_type = rb_str_new_cstr("blob");
        break;
      case DSQL_CLOB:  // char[]
        rb_field_type = rb_str_new_cstr("clob");
        break;
      case DSQL_BIT:          // char[]
        rb_field_type = rb_sprintf("bit(%ld)", field.length);
        break;
      case DSQL_ROWID:          // char[]
        rb_field_type = rb_str_new_cstr("rowid");
        break;
      default:
        rb_field_type = rb_str_new_cstr("unknown");
        break;
    }

    rb_enc_associate(rb_field_type, conn_enc);
    if (default_internal_enc) {
      rb_field_type = rb_str_export_to_enc(rb_field_type, default_internal_enc);
    }

    rb_ary_store(wrapper->fieldTypes, idx, rb_field_type);
  }

  return rb_field_type;
}

static VALUE dm_set_field_string_encoding(VALUE val, rb_encoding *default_internal_enc, rb_encoding *conn_enc) 
{
  rb_enc_associate(val, conn_enc);

  if (default_internal_enc) {
    val = rb_str_export_to_enc(val, default_internal_enc);
  }

  return val;
}

/* Interpret microseconds digits left-aligned in fixed-width field.
 * e.g. 10.123 seconds means 10 seconds and 123000 microseconds,
 * because the microseconds are to the right of the decimal point.
 */
static unsigned int msec_char_to_uint(char *msec_char, size_t len)
{
  size_t i;
  for (i = 0; i < (len - 1); i++) {
    if (msec_char[i] == '\0') {
      msec_char[i] = '0';
    }
  }
  return (unsigned int)strtoul(msec_char, NULL, 10);
}

static VALUE rb_dm_result_fetch_row(VALUE self,const result_each_args *args)
{
  VALUE rowVal;
  char** row;
  unsigned int i = 0;
  slength * fieldLengths;
  void * ptr;
  rb_encoding *default_internal_enc;
  rb_encoding *conn_enc;
  VALUE rv;
  GET_RESULT(self);

  default_internal_enc = rb_default_internal_encoding();
  conn_enc = rb_to_encoding(wrapper->encoding);

  if(wrapper->is_bind == 0)
  {
    struct nogvl_get_col_args args;
    args.wrapper = wrapper;
    rv = (VALUE)rb_thread_call_without_gvl(nogvl_get_col_desc, &args, RUBY_UBF_IO, 0);
    if (rv == Qfalse)
      rb_raise(cdmError, "failed to get col_desc");
  }

  ptr = wrapper->result;
  {
    rv = (VALUE)rb_thread_call_without_gvl(nogvl_stmt_fetch, wrapper, RUBY_UBF_IO, 0);
    if(rv == Qnil)
      return Qnil;
    else if(rv == Qfalse)
      rb_raise(cdmError, "failed to fetch row");
  }

  row = wrapper->result;

  if (wrapper->fields == Qnil) {
    wrapper->fields = rb_ary_new2(wrapper->numberOfFields);
  }
  if (args->asArray) {
    rowVal = rb_ary_new2(wrapper->numberOfFields);
  } else {
    rowVal = rb_hash_new();
  }
  fieldLengths = wrapper->length;

  for (i = 0; i < wrapper->numberOfFields; i++) {
    VALUE field = rb_dm_result_fetch_field(self, i, args->symbolizeKeys);
    if (row[i] && fieldLengths[i]>0) {
      VALUE val = Qnil;
      sdint2 type = wrapper->col_desc[i].sql_type;

      if (!args->cast) {
        val = rb_str_new(row[i], fieldLengths[i]);
        val = dm_set_field_string_encoding(val, default_internal_enc, conn_enc);
      } else {
        switch(type) {
        case DSQL_BIT:         
          if (args->castBool && fieldLengths[i] == 1) {
            val = *row[i] == '1' ? Qtrue : Qfalse;
          }else{
            val = rb_cstr2inum(row[i], 10);
          }
          break;
        case DSQL_TINYINT:       /* TINYINT field */
          if (args->castBool && fieldLengths[i] == 1) {
            val = *row[i] != '0' ? Qtrue : Qfalse;
            break;
          }
        case DSQL_SMALLINT:      /* SMALLINT field */
        case DSQL_INT:       /* INTEGER field */
        case DSQL_BIGINT:   /* BIGINT field */
          val = rb_cstr2inum(row[i], 10);
          break;
        case DSQL_DEC:    /* DECIMAL or NUMERIC field */
          if (strtod(row[i], NULL) == 0.000000){
            val = rb_funcall(rb_mKernel, intern_BigDecimal, 1, opt_decimal_zero);
          }else{
            val = rb_funcall(rb_mKernel, intern_BigDecimal, 1, rb_str_new(row[i], fieldLengths[i]));
          }
          break;
        case DSQL_FLOAT:      /* FLOAT field */
        case DSQL_DOUBLE: {     /* DOUBLE or REAL field */
          double column_to_double;
          column_to_double = strtod(row[i], NULL);
          if (column_to_double == 0.000000){
            val = opt_float_zero;
          }else{
            val = rb_float_new(column_to_double);
          }
          break;
        }
        case DSQL_TIME: {     /* TIME field */
          int tokens;
          unsigned int hour=0, min=0, sec=0, msec=0;
          char msec_char[7] = {'0','0','0','0','0','0','\0'};

          tokens = sscanf(row[i], "%2u:%2u:%2u.%6s", &hour, &min, &sec, msec_char);
          if (tokens < 3) {
            val = rb_str_new(row[i], fieldLengths[i]);
            val = dm_set_field_string_encoding(val, default_internal_enc, conn_enc);
            break;
          }
          msec = msec_char_to_uint(msec_char, sizeof(msec_char));
          val = rb_funcall(rb_cTime, args->db_timezone, 7, opt_time_year, opt_time_month, opt_time_month, UINT2NUM(hour), UINT2NUM(min), UINT2NUM(sec), UINT2NUM(msec));
          if (!NIL_P(args->app_timezone)) {
            if (args->app_timezone == intern_local) {
              val = rb_funcall(val, intern_localtime, 0);
            } else { /* utc */
              val = rb_funcall(val, intern_utc, 0);
            }
          }
          break;
        }
        case DSQL_TIMESTAMP:  {/* TIMESTAMP field */
          int tokens;
          unsigned int year=0, month=0, day=0, hour=0, min=0, sec=0, msec=0;
          char msec_char[7] = {'0','0','0','0','0','0','\0'};
          uint64_t seconds;

          tokens = sscanf(row[i], "%4u-%2u-%2u %2u:%2u:%2u.%6s", &year, &month, &day, &hour, &min, &sec, msec_char);
          if (tokens < 6) { /* msec might be empty */
            val = rb_str_new(row[i], fieldLengths[i]);
            val = dm_set_field_string_encoding(val, default_internal_enc, conn_enc);
            break;
          }
          seconds = (year*31557600ULL) + (month*2592000ULL) + (day*86400ULL) + (hour*3600ULL) + (min*60ULL) + sec;

          if (seconds == 0) {
            val = Qnil;
          } else {
            if (month < 1 || day < 1) {
              rb_raise(cdmError, "Invalid date in field '%.*s': %s", wrapper->col_desc[i].name_length, wrapper->col_desc[i].name, row[i]);
              val = Qnil;
            } else {
              if (seconds < dm_MIN_TIME || seconds > dm_MAX_TIME) { /* use DateTime for larger date range, does not support microseconds */
                VALUE offset = INT2NUM(0);
                if (args->db_timezone == intern_local) {
                  offset = rb_funcall(cdmClient, intern_local_offset, 0);
                }
                val = rb_funcall(cDateTime, intern_civil, 7, UINT2NUM(year), UINT2NUM(month), UINT2NUM(day), UINT2NUM(hour), UINT2NUM(min), UINT2NUM(sec), offset);
                if (!NIL_P(args->app_timezone)) {
                  if (args->app_timezone == intern_local) {
                    offset = rb_funcall(cdmClient, intern_local_offset, 0);
                    val = rb_funcall(val, intern_new_offset, 1, offset);
                  } else { /* utc */
                    val = rb_funcall(val, intern_new_offset, 1, opt_utc_offset);
                  }
                }
              } else {
                msec = msec_char_to_uint(msec_char, sizeof(msec_char));
                val = rb_funcall(rb_cTime, args->db_timezone, 7, UINT2NUM(year), UINT2NUM(month), UINT2NUM(day), UINT2NUM(hour), UINT2NUM(min), UINT2NUM(sec), UINT2NUM(msec));
                if (!NIL_P(args->app_timezone)) {
                  if (args->app_timezone == intern_local) {
                    val = rb_funcall(val, intern_localtime, 0);
                  } else { /* utc */
                    val = rb_funcall(val, intern_utc, 0);
                  }
                }
              }
            }
          }
          break;
        }
        case DSQL_DATE:{       /* DATE field */
          int tokens;
          unsigned int year=0, month=0, day=0;
          tokens = sscanf(row[i], "%4u-%2u-%2u", &year, &month, &day);
          if (tokens < 3) {
            val = rb_str_new(row[i], fieldLengths[i]);
            val = dm_set_field_string_encoding(val, default_internal_enc, conn_enc);
            break;
          }
          if (year+month+day == 0) {
            val = Qnil;
          } else {
            if (month < 1 || day < 1) {
              rb_raise(cdmError, "Invalid date in field '%.*s': %s", wrapper->col_desc[i].name_length, wrapper->col_desc[i].name, row[i]);
              val = Qnil;
            } else {
              val = rb_funcall(cDate, intern_new, 3, UINT2NUM(year), UINT2NUM(month), UINT2NUM(day));
            }
          }
          break;
        }
        case DSQL_BLOB:
        case DSQL_BINARY:
        case DSQL_VARBINARY:
        {
          val = rb_str_new(row[i],  fieldLengths[i]);
          val = dm_set_field_string_encoding(val, default_internal_enc, conn_enc);
          break;
        }
        case DSQL_VARCHAR:
        default:
          val = rb_str_new(row[i], strlen(row[i]));
          val = dm_set_field_string_encoding(val, default_internal_enc, conn_enc);
          break;
        }
      }
      if (args->asArray) {
        rb_ary_push(rowVal, val);
      } else {
        rb_hash_aset(rowVal, field, val);
      }
    } else {
      if (args->asArray) {
        rb_ary_push(rowVal, Qnil);
      } else {
        rb_hash_aset(rowVal, field, Qnil);
      }
    }
  }
  return rowVal;
}

static VALUE rb_dm_result_fetch_fields(VALUE self) {
  unsigned int i = 0;
  short int symbolizeKeys = 0;
  VALUE rv;

  GET_RESULT(self);

  if(wrapper->resultFreed)
  {
    rb_raise(cdmError, "result is freed");
  }

  if(wrapper->is_bind == 0)
  {
    struct nogvl_get_col_args args;
    args.wrapper = wrapper;
    rv = (VALUE)rb_thread_call_without_gvl(nogvl_get_col_desc, &args, RUBY_UBF_IO, 0);
    if (rv == Qfalse)
      rb_raise(cdmError, "failed to get col_desc");
  }

  if (wrapper->fields == Qnil) {
    wrapper->fields = rb_ary_new2(wrapper->numberOfFields);
  }

  if (RARRAY_LEN(wrapper->fields) != wrapper->numberOfFields) {
    for (i=0; i<wrapper->numberOfFields; i++) {
      rb_dm_result_fetch_field(self, i, symbolizeKeys);
    }
  }

  return wrapper->fields;
}

static VALUE rb_dm_result_fetch_field_types(VALUE self) {
  unsigned int i = 0;
  VALUE rv;
  GET_RESULT(self);

  if(wrapper->resultFreed)
  {
    rb_raise(cdmError, "result is freed");
  }

  if(wrapper->is_bind == 0)
  {
    struct nogvl_get_col_args args;
    args.wrapper = wrapper;
    rv = (VALUE)rb_thread_call_without_gvl(nogvl_get_col_desc, &args, RUBY_UBF_IO, 0);
    if (rv == Qfalse)
      rb_raise(cdmError, "failed to get col_desc");
  }

  if (wrapper->fieldTypes == Qnil) {
    wrapper->fieldTypes = rb_ary_new2(wrapper->numberOfFields);
  }

  if (RARRAY_LEN(wrapper->fieldTypes) != wrapper->numberOfFields) {
    for (i=0; i<wrapper->numberOfFields; i++) {
      rb_dm_result_fetch_field_type(self, i);
    }
  }

  return wrapper->fieldTypes;
}

static VALUE rb_dm_result_each_(VALUE self,
                                   VALUE(*fetch_row_func)(VALUE, const result_each_args *args),
                                   const result_each_args *args)
{
  unsigned long i;
  const char *errstr;

  GET_RESULT(self);

  if (args->cacheRows && wrapper->lastRowProcessed == wrapper->numberOfRows) {
    /* we've already read the entire dataset from the C result into our */
    /* internal array. Lets hand that over to the user since it's ready to go */
    for (i = 0; i < wrapper->numberOfRows; i++) {
      rb_yield(rb_ary_entry(wrapper->rows, i));
    }
  } else {
    unsigned long rowsProcessed = 0;
    rowsProcessed = RARRAY_LEN(wrapper->rows);

    for (i = 0; i < wrapper->numberOfRows; i++) {
      VALUE row;
      if (args->cacheRows && i < rowsProcessed) {
        row = rb_ary_entry(wrapper->rows, i);
      } else {
        row = fetch_row_func(self, args);
        if (args->cacheRows) {
          rb_ary_store(wrapper->rows, i, row);
        }
        wrapper->lastRowProcessed++;
      }

      if (row == Qnil) {
        /* we don't need the dm C dataset around anymore, peace it */
        return Qnil;
      }

      if (args->block_given) {
        rb_yield(row);
      }
    }
  }
  
  // FIXME return Enumerator instead?
  // return rb_ary_each(wrapper->rows);
  return wrapper->rows;
}

static VALUE rb_dm_result_each(int argc, VALUE * argv, VALUE self) {
  result_each_args args;
  VALUE rv;
  VALUE defaults, opts, (*fetch_row_func)(VALUE, const result_each_args *args);
  ID db_timezone, app_timezone, dbTz, appTz;
  int symbolizeKeys, asArray, castBool, cacheRows, cast;

  GET_RESULT(self);

  if(wrapper->resultFreed)
  {
    rb_raise(cdmError, "result is freed");
  }

  if (!wrapper->statement) {
    rb_raise(cdmError, "Statement handle already closed");
  }

  if(wrapper->is_bind == 0)
  {
    struct nogvl_get_col_args args;
    args.wrapper = wrapper;
    rv = (VALUE)rb_thread_call_without_gvl(nogvl_get_col_desc, &args, RUBY_UBF_IO, 0);
    if (rv == Qfalse)
      rb_raise(cdmError, "failed to get col_desc");
  }
  defaults = rb_ivar_get(self, intern_query_options);
  // A block can be passed to this method, but since we don't call the block directly from C,
  // we don't need to capture it into a variable here with the "&" scan arg.
  if (rb_scan_args(argc, argv, "01", &opts) == 1) 
  {
    opts = rb_funcall(defaults, intern_merge, 1, opts);
    symbolizeKeys = RTEST(rb_hash_aref(opts, sym_symbolize_keys));
    asArray       = rb_hash_aref(opts, sym_as) == sym_array;
    castBool      = RTEST(rb_hash_aref(opts, sym_cast_booleans));
    cacheRows     = RTEST(rb_hash_aref(opts, sym_cache_rows));
    cast          = RTEST(rb_hash_aref(opts, sym_cast));
  } 
  else if(defaults != Qnil)
  {
    opts = defaults;
    symbolizeKeys = RTEST(rb_hash_aref(opts, sym_symbolize_keys));
    asArray       = rb_hash_aref(opts, sym_as) == sym_array;
    castBool      = RTEST(rb_hash_aref(opts, sym_cast_booleans));
    cacheRows     = 1;
    cast          = 1;
  }
  else
  {
    symbolizeKeys = 0;
    asArray       = 0;
    castBool      = 0;
    cacheRows     = 1;
    cast          = 1;
  }
 
  db_timezone = intern_local;
  app_timezone = Qnil;

  if (wrapper->rows == Qnil ) {
    wrapper->rows = rb_ary_new2(wrapper->numberOfRows);
  } 

  // Backward compat
  args.symbolizeKeys = symbolizeKeys;
  args.asArray = asArray;
  args.castBool = castBool;
  args.cacheRows = cacheRows;
  args.cast = cast;
  args.db_timezone = db_timezone;
  args.app_timezone = app_timezone;
  args.block_given = rb_block_given_p();

 
  fetch_row_func = rb_dm_result_fetch_row;

  return rb_dm_result_each_(self, fetch_row_func, &args);
}

static VALUE rb_dm_result_count(VALUE self) {
  GET_RESULT(self);
  VALUE rt;
  if(wrapper->resultFreed)
  {
    rb_raise(cdmError, "result is freed");
  }
  if(wrapper->is_bind == 0)
  {
    struct nogvl_get_col_args args;
    args.wrapper = wrapper;
    rt = (VALUE)rb_thread_call_without_gvl(nogvl_get_col_desc, &args, RUBY_UBF_IO, 0);
    if (rt == Qfalse)
      rb_raise(cdmError, "failed to get col_desc");
  }

  return ULONG2NUM(wrapper->numberOfRows);
}

static void *nogvl_get_next_result(void *ptr) {
  dm_result_wrapper *wrapper = ptr;
  DPIRETURN rt;
  rt = dpi_more_results(wrapper->statement);
  if(rt == DSQL_NO_DATA)
    return (void*)Qnil;
  else if(!DSQL_SUCCEEDED(rt))
    return (void*)Qfalse;
  else 
    return (void*)Qtrue;

}

static VALUE rb_dm_next_result(VALUE self) {
  GET_RESULT(self);
  VALUE rt,defaults,obj;

  if(wrapper->resultFreed)
  {
    rb_raise(cdmError, "result is freed");
  }

  rt = (VALUE)rb_thread_call_without_gvl(nogvl_get_next_result, wrapper, RUBY_UBF_IO, 0);
  if(rt == Qnil)
    return Qnil;
  else if(rt == Qfalse)
    rb_raise(cdmError, "failed to get next result");
  
  defaults = rb_ivar_get(self, intern_query_options);
  obj = rb_dm_result_to_obj(wrapper->client, wrapper->encoding, wrapper->statement, 1, defaults);
  return obj;
}

/* dm::Result */
VALUE rb_dm_result_to_obj(VALUE client, VALUE encoding, dhstmt statement, int flag, VALUE options) {
  VALUE obj;
  dm_result_wrapper * wrapper;
  DPIRETURN rt;

#ifdef NEW_TYPEDDATA_WRAPPER
  obj = TypedData_Make_Struct(cdmResult, dm_result_wrapper, &rb_dm_result_type, wrapper);
#else
  obj = Data_Make_Struct(cdmResult, dm_result_wrapper, rb_dm_result_mark, rb_dm_result_free, wrapper);
#endif
  wrapper->numberOfFields = 0;
  wrapper->numberOfRows = 0;
  wrapper->resultFreed = 0;
  wrapper->fields = Qnil;
  wrapper->fieldTypes = Qnil;
  wrapper->rows = Qnil;
  wrapper->encoding = encoding;
  wrapper->client = client;
  wrapper->client_wrapper = DATA_PTR(client);
  wrapper->client_wrapper->refcount++;
  wrapper->result = NULL;
  wrapper->length = NULL;
  wrapper->lobs = NULL;
  wrapper->col_desc = NULL;
  wrapper->is_bind = 0;
  /* Keep a handle to the Statement to ensure it doesn't get garbage collected first */
  wrapper->statement = statement;
  wrapper->is_prepare = flag;

  rb_obj_call_init(obj, 0, NULL);
  if(wrapper->is_prepare == 0)
  {
    rb_ivar_set(obj, intern_query_options, options);
  }
  else
  {
     rb_ivar_set(obj,intern_query_options, rb_hash_dup(rb_ivar_get(wrapper->client, intern_query_options)));
  }
  /* Options that cannot be changed in results.each(...) { |row| }
   * should be processed here. */

  return obj;
}

void init_dm_result() {
  cDate = rb_const_get(rb_cObject, rb_intern("Date"));
  rb_global_variable(&cDate);
  cDateTime = rb_const_get(rb_cObject, rb_intern("DateTime"));
  rb_global_variable(&cDateTime);

  cdmResult = rb_define_class_under(mdm, "Result", rb_cObject);
  rb_undef_alloc_func(cdmResult);
  rb_global_variable(&cdmResult);

  rb_define_method(cdmResult, "each", rb_dm_result_each, -1);
  rb_define_method(cdmResult, "fields", rb_dm_result_fetch_fields, 0);
  rb_define_method(cdmResult, "field_types", rb_dm_result_fetch_field_types, 0);
  rb_define_method(cdmResult, "free", rb_dm_result_free_, 0);
  rb_define_method(cdmResult, "count", rb_dm_result_count, 0);
  rb_define_alias(cdmResult, "size", "count");
  rb_define_method(cdmResult, "next_result", rb_dm_next_result, 0);

  intern_new          = rb_intern("new");
  intern_utc          = rb_intern("utc");
  intern_local        = rb_intern("local");
  intern_merge        = rb_intern("merge");
  intern_localtime    = rb_intern("localtime");

  intern_local_offset = rb_intern("local_offset");
  intern_civil        = rb_intern("civil");
  intern_new_offset   = rb_intern("new_offset");
  intern_BigDecimal   = rb_intern("BigDecimal");
  intern_query_options = rb_intern("@query_options");

  sym_symbolize_keys  = ID2SYM(rb_intern("symbolize_keys"));
  sym_as              = ID2SYM(rb_intern("as"));
  sym_array           = ID2SYM(rb_intern("array"));
  sym_local           = ID2SYM(rb_intern("local"));
  sym_utc             = ID2SYM(rb_intern("utc"));
  sym_cast_booleans   = ID2SYM(rb_intern("cast_booleans"));
  sym_database_timezone     = ID2SYM(rb_intern("database_timezone"));
  sym_application_timezone  = ID2SYM(rb_intern("application_timezone"));
  sym_cache_rows     = ID2SYM(rb_intern("cache_rows"));
  sym_cast           = ID2SYM(rb_intern("cast"));
  sym_stream         = ID2SYM(rb_intern("stream"));
  sym_name           = ID2SYM(rb_intern("name"));

  opt_decimal_zero = rb_str_new2("0.0");
  rb_global_variable(&opt_decimal_zero); /*never GC */
  opt_float_zero = rb_float_new((double)0);
  rb_global_variable(&opt_float_zero);
  opt_time_year = INT2NUM(2000);
  opt_time_month = INT2NUM(1);
  opt_utc_offset = INT2NUM(0);

  binaryEncoding = rb_enc_find("binary");
}
