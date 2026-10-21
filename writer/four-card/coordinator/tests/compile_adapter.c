/* Freestanding compile only. There is deliberately no EFI entrypoint. */
#include "../src/coordinator.h"
enum pc_status cmp_adapter_run(struct pc_pair_session *s,const struct pc_environment *e){return pc_pair_run(s,e);}
enum pc_status cmp_adapter_start(struct pc_pair_session *s){return pc_pair_start(s);}
size_t cmp_adapter_size(void){return sizeof(struct pc_pair_session);}
