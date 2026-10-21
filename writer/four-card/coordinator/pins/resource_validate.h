#ifndef CMP_RESOURCE_VALIDATE_R1_H
#define CMP_RESOURCE_VALIDATE_R1_H
#include "resource_types.h"
#include "resource_policy.h"
#include "pci_conventional.h"
#include "pci_decode.h"
#include "inventory_sha256.h"
#include <string.h>
/* Pure host-specific arithmetic. Input pointers are owned RAM copies, never
 * physical addresses. Stable collection, freshness, GCD reservation and the
 * subsequent transaction/handoff remain separate adapter responsibilities. */
struct rc_desc { uint64_t gran,min,max,translation,len,end; unsigned type; };
struct rc_domain { unsigned first,last; };
struct rc_bridge { unsigned owner,root,secondary,subordinate,top; uint64_t lo[2],hi[2]; };
struct rc_cursor { const struct rc_read *reads; size_t count,pos; };
static int rc_trace(void *context,uint32_t offset,uint32_t *value)
{
 struct rc_cursor *c=context; *value=0;
 if(c->pos>=c->count) return 1;
 const struct rc_read *r=&c->reads[c->pos++];
 if(r->status || r->offset!=offset) return 1;
 *value=r->value; return 0;
}
static enum rc_status rc_fail(struct rc_result *r,enum rc_reason why,unsigned owner,unsigned index)
{ r->status=RC_REJECTED;r->reason=why;r->owner=owner;r->index=index;return RC_REJECTED; }
static int rc_overlap(uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{ return a<=b && c<=d && a<=d && c<=b; }
static int rc_collision(uint64_t a,uint64_t b,unsigned pair)
{
 for(unsigned t=0;t<(pair?PT_COUNT:1U);t++)
  if(rc_overlap(a,b,pair_targets[t].bar1,pair_targets[t].bar1+pair_targets[t].reservation_size-1))return 1;
 return 0;
}
static int rc_power2(uint64_t x) { return x && !(x&(x-1)); }
static int rc_extent(uint64_t base,uint64_t size,uint64_t *end)
{ if(!size || size-1>UINT64_MAX-base)return 0; *end=base+size-1;return 1; }
/* Address-space descriptors only. EndTag ends the entire owned list. Zero
 * checksum is UEFI's no-checksum form; a supplied checksum must validate. */
static int rc_descriptors(struct rc_blob b,struct rc_desc d[128],unsigned *count)
{
 size_t p=0;unsigned n=0;
 if(!b.data || b.size<2 || b.size>RC_LIST_MAX)return 0;
 while(p<b.size){
  unsigned tag=b.data[p],w,length;
  if(tag==0x79){
   if(p+2!=b.size)return 0;
   if(b.data[p+1]){uint8_t sum=0;for(size_t j=0;j<b.size;j++)sum=(uint8_t)(sum+b.data[j]);if(sum)return 0;}
   *count=n;return 1;
  }
  if(b.size-p<3 || n>=128)return 0;
  w=tag==0x8a?8:tag==0x87?4:tag==0x88?2:0;length=(unsigned)inv_le(b.data+p+1,2);
  if(!w || length!=3+5*w || (size_t)length+3>b.size-p)return 0;
  struct rc_desc x={0};x.type=b.data[p+3];x.gran=inv_le(b.data+p+6,w);
  x.min=inv_le(b.data+p+6+w,w);x.max=inv_le(b.data+p+6+2*w,w);
  x.translation=inv_le(b.data+p+6+3*w,w);x.len=inv_le(b.data+p+6+4*w,w);
  if(x.type>2 || x.translation || !rc_extent(x.min,x.len,&x.end))return 0;
  d[n++]=x;p+=length+3;
 }
 return 0;
}
static int rc_domain_for(const struct rc_domain d[RC_ROOTS],unsigned bus)
{
 int found=-1;for(unsigned i=0;i<RC_ROOTS;i++)if(d[i].first<=bus && bus<=d[i].last){if(found!=-1)return -1;found=(int)i;}return found;
}
static int rc_in_windows(const struct rc_bridge *b,uint64_t start,uint64_t end)
{ for(unsigned j=0;j<2;j++)if(b->lo[j]<=start && end<=b->hi[j])return 1;return 0; }
static enum rc_status rc_validate_inventory(const struct rc_snapshot *s,struct rc_result *r,
                                           unsigned pair,struct rc_pair_result *result)
{
 struct rc_desc desc[128];struct rc_domain domains[RC_ROOTS];struct rc_bridge bridges[RC_FUNCTIONS];
 unsigned bridge_count=0,root_seen=0,identity_seen[RC_FUNCTIONS]={0};
 unsigned function_root[RC_FUNCTIONS],query_seen[RC_FUNCTIONS][6]={{0}};
 unsigned targets[PT_COUNT],ports[PT_COUNT],buses[PT_COUNT]={0};
 unsigned target_count=pair?PT_COUNT:1;uint64_t bases[PT_COUNT][3]={{0}},sizes[PT_COUNT][3]={{0}};size_t list_total=0;
 for(unsigned t=0;t<PT_COUNT;t++){targets[t]=RC_FUNCTIONS;ports[t]=RC_FUNCTIONS;}
 if(!r)return RC_REJECTED;
 memset(r,0,sizeof(*r));
 if(!s || !s->functions || !s->roots || !s->queries || s->function_count!=RC_FUNCTIONS ||
    s->root_count!=RC_ROOTS || s->query_count!=RC_QUERIES)return rc_fail(r,RC_ARGUMENT,0,0);
 for(unsigned i=0;i<RC_FUNCTIONS;i++){
  const struct rc_function *f=&s->functions[i];unsigned pin=RC_FUNCTIONS;
  for(unsigned j=0;j<RC_FUNCTIONS;j++)if(rc_identities[j].bdf==f->bdf)pin=j;
  if(pin==RC_FUNCTIONS || identity_seen[pin]++ || f->header[0]!=rc_identities[pin].id ||
     f->header[2]!=rc_identities[pin].class_revision || ((f->header[3]>>16)&255)!=rc_identities[pin].header_type)
   return rc_fail(r,RC_IDENTITY,i,0);
  if(!f->conventional || f->conventional_count<8 || f->conventional_count>104)return rc_fail(r,RC_CONVENTIONAL,i,0);
  struct rc_cursor c={f->conventional,f->conventional_count,0};
  struct pci_conv_result walk=pci_conventional_walk(rc_trace,&c);
  if(walk.status!=PCI_CONV_NO_EA || c.pos!=c.count || walk.identity_raw!=f->header[0] ||
     walk.command_status_raw!=f->header[1] || walk.header_raw!=f->header[3] || walk.pointer_raw!=f->header[13])
   return rc_fail(r,RC_CONVENTIONAL,i,walk.error_offset);
  r->conventional_read_count+=(uint32_t)c.count;
  unsigned slots=((f->header[3]>>16)&127)?2:6;
  if(pair && s->raw_slot_present[i]!=(1U<<slots)-1)return rc_fail(r,RC_BAR,i,0);
  for(unsigned t=0;t<target_count;t++){
   if(f->bdf==pair_targets[t].endpoint)targets[t]=i;
   if((f->bdf>>8)==pair_targets[t].bus)buses[t]++;
  }
 }
 for(unsigned t=0;t<target_count;t++)
  if(targets[t]==RC_FUNCTIONS || buses[t]!=1)return rc_fail(r,RC_IDENTITY,t,0);
 for(unsigned i=0;i<RC_ROOTS;i++){
  const struct rc_root *root=&s->roots[i];unsigned count=0,buses=0;
  if(root->segment || root->status || !rc_descriptors(root->resources,desc,&count))return rc_fail(r,RC_ROOT,i,0);
  list_total+=root->resources.size;if(list_total>RC_LIST_TOTAL_MAX)return rc_fail(r,RC_ROOT,i,0);
  for(unsigned j=0;j<count;j++){
   const struct rc_desc *d=&desc[j];
   if(d->type==2){
    if(++buses!=1 || d->min>255 || d->max>255 || d->end!=d->max || (d->min&63) || d->max/64!=d->min/64)return rc_fail(r,RC_ROOT,i,j);
    unsigned bit=1U<<(unsigned)(d->min/64);if(root_seen&bit)return rc_fail(r,RC_ROOT,i,j);
    root_seen|=bit;domains[i]=(struct rc_domain){(unsigned)d->min,(unsigned)d->max};
   }else{
    /* AMI maximum may be an alignment mask, not an address. The full
     * min+length remains occupied; this is not a root forwarding aperture. */
    if(d->max!=d->end && !(d->max<UINT64_MAX && rc_power2(d->max+1) && !(d->min&d->max)))return rc_fail(r,RC_ROOT,i,j);
    if(d->type==0 && rc_collision(d->min,d->end,pair))return rc_fail(r,RC_COLLISION,i,j);
   }
  }
  if(buses!=1)return rc_fail(r,RC_ROOT,i,0);
 }
 if(root_seen!=15)return rc_fail(r,RC_ROOT,0,0);
 for(unsigned i=0;i<RC_FUNCTIONS;i++){
  int root=rc_domain_for(domains,s->functions[i].bdf>>8);if(root<0)return rc_fail(r,RC_ROOT,i,0);function_root[i]=(unsigned)root;
 }
 for(unsigned i=0;i<RC_FUNCTIONS;i++){
  const struct rc_function *f=&s->functions[i];const uint32_t *h=f->header;unsigned type=(h[3]>>16)&127;
  if(type>1)return rc_fail(r,RC_IDENTITY,i,0);
  r->raw_slot_count+=type?2:6;
  if(!type)continue;
  struct rc_bridge b={0};b.owner=i;b.root=function_root[i];b.secondary=(h[6]>>8)&255;b.subordinate=(h[6]>>16)&255;
  const struct rc_domain *d=&domains[b.root];unsigned bus=f->bdf>>8;int disabled=!b.secondary && !b.subordinate;
  if(h[2]>>8!=0x060400 || (h[6]&255)!=bus || (!disabled && !(d->first<b.secondary && b.secondary<=b.subordinate && b.subordinate<=d->last)))return rc_fail(r,RC_BRIDGE,i,0);
  b.top=(bus==d->first && !disabled);
  if(h[8]&0x000f000f)return rc_fail(r,RC_BRIDGE,i,8);
  b.lo[0]=(uint64_t)(h[8]&0xfff0)<<16;b.hi[0]=(h[8]&0xfff00000)|0xfffff;
  unsigned low=h[9]&15,high=(h[9]>>16)&15;
  if(low!=high || low>1 || (!low && (h[10]||h[11])))return rc_fail(r,RC_BRIDGE,i,9);
  b.lo[1]=(uint64_t)(h[9]&0xfff0)<<16;b.hi[1]=(h[9]&0xfff00000)|0xfffff;
  if(low){b.lo[1]|=(uint64_t)h[10]<<32;b.hi[1]|=(uint64_t)h[11]<<32;}
  for(unsigned j=0;j<2;j++)if(rc_collision(b.lo[j],b.hi[j],pair))return rc_fail(r,RC_COLLISION,i,j);
  if(b.top)for(unsigned j=0;j<bridge_count;j++){
   const struct rc_bridge *other=&bridges[j];
   if(other->top && other->root==b.root && !(b.subordinate<other->secondary || other->subordinate<b.secondary))return rc_fail(r,RC_BRIDGE,i,j);
  }
  for(unsigned t=0;t<target_count;t++)if(f->bdf==pair_targets[t].port){
   if(bus!=(pair_targets[t].port>>8) || b.secondary!=pair_targets[t].bus || b.subordinate!=pair_targets[t].bus)return rc_fail(r,RC_BRIDGE,i,0);
   if(pair)for(unsigned j=0;j<4;j++)if(h[8+j]!=pair_cold_windows[t][j])return rc_fail(r,RC_BRIDGE,i,8+j);
   ports[t]=bridge_count;
  }
  bridges[bridge_count++]=b;
 }
 r->bridge_count=bridge_count;
 if(r->raw_slot_count!=RC_QUERIES)return rc_fail(r,RC_BRIDGE,0,0);
 for(unsigned t=0;t<target_count;t++)if(ports[t]==RC_FUNCTIONS)return rc_fail(r,RC_BRIDGE,t,0);
 for(unsigned i=0;i<RC_FUNCTIONS;i++){
  const struct rc_function *f=&s->functions[i];unsigned count=((f->header[3]>>16)&127)?2:6;
  for(unsigned j=0;j<count;j++){
   uint32_t value=f->header[4+j];uint64_t base=value&UINT32_C(0xfffffff0);
   if(value&1){if(value&2)return rc_fail(r,RC_BAR,i,j);continue;}
   unsigned kind=value&6;/* No below-1MiB BAR exists on the pinned host. */
   if(kind!=0 && kind!=4)return rc_fail(r,RC_BAR,i,j);
   if(kind==4){
    if(j+1>=count)return rc_fail(r,RC_BAR,i,j);
    base|=(uint64_t)f->header[5+j]<<32;unsigned matches=0;const struct rc_bridge *bound=NULL;
    for(unsigned k=0;k<bridge_count;k++)if(bridges[k].top && bridges[k].root==function_root[i] &&
       bridges[k].secondary<=(f->bdf>>8) && (f->bdf>>8)<=bridges[k].subordinate){matches++;bound=&bridges[k];}
    if(matches!=1 || !base || !rc_in_windows(bound,base,base))return rc_fail(r,RC_BAR,i,j);
    r->upstream_bounded_64bit_bars++;
   }
   for(unsigned t=0;t<target_count;t++)if(i==targets[t]){
    if(j==0){if(kind || (value&8))return rc_fail(r,RC_BAR,i,j);bases[t][0]=base;}
    if(j==1 || j==3){if(kind!=4 || !(value&8))return rc_fail(r,RC_BAR,i,j);bases[t][j==1?1:2]=base;}
   }
   if(kind==4)j++;
  }
 }
 for(unsigned i=0;i<RC_QUERIES;i++){
  const struct rc_query *q=&s->queries[i];unsigned count=0;
  if(q->owner_index>=RC_FUNCTIONS)return rc_fail(r,RC_QUERY,i,0);
  unsigned slots=((s->functions[q->owner_index].header[3]>>16)&127)?2:6;
  if(q->query_index>=slots || query_seen[q->owner_index][q->query_index]++)return rc_fail(r,RC_QUERY,i,0);
  if(q->status==UINT64_C(0x8000000000000003)){
   if(q->resources.data || q->resources.size)return rc_fail(r,RC_QUERY,i,0);
  }else{
   if(q->status || !rc_descriptors(q->resources,desc,&count) || count>1)return rc_fail(r,RC_QUERY,i,0);
   list_total+=q->resources.size;if(list_total>RC_LIST_TOTAL_MAX)return rc_fail(r,RC_QUERY,i,0);
   if(count){
    const struct rc_desc *d=&desc[0];
    if(d->type>1 || !rc_power2(d->len) || (d->min&(d->len-1)) || (d->max!=d->len-1 && d->max!=d->end) ||
       (d->type==0 && d->gran!=32 && d->gran!=64) || (d->type==1 && d->gran!=0 && d->gran!=16 && d->gran!=32))return rc_fail(r,RC_QUERY,i,0);
    if(d->type==0){if(rc_collision(d->min,d->end,pair))return rc_fail(r,RC_COLLISION,i,0);r->query_memory_intervals++;}
   }
  }
  r->query_count++;
 }
 /* Exact count + unique legal owner/query pairs gives complete slot coverage. */
 const struct rc_memory *m=&s->memory;
 if(m->status || m->descriptor_version!=1 || m->descriptor_size<40 || m->descriptor_size>4096 || (m->descriptor_size&7) ||
    !m->raw.data || !m->raw.size || m->raw.size>RC_MAP_MAX || m->raw.size%m->descriptor_size || m->raw.size/m->descriptor_size>4096)return rc_fail(r,RC_MEMORY,0,0);
 for(size_t p=0;p<m->raw.size;p+=m->descriptor_size){
  const uint8_t *d=m->raw.data+p;uint64_t base=inv_le(d+8,8),pages=inv_le(d+24,8),end;unsigned type=(unsigned)inv_le(d,4);
  if(type>15 || (base&4095) || pages>UINT64_MAX/4096 || !rc_extent(base,pages*4096,&end))return rc_fail(r,RC_MEMORY,(unsigned)(p/m->descriptor_size),0);
  for(size_t k=0;k<p;k+=m->descriptor_size){
   const uint8_t *prior=m->raw.data+k;uint64_t start=inv_le(prior+8,8),stop=start+inv_le(prior+24,8)*4096-1;
   if(rc_overlap(base,end,start,stop))return rc_fail(r,RC_MEMORY,(unsigned)(p/m->descriptor_size),(unsigned)(k/m->descriptor_size));
  }
  if(type!=11 && type!=12){if(rc_collision(base,end,pair))return rc_fail(r,RC_COLLISION,(unsigned)(p/m->descriptor_size),0);r->efi_memory_intervals++;}
 }
 size_t aml_total=0;
 for(unsigned i=0;i<7;i++){
  const struct rc_blob *b=i?&s->acpi.ssdt[i-1]:&s->acpi.dsdt;
  if(!b->data || b->size<36 || b->size>RC_AML_TOTAL_MAX || b->size>RC_AML_TOTAL_MAX-aml_total)return rc_fail(r,RC_ACPI,i,0);
  aml_total+=b->size;if(memcmp(b->data,i?"SSDT":"DSDT",4) || inv_le(b->data+4,4)!=b->size)return rc_fail(r,RC_ACPI,i,0);
  uint8_t sum=0,digest[32];for(size_t j=0;j<b->size;j++)sum=(uint8_t)(sum+b->data[j]);inv_sha256(b->data,b->size,digest);
  if(sum || memcmp(digest,rc_aml_sha[i],32))return rc_fail(r,RC_ACPI,i,0);
 }
 for(unsigned t=0;t<target_count;t++){
  unsigned target=targets[t],port=ports[t];
  uint32_t bdf=pair?s->target_extcaps[t].bdf:s->target_extcap_bdf;
  const struct rc_read *trace=pair?s->target_extcaps[t].trace:s->target_extcap;
  size_t count=pair?s->target_extcaps[t].count:s->target_extcap_count;
  if(bdf!=pair_targets[t].endpoint || !trace || !count || count>1024)return rc_fail(r,RC_REBAR,target,0);
  struct rc_cursor cursor={trace,count,0};struct cap_result cap=decode_rebar(rc_trace,&cursor);
  if(cap.status!=CAP_FOUND || cap.offset!=0xbb0 || cap.count!=3 || cursor.pos!=cursor.count)return rc_fail(r,RC_REBAR,target,cap.error_offset);
  const unsigned indices[3]={0,1,3},selected[3]={4,6,5};
  for(unsigned j=0;j<3;j++){
   const struct rebar_entry *e=&cap.entries[j];uint64_t size=UINT64_C(1)<<(20+selected[j]),end;
   if((e->control&7)!=indices[j] || ((e->control>>8)&31)!=selected[j] || !(e->capability&(1U<<(selected[j]+4))) || !bases[t][j] ||
      (bases[t][j]&(size-1)) || !rc_extent(bases[t][j],size,&end) || !rc_in_windows(&bridges[port],bases[t][j],end))return rc_fail(r,RC_REBAR,target,j);
   if(rc_collision(bases[t][j],end,pair))return rc_fail(r,RC_COLLISION,target,j);
   if(pair && bases[t][j]!=pair_cold_bases[t][j])return rc_fail(r,RC_REBAR,target,j);
   sizes[t][j]=size;
  }
  if(pair && s->functions[target].header[9])return rc_fail(r,RC_BAR,target,5);
 }
 /* The seven hashes bind reviewed PCI0/root00 _CRS bytes. They establish the
  * trusted firmware advertised aperture, not observed physical DF decoding. */
 r->aperture_start=UINT64_C(0x63d20200000);r->aperture_end=UINT64_C(0x7fbffffffff);
 r->proposal_start=RC_PROPOSAL_START;r->proposal_end=RC_PROPOSAL_END;
 /* Each target binds to its own declared root interval: PCI0 for 01/02 and
  * S0D2 for 47/48. The UEFI root bus descriptor starts at the DSDT BRB and
  * stays inside BRB..BRB+BRL-1 (the firmware advertises the current bus
  * allocation, e.g. 00-05 and 40-4c, not the declared 64-bus range); the
  * port's root, the port's own bus and the DSDT aperture of that root must all
  * agree; targets declared on the same root must resolve to the same root
  * domain and targets on different roots to different ones. */
 for(unsigned t=0;t<target_count;t++){
  unsigned target=targets[t],port=ports[t];const struct pair_root *root=&pair_roots[t];
  uint64_t start=pair_targets[t].bar1,end=start+pair_targets[t].reservation_size-1;
  if(domains[function_root[target]].first!=root->first_bus || domains[function_root[target]].last>root->last_bus ||
     bridges[port].root!=function_root[target] || (pair_targets[t].port>>8)!=root->first_bus ||
     start<root->aperture_start || end>root->aperture_end)
   return rc_fail(r,RC_ROOT,target,0);
  for(unsigned u=0;u<t;u++)
   if((pair_roots[u].first_bus==root->first_bus)!=(function_root[targets[u]]==function_root[target]))
    return rc_fail(r,RC_ROOT,target,u);
 }
 memcpy(r->target_bases,bases[0],sizeof(r->target_bases));
 memcpy(r->target_sizes,sizes[0],sizeof(r->target_sizes));
 if(pair){memcpy(result->target_bases,bases,sizeof(bases));memcpy(result->target_sizes,sizes,sizeof(sizes));}
 r->trusted_firmware_aperture_match=1;r->standard_bar_collision_excluded=1;r->status=RC_COVERED;return RC_COVERED;
}
/* Historical single-target entry point retained solely for unchanged baseline
 * consumers; the pair path never invokes this entry point. */
static enum rc_status rc_validate(const struct rc_snapshot *s,struct rc_result *r)
{ return rc_validate_inventory(s,r,0,NULL); }
static enum rc_status rc_validate_pair(const struct rc_snapshot *s,struct rc_pair_result *r)
{
 if(!r)return RC_REJECTED;
 memset(r,0,sizeof(*r));
 if(!s || !pair_policy_matches(s->plans,s->plan_count))return rc_fail(&r->coverage,RC_ARGUMENT,0,0);
 /* Policy is fixed; keep its geometric invariants explicit before any inventory
  * reads. Neither the gap nor old target ranges receive an ownership exclusion. */
 for(unsigned t=0;t<PT_COUNT;t++){
  const struct pair_target *p=&s->plans[t];uint64_t end;
  if((p->bar1&(p->bar1_size-1)) || p->bar3!=p->bar1+p->bar1_size ||
     p->reservation_size!=p->bar1_size+p->bar3_size || !rc_extent(p->bar1,p->reservation_size,&end) ||
     end>=UINT64_C(0x1000000000000))return rc_fail(&r->coverage,RC_ARGUMENT,t,0);
 }
 for(unsigned a=0;a<PT_COUNT;a++)for(unsigned b=a+1;b<PT_COUNT;b++)
  if(rc_overlap(s->plans[a].bar1,s->plans[a].bar1+s->plans[a].reservation_size-1,
                s->plans[b].bar1,s->plans[b].bar1+s->plans[b].reservation_size-1))
   return rc_fail(&r->coverage,RC_ARGUMENT,a,b);
 return rc_validate_inventory(s,&r->coverage,1,r);
}
#endif
