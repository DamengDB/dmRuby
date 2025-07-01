#ifndef DM_RESULT_H
#define DM_RESULT_H

void init_dm_result(void);
VALUE rb_dm_result_to_obj(VALUE client, VALUE encoding, dhstmt statement, int flag, VALUE options);

typedef struct {
  sdbyte name[128 + 1];
  sdint2 name_length;
  sdint2 sql_type;
  ulength prec;
  sdint2 scale;
  sdint2 nullable;
  slength length;
}DmColDesc;

typedef struct {
  VALUE fields;
  VALUE fieldTypes;
  VALUE rows;
  VALUE client;
  VALUE encoding;
  dhstmt statement;
  sdint2 numberOfFields;
  sdint8 numberOfRows;
  sdint8 lastRowProcessed;
  dhloblctr* lobs;
  char **result;
  int resultFreed;
  int is_bind;
  int streamingComplete;
  dm_client_wrapper *client_wrapper;
  /* statement result bind buffers */
  slength *length;
  DmColDesc *col_desc;
  int is_prepare;
} dm_result_wrapper;

#endif
