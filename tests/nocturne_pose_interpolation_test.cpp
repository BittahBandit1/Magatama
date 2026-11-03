#include "nocturne_pose_interpolation.h"
#include <cstdlib>
#include <cstdio>
#define CHECK(condition) do { if(!(condition)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#condition);std::exit(1);} } while(0)
using namespace magatama;
using R=pose_trace::Record;
R packet(unsigned kind,uint64_t field,float x){R r;r.kind=kind;r.field=field;r.node=kind=='C'?0x2000:0x3000;r.root=kind=='C'?0x2f59b0:0x4000;r.shape=kind=='C'?0:0x5000;r.packet=kind=='C'?0x6000:0x7000;r.flags=1;if(kind=='C'){r.shape=0x8888;r.slot=0x9999;}r.bytes=kind=='C'?176:128;pose::Matrix m={1,0,0,0,0,1,0,0,0,0,1,0,x,0,0,1};memcpy(r.data.data(),m.data(),64);if(kind=='C'){pose::Matrix p={2,0,0,0,0,3,0,0,0,0,-1,1,0,0,32,0};auto b=pose::multiply(m,p);memcpy(r.data.data()+64,b.data(),64);}else{for(int i=0;i<3;i++){float one=1;memcpy(r.data.data()+64+16*i+4*i,&one,4);}}return r;}
R start(uint64_t field,unsigned mode,unsigned tag=0x1000){R s;s.field=field;s.kind='S';s.mode=mode;s.slot=0;s.flags=2;s.root=0x5293f8;s.packet=tag;return s;}
void endpoint(pose::Interpolator& e,uint64_t field,float x){auto c=packet('C',field,x),p=packet('P',field,x);e.publish(c);e.publish(p);e.begin(start(field,0));e.begin(start(field+1,1));uint8_t out[208];CHECK(!e.unpack(c.packet,0x6c0bc000,c.data.data(),176,out));CHECK(!e.unpack(p.packet,0x6c07c000,p.data.data(),112,out));e.finish(1,1,field+1);}
int main(){pose::Interpolator e;e.configure();endpoint(e,10,0);auto c=packet('C',12,10),p=packet('P',12,10);e.publish(c);e.publish(p);e.begin(start(12,0));uint8_t out[208];auto original=c.data;CHECK(e.unpack(c.packet,0x6c0bc000,c.data.data(),176,out));CHECK(original==c.data);float x;memcpy(&x,out+48,4);CHECK(x==5);CHECK(e.unpack(p.packet,0x6c07c000,p.data.data(),112,out));memcpy(&x,out+48,4);CHECK(x==5);
 original_frame::hostMenu=true;CHECK(!e.unpack(p.packet,0x6c07c000,p.data.data(),112,out));original_frame::hostMenu=false;
 original_frame::fastForward=true;CHECK(!e.unpack(p.packet,0x6c07c000,p.data.data(),112,out));original_frame::fastForward=false;
 auto corrupt=p.data;corrupt[0]^=1;CHECK(!e.unpack(p.packet,0x6c07c000,corrupt.data(),112,out));CHECK(!e.unpack(p.packet,0x6c06c000,p.data.data(),112,out));
 // A camera teleport disables model interpolation for that draw.
 endpoint(e,20,0);c=packet('C',22,1000);p=packet('P',22,10);e.publish(c);e.publish(p);e.begin(start(22,0));CHECK(!e.unpack(c.packet,0x6c0bc000,c.data.data(),176,out));CHECK(!e.unpack(p.packet,0x6c07c000,p.data.data(),112,out));
 // Object identity changes and frees cannot reuse previous pose history.
 endpoint(e,30,0);c=packet('C',32,10);p=packet('P',32,10);p.shape++;e.publish(c);e.publish(p);e.begin(start(32,0));CHECK(e.unpack(c.packet,0x6c0bc000,c.data.data(),176,out));CHECK(!e.unpack(p.packet,0x6c07c000,p.data.data(),112,out));
 endpoint(e,40,0);R freed;freed.kind='F';freed.node=0x3000;e.publish(freed);c=packet('C',42,10);p=packet('P',42,10);e.publish(c);e.publish(p);e.begin(start(42,0));CHECK(e.unpack(c.packet,0x6c0bc000,c.data.data(),176,out));CHECK(!e.unpack(p.packet,0x6c07c000,p.data.data(),112,out));
 // Camera scene/object or director state changes also break continuity.
 endpoint(e,50,0);c=packet('C',52,10);c.shape++;e.publish(c);e.begin(start(52,0));CHECK(!e.unpack(c.packet,0x6c0bc000,c.data.data(),176,out));
 endpoint(e,60,0);c=packet('C',62,10);c.mode++;e.publish(c);e.begin(start(62,0));CHECK(!e.unpack(c.packet,0x6c0bc000,c.data.data(),176,out));
 // A second unsupported/stale camera in the same phase revokes model continuity.
 // Multi-view draws must not inherit permission from an earlier recognized camera.
 for(unsigned failure=0;failure<3;++failure){
  endpoint(e,80+failure*10,0);const auto field=82+failure*10;
  c=packet('C',field,10);p=packet('P',field,10);e.publish(c);e.publish(p);e.begin(start(field,0));
  CHECK(e.unpack(c.packet,0x6c0bc000,c.data.data(),176,out));
  if(failure==0){auto other=c;other.packet+=0x100;CHECK(!e.unpack(other.packet,0x6c0bc000,other.data.data(),176,out));}
  if(failure==1){auto other=c;other.shape++;e.publish(other);CHECK(!e.unpack(other.packet,0x6c0bc000,other.data.data(),176,out));}
  if(failure==2){auto other=c.data;other[0]^=1;CHECK(!e.unpack(c.packet,0x6c0bc000,other.data(),176,out));}
  CHECK(!e.unpack(p.packet,0x6c07c000,p.data.data(),112,out));
 }
 // Cross-kind source aliases must never interpret a model as a camera or
 // read the 176-byte camera layout from a 112-byte model command.
 endpoint(e,120,0);c=packet('C',122,10);p=packet('P',122,10);e.publish(c);e.publish(p);e.begin(start(122,0));
 CHECK(e.unpack(c.packet,0x6c0bc000,c.data.data(),176,out));
 uint8_t shortPayload[112];memcpy(shortPayload,c.data.data(),112);
 CHECK(!e.unpack(c.packet,0x6c07c000,shortPayload,112,out));
 CHECK(!e.unpack(p.packet,0x6c0bc000,p.data.data(),176,out));
 // A mismatched completed draw pair discards history rather than crossing scenes.
 e.begin(start(63,1,0x9999));e.finish(1,1,63);c=packet('C',64,10);e.publish(c);e.begin(start(64,0));CHECK(!e.unpack(c.packet,0x6c0bc000,c.data.data(),176,out));
 endpoint(e,70,0);e.reset();c=packet('C',72,10);p=packet('P',72,10);e.publish(c);e.publish(p);e.begin(start(72,0));CHECK(!e.unpack(c.packet,0x6c0bc000,c.data.data(),176,out));CHECK(!e.unpack(p.packet,0x6c07c000,p.data.data(),112,out));
 puts("PASS: immutable original endpoints; distinct midpoint; host menu/fast-forward bypass; byte identity; command bounds; camera teleport; model shape/free identity; mismatched pair invalidation");
}
