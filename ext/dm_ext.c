#include <dm_ext.h>

VALUE mdm, cdmError;


/* Ruby Extension initializer */
void Init_dm_ext() {
  mdm = rb_define_module("Dm");
  rb_global_variable(&mdm);

  cdmError = rb_const_get(mdm, rb_intern("Error"));
  rb_global_variable(&cdmError);

  init_dm_client();
  init_dm_result();
  init_dm_statement();
}
