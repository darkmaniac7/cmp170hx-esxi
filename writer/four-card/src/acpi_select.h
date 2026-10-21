#ifndef CMP_WRITER_ACPI_SELECT_H
#define CMP_WRITER_ACPI_SELECT_H
#include "../pins/acpi_export.h"
#include "../pins/resource_types.h"
#include "inventory_sha256.h"
#include <string.h>
static int ws_select(const struct ae_result *r,const uint8_t *workspace,size_t capacity,
                     const uint8_t pins[7][32],struct rc_acpi *out){
 if(!out)return 0;
 memset(out,0,sizeof *out);
 if(!r||!workspace||!pins||r->status!=AE_OK||r->route_authorized||
    !r->table_count||r->table_count>AE_MAX_TABLES||!r->total_bytes||
    r->total_bytes>AE_TOTAL_LIMIT||r->total_bytes>capacity)return 0;
 struct rc_acpi selected={0};unsigned seen[7]={0},dsdt=0;size_t total=0;
 for(unsigned i=0;i<r->table_count;i++){
  const struct ae_table *t=&r->tables[i];
  if(t->kind>AE_OTHER||!t->length||t->length>AE_TABLE_LIMIT||
     t->workspace_offset>r->total_bytes||t->length>r->total_bytes-t->workspace_offset||
     t->length>r->total_bytes-total)return 0;
  for(unsigned j=0;j<i;j++){
   const struct ae_table *p=&r->tables[j];
   if(t->workspace_offset<(uint64_t)p->workspace_offset+p->length&&
      p->workspace_offset<(uint64_t)t->workspace_offset+t->length)return 0;
  }
  total+=t->length;
  if(t->kind!=AE_DSDT&&t->kind!=AE_SSDT)continue;
  const uint8_t *p=workspace+t->workspace_offset;uint8_t digest[32];
  if(t->length<36||memcmp(p,t->kind==AE_DSDT?"DSDT":"SSDT",4)||
     ae_le(p+4,4)!=t->length||!ae_checksum(p,t->length))return 0;
  if(t->kind==AE_DSDT&&++dsdt!=1)return 0;
  inv_sha256(p,t->length,digest);unsigned matched=0;
  for(unsigned j=0;j<7;j++)if(!memcmp(digest,pins[j],32)){
   if(++seen[j]!=1||(j==0)!=(t->kind==AE_DSDT))return 0;
   matched++;
   struct rc_blob b={p,t->length};if(!j)selected.dsdt=b;else selected.ssdt[j-1]=b;
  }
  if(matched!=1)return 0;
 }
 if(total!=r->total_bytes||dsdt!=1)return 0;
 for(unsigned i=0;i<7;i++)if(seen[i]!=1)return 0;
 *out=selected;return 1;
}
#endif
