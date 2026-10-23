#include "../Source/VoiceFemEngine.h"
#include <cstdio>
#include <chrono>
#include <random>
using namespace vfem;

struct Res { double a1=0,a2=0,b1=0,b2=0,y1=0,y2=0; double process(double x,double f,double sr,double bw){ double r=std::exp(-kPi*bw/sr); double th=2*kPi*f/sr; double A1=-2*r*std::cos(th),A2=r*r; double y=(1-r)*x - A1*y1 - A2*y2; y2=y1;y1=y; return y;} };

static double estF0(const std::vector<float>& x,int s,int len,double sr){
  double best=0;int bl=0; double e0=0; for(int i=0;i<len;i++) e0+=x[s+i]*x[s+i];
  for(int lag=(int)(sr/400); lag<(int)(sr/70); lag++){ double a=0,e1=0; for(int i=0;i<len-lag;i++){a+=x[s+i]*x[s+i+lag]; e1+=x[s+i+lag]*x[s+i+lag];} double sc=a/std::sqrt(e0*e1+1e-12); if(sc>best){best=sc;bl=lag;} }
  return sr/bl;
}
static double centroid(const std::vector<float>& x,int s,int len,double sr){
  // crude DFT centroid 100..5000Hz
  double num=0,den=0; for(double f=100;f<5000;f+=25){ double re=0,im=0; for(int i=0;i<len;i++){ double w=0.5-0.5*std::cos(2*kPi*i/len); re+=w*x[s+i]*std::cos(2*kPi*f*i/sr); im+=w*x[s+i]*std::sin(2*kPi*f*i/sr);} double p=re*re+im*im; num+=f*p; den+=p;} return num/den;
}

int main(){
  const double sr=48000; const int N=(int)(sr*3);
  std::vector<float> x(N);
  Res r1,r2,r3; double ph=0; std::mt19937 g(1); std::normal_distribution<double> nd(0,1);
  for(int i=0;i<N;i++){
    double t=i/sr; double f0=110*(1+0.03*std::sin(2*kPi*5*t));
    ph+=f0/sr; double imp=0; if(ph>=1){ph-=1; imp=1;}
    double src=imp*sr/f0*0.02;
    double y=r1.process(src,650,sr,80)+0.7*r2.process(src,1250,sr,100)+0.4*r3.process(src,2600,sr,150);
    x[i]=(float)(y*0.5);
  }
  double mx=0; for(float v:x) mx=std::max(mx,(double)std::fabs(v)); for(auto&v:x) v=(float)(v/mx*0.5);
  for(auto cfg: {0,1,2,3}){
    Params P; P.mix=1; P.voicingThresh=0.55f;
    if(cfg==0){P.pitchSemis=0;P.formantSemis=0;}
    if(cfg==1){P.pitchSemis=4;P.formantSemis=0;}
    if(cfg==2){P.pitchSemis=4;P.formantSemis=4;}
    if(cfg==3){P.targetMode=true;P.targetHz=200;P.formantSemis=2;}
    Engine e; e.prepare(sr,80); e.setParams(P,true);
    std::vector<float> y(N);
    auto t0=std::chrono::steady_clock::now();
    for(int i=0;i<N;i++) y[i]=e.process(x[i]);
    double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
    bool bad=false; double ymax=0; for(float v:y){ if(!std::isfinite(v)) bad=true; ymax=std::max(ymax,(double)std::fabs(v)); }
    int s=(int)(sr*1.0), L=4096;
    printf("cfg%d: latency=%d F0 in=%.1f out=%.1f centroid in=%.0f out=%.0f peak=%.2f nan=%d cpu=%.1f%% of realtime\n",cfg,e.getLatency(),
       estF0(x,s,L,sr),estF0(y,s+e.getLatency(),L,sr),centroid(x,s,L,sr),centroid(y,s+e.getLatency(),L,sr),ymax,bad,ms/3000.0*100);
  }
}
