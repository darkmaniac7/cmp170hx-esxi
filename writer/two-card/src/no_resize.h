#ifndef CMP_NO_RESIZE_H
#define CMP_NO_RESIZE_H
#include "handoff.h"
enum nr_result {NR_REFUSED,NR_CHILD_RETURNED};
static enum nr_result nr_start(struct hf_session *s,unsigned execute){
    if(!s)return NR_REFUSED;
    if(execute!=1||s->phase!=HF_PREPARED||s->mutation_started||s->start_called||
       !s->callbacks.check_current||!s->callbacks.inspect_child||!s->callbacks.start){
        s->phase=HF_REJECTED;s->status=HF_STATE;return NR_REFUSED;
    }
    s->phase=HF_FINAL_CHECKING;
    if(!hf_check(s,HF_CHECK_BEFORE_START,HF_FINAL_CHECKING)||
       !hf_child_valid(s,HF_FINAL_CHECKING,1))return NR_REFUSED;
    s->start_called=1;s->phase=HF_STARTING;
    s->start_return_status=s->callbacks.start(s->callbacks.context,s->child);
    /* This branch never changed PCI state. Preserve terminal returned-child
     * evidence without manufacturing HF_TRANSACTION_OK or a mutation flag. */
    s->phase=HF_REJECTED;s->status=HF_IO;
    return NR_CHILD_RETURNED;
}
#endif
