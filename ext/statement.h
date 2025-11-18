#ifndef dm_STATEMENT_H
#define dm_STATEMENT_H

typedef struct {
  void* buffer;
  ulength buffer_length;
}dm_BIND;


typedef struct {
  sdint2 sql_type;
  ulength prec;
  sdint2 scale;
  sdint2 nullable;
}ParamDesc;

typedef struct {
  VALUE client;
  dhstmt stmt;
  int refcount;
  int closed;
  int is_select;
  sdbyte lastrowid[20];
  sdint8 affected_rows;
  udint2 param_num;
  udint2 col_num;
  ParamDesc *paramdesc;
  int is_prepare;
  dm_client_wrapper *client_wrapper;
} dm_stmt_wrapper;

void init_dm_statement(void);
void decr_dm_stmt(dm_stmt_wrapper *stmt_wrapper);

VALUE rb_dm_stmt_new(VALUE rb_client, VALUE sql);

#endif
