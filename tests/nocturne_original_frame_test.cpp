#include "rrv_nocturne_original_frame.h"
#include <cassert>
#include <fstream>
#include <sstream>
#include <string>
#include <iostream>
#include <map>
using namespace magatama::original_frame;
int main(int argc,char**argv) {
 PairTracker t;Selector s;Delivery d{1,1,1,0,123,0,0,2};
 assert(!s.field(1,t.deliver(d),true,false,false).present);
 d.field=2;d.mode=d.count=1;d.kernel=2; // next simulation may begin before phase1
 auto complete=t.deliver(d);assert(complete.completed==1);
 assert(s.field(2,complete,true,false,false).present);
 assert(!s.field(3,complete,true,false,false).present);
 assert(s.field(3,complete,true,true,false).present);
 assert(s.field(3,complete,false,false,false).present);
 assert(s.field(3,complete,true,false,true).present);
 assert(s.field(6,complete,true,false,false).present);
 assert(s.field(1,complete,true,false,false).present);
 d.field=7;d.mode=d.count=0;d.kernel=d.request=0xffffffffu;
 t.deliver(d);d.field=8;d.mode=d.count=1;assert(t.deliver(d).completed==2);
 d.field=9;d.mode=d.count=0;d.kernel=d.request=0;t.deliver(d);
 d.field=10;d.mode=d.count=1;assert(t.deliver(d).completed==3);
 d.field=11;d.mode=d.count=0;t.deliver(d);d.field=12;d.mode=d.count=1;d.tag=456;
 assert(!t.deliver(d).supported);
 d.field=13;d.mode=d.count=0;t.deliver(d);d.field=17;d.mode=d.count=1;assert(!t.deliver(d).supported);
 d.cadence=1;assert(!t.deliver(d).supported);d.cadence=2;d.slot=3;assert(!t.deliver(d).supported);
 d.slot=0;d.mode=3;assert(!t.deliver(d).supported);
 d.mode=1;d.count=0;assert(!t.deliver(d).supported);
 std::cout<<"PASS: complete-pair identity, no early duplicate, menu/field/fast-forward bypass, stale/regressed receipts, counter wrap, mixed pair rejection\n";
 if(argc<2)return 0;
 PairTracker real;Selector select;std::ifstream f(argv[1]);std::string line;
 std::ofstream selected;if(argc>2)selected.open(argv[2]);
 struct Stats {uint64_t fields=0,selected=0,fallback=0;std::map<uint64_t,uint64_t> gaps;uint64_t last=0;};
 Stats all,walk,battle,effect;
 auto count=[&](Stats&v,uint64_t field,Decision decision){
  ++v.fields;if(decision.fields_per_present==1)++v.fallback;
  if(decision.present){++v.selected;if(v.last)++v.gaps[field-v.last];v.last=field;}
 };
 while(std::getline(f,line)) {
  if(line.empty()||line[0]=='#')continue;
  std::istringstream in(line);char kind;uint64_t ns,field,serial,df;uint32_t kernel,request,queue,tag,mode,buffer,half,count_,cadence,slot;
  in>>kind>>ns>>field>>serial>>df>>kernel>>request>>queue>>std::hex>>tag>>std::dec>>mode>>buffer>>half>>count_>>cadence>>slot;
  assert(in);
  if(kind=='D')real.deliver({df,kernel,request,slot,tag,mode,count_,cadence});
  else {
   auto q=select.field(field,real.snapshot(),true,false,false);count(all,field,q);
   if(selected && q.present)selected<<ns<<" "<<field<<" "<<real.snapshot().completed<<" "<<q.fields_per_present<<"\n";
   if(field>=1200 && field<1800)count(walk,field,q);
   if(field>=2000 && field<2400)count(battle,field,q);
   if(field>=3300 && field<3600)count(effect,field,q);
  }
 }
 for(auto item:{std::pair{"all",all},std::pair{"walk",walk},std::pair{"battle",battle},std::pair{"effect",effect}}) {
  const auto&[name,v]=item;std::cout<<name<<" fields="<<v.fields<<" selected="<<v.selected<<" fallback="<<v.fallback<<" gaps=";
  for(auto[a,b]:v.gaps)std::cout<<a<<":"<<b<<",";std::cout<<"\n";
 }
}
