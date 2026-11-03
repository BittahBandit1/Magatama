#pragma once
#include <array>
#include <cmath>
#include <cstring>
#include <algorithm>

// Rendering-copy math only. Matrices are the original row-vector SDF layout.
namespace magatama::pose {
using Matrix=std::array<float,16>;
struct TRS {std::array<float,3> t{},s{};std::array<float,4> q{};};
inline bool finite(const Matrix& m){for(float v:m)if(!std::isfinite(v))return false;return true;}
inline float dot3(const float* a,const float* b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline bool decompose(const Matrix& m,TRS& p){
 if(!finite(m)||std::fabs(m[3])+std::fabs(m[7])+std::fabs(m[11])>1e-5f||std::fabs(m[15]-1)>1e-5f)return false;
 Matrix r=m;for(unsigned i=0;i<3;++i){p.s[i]=std::sqrt(dot3(&m[4*i],&m[4*i]));if(p.s[i]<1e-5f||p.s[i]>1000)return false;for(unsigned j=0;j<3;++j)r[4*i+j]/=p.s[i];p.t[i]=m[12+i];}
 if(std::fabs(dot3(&r[0],&r[4]))>0.015f||std::fabs(dot3(&r[0],&r[8]))>0.015f||std::fabs(dot3(&r[4],&r[8]))>0.015f)return false;
 const float det=r[0]*(r[5]*r[10]-r[6]*r[9])-r[1]*(r[4]*r[10]-r[6]*r[8])+r[2]*(r[4]*r[9]-r[5]*r[8]);if(det<0.98f||det>1.02f)return false;
 // Convert the transpose (column-vector rotation) to a quaternion.
 const float a[3][3]={{r[0],r[4],r[8]},{r[1],r[5],r[9]},{r[2],r[6],r[10]}};
 float tr=a[0][0]+a[1][1]+a[2][2];auto& q=p.q;
 if(tr>0){float s=2*std::sqrt(tr+1);q={ (a[2][1]-a[1][2])/s,(a[0][2]-a[2][0])/s,(a[1][0]-a[0][1])/s,s/4};}
 else {unsigned i=a[1][1]>a[0][0]?1:0;if(a[2][2]>a[i][i])i=2;unsigned j=(i+1)%3,k=(j+1)%3;float s=2*std::sqrt(1+a[i][i]-a[j][j]-a[k][k]);if(s<1e-6f)return false;q[i]=s/4;q[j]=(a[i][j]+a[j][i])/s;q[k]=(a[i][k]+a[k][i])/s;q[3]=(a[k][j]-a[j][k])/s;}
 float n=0;for(float v:q)n+=v*v;if(n<1e-6f)return false;for(float& v:q)v/=std::sqrt(n);return true;
}
inline Matrix compose(const TRS& p){
 const float x=p.q[0],y=p.q[1],z=p.q[2],w=p.q[3];
 Matrix m={1-2*(y*y+z*z),2*(x*y+w*z),2*(x*z-w*y),0,
 2*(x*y-w*z),1-2*(x*x+z*z),2*(y*z+w*x),0,
 2*(x*z+w*y),2*(y*z-w*x),1-2*(x*x+y*y),0,p.t[0],p.t[1],p.t[2],1};
 for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)m[i*4+j]*=p.s[i];return m;
}
inline bool interpolate(const Matrix& a,const Matrix& b,float alpha,Matrix& result,float maxDistance=100.f){
 if(!(alpha>=0&&alpha<=1))return false;TRS x,y;if(!decompose(a,x)||!decompose(b,y))return false;
 float distance=0;for(unsigned i=0;i<3;++i){float d=y.t[i]-x.t[i];distance+=d*d;if(y.s[i]/x.s[i]<0.8f||y.s[i]/x.s[i]>1.25f)return false;}
 if(distance>maxDistance*maxDistance)return false;
 float d=0;for(unsigned i=0;i<4;++i)d+=x.q[i]*y.q[i];if(d<0){d=-d;for(float& v:y.q)v=-v;}
 // Reject discontinuous rotations (>60degrees/update), including camera cuts.
 if(d<0.8660254f)return false;
 if(alpha==0){result=a;return true;}if(alpha==1){result=b;return true;}
 TRS p;float l=1-alpha,r=alpha;if(d<0.9995f){float theta=std::acos(std::clamp(d,-1.f,1.f)),den=std::sin(theta);l=std::sin((1-alpha)*theta)/den;r=std::sin(alpha*theta)/den;}
 float norm=0;for(unsigned i=0;i<4;++i){p.q[i]=l*x.q[i]+r*y.q[i];norm+=p.q[i]*p.q[i];}for(float& v:p.q)v/=std::sqrt(norm);
 for(unsigned i=0;i<3;++i){p.t[i]=x.t[i]+alpha*(y.t[i]-x.t[i]);p.s[i]=x.s[i]+alpha*(y.s[i]-x.s[i]);}result=compose(p);return finite(result);
}
inline Matrix multiply(const Matrix& a,const Matrix& b){Matrix m{};for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)m[i*4+j]+=a[i*4+k]*b[k*4+j];return m;}
inline bool inverseAffine(const Matrix& a,Matrix& m){
 if(!finite(a)||std::fabs(a[3])+std::fabs(a[7])+std::fabs(a[11])>1e-5f||std::fabs(a[15]-1)>1e-5f)return false;
 const float d=a[0]*(a[5]*a[10]-a[6]*a[9])-a[1]*(a[4]*a[10]-a[6]*a[8])+a[2]*(a[4]*a[9]-a[5]*a[8]);if(std::fabs(d)<1e-8f)return false;
 m={ (a[5]*a[10]-a[6]*a[9])/d,(a[2]*a[9]-a[1]*a[10])/d,(a[1]*a[6]-a[2]*a[5])/d,0,
 (a[6]*a[8]-a[4]*a[10])/d,(a[0]*a[10]-a[2]*a[8])/d,(a[2]*a[4]-a[0]*a[6])/d,0,
 (a[4]*a[9]-a[5]*a[8])/d,(a[1]*a[8]-a[0]*a[9])/d,(a[0]*a[5]-a[1]*a[4])/d,0,0,0,0,1};
 for(unsigned j=0;j<3;++j)for(unsigned k=0;k<3;++k)m[12+j]-=a[12+k]*m[k*4+j];return finite(m);
}
inline bool modelPacket(const uint8_t* old,const uint8_t* now,float alpha,uint8_t* out){
 Matrix a,b,m;std::memcpy(a.data(),old,64);std::memcpy(b.data(),now,64);if(!interpolate(a,b,alpha,m))return false;
 std::memcpy(out,now,112);if(alpha==1)return true;std::memcpy(out,m.data(),64);
 // The original lighting rows normalize each matrix column's XYZ length.
 for(unsigned j=0;j<3;++j){float n=std::sqrt(m[j]*m[j]+m[4+j]*m[4+j]+m[8+j]*m[8+j]);if(n<1e-6f)return false;for(unsigned i=0;i<3;++i){float v=m[i*4+j]/n;std::memcpy(out+64+16*i+4*j,&v,4);}}return true;
}
inline bool cameraPacket(const uint8_t* old,const uint8_t* now,float alpha,uint8_t* out){
 Matrix a,b,aw,bw,m,mw,pa,pb;std::memcpy(a.data(),old,64);std::memcpy(b.data(),now,64);
 if(!inverseAffine(a,aw)||!inverseAffine(b,bw)||!interpolate(aw,bw,alpha,mw,150.f)||!inverseAffine(mw,m))return false;
 Matrix ab,bb;std::memcpy(ab.data(),old+64,64);std::memcpy(bb.data(),now+64,64);pa=multiply(aw,ab);pb=multiply(bw,bb);
 // Preserve projection/viewport changes immediately, not through unrelated views.
 for(unsigned i=0;i<16;++i)if(!std::isfinite(pa[i])||!std::isfinite(pb[i])||std::fabs(pa[i]-pb[i])>0.002f)return false;
 if(std::memcmp(old+128,now+128,32)!=0)return false;
 std::memcpy(out,now,176);if(alpha==1)return true;auto combined=multiply(m,pb);std::memcpy(out,m.data(),64);std::memcpy(out+64,combined.data(),64);std::memcpy(out+160,mw.data()+12,16);return true;
}
} // namespace magatama::pose
