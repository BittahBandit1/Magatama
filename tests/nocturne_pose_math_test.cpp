#include "nocturne_pose_math.h"
#include <cstdlib>
#include <cstdio>
#define CHECK(condition) do { if(!(condition)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#condition);std::exit(1);} } while(0)
#include <cstdio>
#include <limits>
using namespace magatama::pose;
int main(){unsigned checks=0;for(int i=0;i<1000;i++){
 float theta=(i-500)*.003f;TRS p;p.q={0,std::sin(theta/2),0,std::cos(theta/2)};p.s={1.2f,0.9f,1.4f};p.t={float(i),20,-50};auto m=compose(p);TRS q;CHECK(decompose(m,q));auto rebuilt=compose(q);for(int j=0;j<16;j++){CHECK(std::fabs(m[j]-rebuilt[j])<.0004f);++checks;}Matrix inv;CHECK(inverseAffine(m,inv));auto id=multiply(m,inv);for(int j=0;j<16;j++){CHECK(std::fabs(id[j]-(j%5==0?1:0))<.0005f);++checks;}
 auto next=p;next.t[0]+=10;next.q={0,std::sin((theta+.2f)/2),0,std::cos((theta+.2f)/2)};auto n=compose(next);Matrix mid;CHECK(interpolate(m,n,.5,mid));TRS z;CHECK(decompose(mid,z));CHECK(std::fabs(z.t[0]-(p.t[0]+5))<.0001f);auto exp=p;exp.t[0]+=5;exp.q={0,std::sin((theta+.1f)/2),0,std::cos((theta+.1f)/2)};auto expected=compose(exp);for(int j=0;j<16;j++){CHECK(std::fabs(mid[j]-expected[j])<.0005f);++checks;}CHECK(interpolate(m,n,1,mid)&&mid==n);
 }
 TRS p;p.s={1,1,1};p.q={0,0,0,1};auto a=compose(p),b=a;Matrix out;b[12]=1000;CHECK(!interpolate(a,b,.5,out));b=a;b[0]=std::numeric_limits<float>::quiet_NaN();CHECK(!interpolate(a,b,.5,out));b=a;b[4]=.5;CHECK(!interpolate(a,b,.5,out));b=a;b[0]=-1;CHECK(!interpolate(a,b,.5,out));p.q={0,1,0,0};b=compose(p);CHECK(!interpolate(a,b,.5,out));
 // Camera interpolation preserves viewport bytes and rebuilds both projection
 // transform and inverse eye origin from one interpolated world camera.
 std::array<uint8_t,176> ca{},cb{},co{};auto aw=a;aw[12]=10;Matrix bv;CHECK(inverseAffine(aw,bv));Matrix proj={2,0,0,0,0,3,0,0,0,0,-1,1,0,0,32,0};auto ap=multiply(a,proj),bp=multiply(bv,proj);memcpy(ca.data(),a.data(),64);memcpy(ca.data()+64,ap.data(),64);memcpy(cb.data(),bv.data(),64);memcpy(cb.data()+64,bp.data(),64);CHECK(cameraPacket(ca.data(),cb.data(),.5,co.data()));Matrix view;memcpy(view.data(),co.data(),64);CHECK(std::fabs(view[12]+5)<1e-5);float eye;memcpy(&eye,co.data()+160,4);CHECK(std::fabs(eye-5)<1e-5);cb[128]=1;CHECK(!cameraPacket(ca.data(),cb.data(),.5,co.data()));
 printf("PASS: %u random quaternion/TRS/inverse/midpoint checks; exact endpoints; NaN/shear/reflection/teleport/rotation-cut rejection; consistent camera eye/projection and viewport-change rejection\n",checks);
}
