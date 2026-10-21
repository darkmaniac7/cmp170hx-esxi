#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bar_transaction.h"
struct event {char op;uint32_t bdf,off,width,value;};
static unsigned target_index;
struct fake {uint32_t endpoint,bridge;uint32_t ep[1024],br[1024];struct event events[1024];unsigned n,fail_at,fail_mode,drift_bar3;};
static uint32_t *reg(struct fake *f,uint32_t bdf,uint16_t off){assert(bdf==f->endpoint||bdf==f->bridge);assert(off<4096);return bdf==f->endpoint?&f->ep[off/4]:&f->br[off/4];}
static int read_cfg(void *ctx,uint32_t bdf,uint16_t off,unsigned width,uint32_t *value){struct fake *f=ctx;assert(width==2||width==4);assert(!(off&(width-1)));*value=(*reg(f,bdf,off)>>((off&3)*8))&(width==2?0xffffU:0xffffffffU);f->events[f->n++]=(struct event){'r',bdf,off,width,*value};if(f->n==f->fail_at){if(f->fail_mode==1)*value=width==2?0xffffU:0xffffffffU;else if(f->fail_mode==2)*value^=1;else return (int)width-1;}return (int)width;}
static int write_cfg(void *ctx,uint32_t bdf,uint16_t off,unsigned width,uint32_t value){struct fake *f=ctx;assert(width==2||width==4);assert(!(off&(width-1)));assert(bdf==f->endpoint||bdf==f->bridge);if(off!=4){assert((f->ep[1]&6)==0&&(f->br[1]&6)==0);}
 if(bdf==f->bridge&&off==4)assert((f->ep[1]&6)==0);
 f->events[f->n++]=(struct event){'w',bdf,off,width,value};uint32_t *p=reg(f,bdf,off);if(width==2)*p=(*p&0xffff0000U)|value;else *p=value;
 if(bdf==f->endpoint&&off==0xdcc){f->ep[0xbbc/4]=BT_CFG_CAP_OBSERVED;if(f->drift_bar3)f->ep[0xbc8/4]^=1U<<8;}
 if(f->n==f->fail_at)return (int)width-1;
 return (int)width;}
static int owned(void *ctx,const struct bt_plan *p){struct fake *f=ctx;assert(p->reservation_owner==0x1234);f->events[f->n++]=(struct event){'g',0,0,0,0};return f->n!=f->fail_at;}
static int cfg32(void *ctx,uint32_t off,uint32_t *v){struct fake *f=ctx;*v=f->ep[off/4];return 0;}
static void fixture(struct fake *f,struct bt_plan *p){memset(f,0,sizeof(*f));memset(p,0,sizeof(*p));const struct pair_target *t=&pair_targets[target_index];f->endpoint=t->endpoint;f->bridge=t->port;
 f->ep[0]=0x20c210de;f->br[0]=0x14831022;f->ep[1]=7;f->br[1]=7;f->ep[2]=0x030200a1;f->br[2]=0x06040001;f->br[3]=0x00010000;
 f->ep[4]=0xf3000000;f->ep[5]=0xfc00000c;f->ep[6]=0x7fb;f->ep[7]=0xfa00000c;f->ep[8]=0x7fb;
 f->br[6]=(t->bus<<16)|(t->bus<<8)|(t->port>>8);f->br[8]=0xf3f0f300;f->br[9]=0xfff1fa01;f->br[10]=0x7fb;f->br[11]=0x7fb;
 f->br[0x50/4]=0x00420010;f->ep[0x78/4]=0x00020010;f->ep[0x80/4]=0;
 f->ep[0x100/4]=(0xbb0U<<20)|0x10001;f->ep[0xbb0/4]=0x10015;
 f->ep[0xbb4/4]=1U<<8;f->ep[0xbb8/4]=(3U<<5)|(4U<<8);
 f->ep[0xbbc/4]=1U<<10;f->ep[0xbc0/4]=0x2000|(6U<<8)|1;
 f->ep[0xbc4/4]=1U<<9;f->ep[0xbc8/4]=(5U<<8)|3;
 f->ep[0x724/4]=0x10;f->ep[0xdcc/4]=2;
 p->apply=1;p->proofs=BT_REQUIRED_PROOFS;p->target_index=target_index;p->endpoint=f->endpoint;p->bridge=f->bridge;p->pcie_cap=0x78;p->bridge_pcie_cap=0x50;
 p->bar1_base=t->bar1;p->bar1_size=BT_BAR1_SIZE;p->bar3_base=p->bar1_base+BT_BAR1_SIZE;p->bar3_size=BT_BAR3_SIZE;
 p->bridge_base=p->bar1_base;p->bridge_limit=p->bar3_base+BT_BAR3_SIZE-1;p->root_start=pair_roots[target_index].aperture_start;p->root_end=pair_roots[target_index].aperture_end;
 p->reservation_base=p->bar1_base;p->reservation_length=BT_SPAN;p->reservation_owner=0x1234;
 memcpy(p->endpoint_before,f->ep,64);memcpy(p->bridge_before,f->br,64);p->cya_before=f->ep[0x724/4];p->cfg_before=f->ep[0xdcc/4];p->rebar_before=decode_rebar(cfg32,f);
}
static struct bt_io io_for(struct fake *f){return (struct bt_io){f,read_cfg,write_cfg,owned};}
static unsigned writes(const struct fake *f){unsigned n=0;for(unsigned i=0;i<f->n;i++)n+=f->events[i].op=='w';return n;}
int main(void){for(target_index=0;target_index<PT_COUNT;target_index++){struct fake f;struct bt_plan p;struct bt_state s={0};fixture(&f,&p);struct bt_io io=io_for(&f);
 assert(bt_apply(&p,&io,&s)==BT_APPLIED);assert((f.ep[1]&6)==0);assert(f.br[1]==7);assert(f.ep[0xbc0/4]==0x3001);assert(f.ep[0x724/4]==0x30&&f.ep[0xdcc/4]==0x8000000a);
 assert((((uint64_t)f.ep[6]<<32)|(f.ep[5]&~15U))==p.bar1_base);assert((((uint64_t)f.ep[8]<<32)|(f.ep[7]&~15U))==p.bar3_base);
 assert(f.ep[4]==p.endpoint_before[4]&&f.br[8]==p.bridge_before[8]);assert(s.write_attempted&&writes(&f)==13);
 unsigned success_n=f.n,cases=1;struct event trace[1024];memcpy(trace,f.events,sizeof(trace));
 for(unsigned at=1;at<=success_n;at++)for(unsigned mode=0;mode<2;mode++){
  if(mode==1&&trace[at-1].op!='r')continue;
  fixture(&f,&p);memset(&s,0,sizeof(s));f.fail_at=at;f.fail_mode=mode;io=io_for(&f);enum bt_status got=bt_apply(&p,&io,&s);
  assert(got==(s.write_attempted?BT_COLD_RECOVERY_REQUIRED:BT_REFUSED));assert(f.n==at);unsigned before=f.n;assert(bt_apply(&p,&io,&s)==got);assert(f.n==before);cases++;
 }
 for(unsigned selected=0;selected<32;selected++)for(unsigned supported=0;supported<2;supported++){
  fixture(&f,&p);memset(&s,0,sizeof s);f.ep[0xbc0/4]=(f.ep[0xbc0/4]&~0x1f00U)|(selected<<8);
  f.ep[0xbbc/4]=supported&&selected<=19?(1U<<(selected+4)):1;
  p.rebar_before=decode_rebar(cfg32,&f);io=io_for(&f);
  if(supported&&selected>=6&&selected<=16)assert(bt_apply(&p,&io,&s)==BT_APPLIED);
  else if(supported&&selected>=17&&selected<=19)assert(bt_apply(&p,&io,&s)==BT_COLD_RECOVERY_REQUIRED&&writes(&f)==4);
  else assert(bt_apply(&p,&io,&s)==BT_REFUSED&&!writes(&f));
  cases++;
 }
 for(unsigned bit=0;bit<9;bit++){fixture(&f,&p);memset(&s,0,sizeof(s));p.proofs&=~(1U<<bit);io=io_for(&f);assert(bt_apply(&p,&io,&s)==BT_REFUSED&&!writes(&f));cases++;}
 for(unsigned bad=0;bad<25;bad++){fixture(&f,&p);memset(&s,0,sizeof(s));switch(bad){case 0:p.apply=0;break;case 1:p.endpoint++;break;case 2:p.bridge++;break;case 3:p.bar1_base++;break;case 4:p.bar3_base++;break;case 5:p.bridge_limit++;break;case 6:p.bar1_size--;break;case 7:p.bar3_size--;break;case 8:p.reservation_length--;break;case 9:p.root_translation=1;break;case 10:p.root_end=p.bridge_limit-1;break;case 11:f.ep[0]^=1;break;case 12:f.br[0]^=1;break;case 13:f.ep[0x80/4]=0x00200000;break;case 14:p.bar1_base=UINT64_MAX&~(BT_BAR1_SIZE-1);break;case 15:p.pcie_cap=0xff;break;case 16:f.ep[1]=p.endpoint_before[1]=0xffff;break;case 17:f.br[1]=p.bridge_before[1]=0xffff;break;case 18:p.bridge_pcie_cap=0xff;break;case 20:p.target_index=(p.target_index+1)%PT_COUNT;break;case 21:p.target_index=PT_COUNT;break;case 22:p.bar1_base+=BT_BAR1_SIZE;p.bar3_base+=BT_BAR1_SIZE;p.bridge_base+=BT_BAR1_SIZE;p.bridge_limit+=BT_BAR1_SIZE;p.reservation_base+=BT_BAR1_SIZE;break;case 23:p.root_start=pair_roots[(target_index+2)%PT_COUNT].aperture_start;p.root_end=pair_roots[(target_index+2)%PT_COUNT].aperture_end;break;case 24:f.br[6]^=0x40;p.bridge_before[6]^=0x40;break;case 19:f.ep[0xbc8/4]=(4U<<8)|3;f.ep[0xbc4/4]=1U<<8;p.rebar_before=decode_rebar(cfg32,&f);break;}io=io_for(&f);assert(bt_apply(&p,&io,&s)==BT_REFUSED&&!writes(&f));cases++;}
 for(unsigned at=2;at<=success_n;at++)if(trace[at-2].op=='w'&&trace[at-1].op=='r'){
  fixture(&f,&p);memset(&s,0,sizeof(s));f.fail_at=at;f.fail_mode=2;io=io_for(&f);
  assert(bt_apply(&p,&io,&s)==BT_COLD_RECOVERY_REQUIRED&&f.n==at);cases++;
 }
 fixture(&f,&p);memset(&s,0,sizeof(s));f.drift_bar3=1;io=io_for(&f);
 assert(bt_apply(&p,&io,&s)==BT_COLD_RECOVERY_REQUIRED);assert((f.br[1]&6)==0);cases++;
 printf("%u guarded transaction scenarios PASS (%u success-path callbacks)\n",cases,success_n); }return 0;}
