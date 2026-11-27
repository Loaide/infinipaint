// Host-only, included by the existing RGBA8 pbuffer fixture.
#include <chrono>
#include <cmath>
namespace cov {
struct V { double x,y; V operator+(V p)const{return{x+p.x,y+p.y};}
    V operator-(V p)const{return{x-p.x,y-p.y};} V operator*(double k)const{return{x*k,y*k};} };
double dot(V a,V b){return a.x*b.x+a.y*b.y;}
struct Q {V a,b;double r,s;};
using Geometry=std::vector<std::vector<Q>>;
Geometry world;
using Val=std::array<double,5>; // transparent premultiplied RGB, opacity, geometric union
constexpr int X=36,Y=20,W=12,H=10;
using Tile=std::array<Val,W*H>;
struct Sample {double x,y;float p;};
struct Stroke {std::vector<Sample> path; int channel;};
using Scene=std::vector<Stroke>;
constexpr double opacity=.5;
void over(Val& v,int c){
    for(int k=0;k<3;k++)v[k]=v[k]*(1-opacity)+(c==k?opacity:0);
    v[3]=v[3]*(1-opacity)+opacity;v[4]=1;
}
void accum(Val& v,const Val& u,double w){for(int k=0;k<5;k++)v[k]+=w*u[k];}

// Candidate: minimum of the quadratic |p-A-tD|² - (r+t dr)².
// Concave or linear => minimum at an endpoint. Same sites for all IDs.
bool contains(const Q& q,V p){
    V d=q.b-q.a, l=p-q.a; double dr=q.s-q.r;
    double h=dot(d,d)-dr*dr;
    double t=h>0?std::clamp((dot(l,d)+q.r*dr)/h,0.,1.):
        (dot(p-q.b,p-q.b)-q.s*q.s<dot(l,l)-q.r*q.r?1.:0.);
    auto delta=l-d*t;double r=q.r+dr*t;return dot(delta,delta)<=r*r;
}
Tile candidate(const Geometry& g,const Scene& s){
    Tile out{};
    for(int y=0;y<H;y++)for(int x=0;x<W;x++)
        for(int j=0;j<8;j++)for(int i=0;i<8;i++){
            V p{X+x+(i+.5)/8,Y+y+(j+.5)/8};Val site{};
            for(size_t id=0;id<s.size();id++)
                if(std::any_of(g[id].begin(),g[id].end(),[&](auto& q){return contains(q,p);}))
                    over(site,s[id].channel);
            accum(out[y*W+x],site,1./64);
        }
    return out;
}

// Oracle independent of point membership: convex hull of endpoint discs.
// Intersect the two external tangent segments and both circles with a
// horizontal line. Convexity makes the extremal pair the exact interval.
struct I {double lo=1e30,hi=-1e30; void add(double x){lo=std::min(lo,x);hi=std::max(hi,x);}};
I slice(const Q& q,double y){
    I range;
    auto circle=[&](V p,double r){double w=r*r-(y-p.y)*(y-p.y);
        if(w>=0){w=std::sqrt(w);range.add(p.x-w);range.add(p.x+w);}};
    circle(q.a,q.r);circle(q.b,q.s);
    V d=q.b-q.a;double len=std::hypot(d.x,d.y);
    if(len>std::abs(q.s-q.r)){
        V u=d*(1/len),v{-u.y,u.x}; double k=(q.r-q.s)/len;
        for(int sign:{-1,1}){
            V normal=u*k+v*(sign*std::sqrt(1-k*k));
            V a=q.a+normal*q.r,b=q.b+normal*q.s;
            if((y>=a.y)!=(y>=b.y))range.add(a.x+(y-a.y)*(b.x-a.x)/(b.y-a.y));
        }
    }
    return range;
}
Tile reference(const Geometry& g,const Scene& s,int rows){
    Tile out{};
    for(int y=0;y<H;y++)for(int j=0;j<rows;j++){
        std::vector<std::vector<I>> spans(s.size());std::vector<double> cuts;
        for(int x=0;x<=W;x++)cuts.push_back(X+x);
        for(size_t id=0;id<s.size();id++)for(auto& q:g[id]){
            auto i=slice(q,Y+y+(j+.5)/rows);
            i.lo=std::max(i.lo,double(X));i.hi=std::min(i.hi,double(X+W));
            if(i.lo<i.hi){spans[id].push_back(i);cuts.push_back(i.lo);cuts.push_back(i.hi);}
        }
        std::sort(cuts.begin(),cuts.end());
        for(size_t n=1;n<cuts.size();n++){
            double l=cuts[n-1],r=cuts[n];if(r==l)continue;
            double m=(l+r)*.5;Val v{};
            for(size_t id=0;id<s.size();id++)
                if(std::any_of(spans[id].begin(),spans[id].end(),[&](I i){return m>=i.lo && m<=i.hi;}))
                    over(v,s[id].channel);
            accum(out[y*W+std::min(W-1,int(m-X))],v,(r-l)/rows);
        }
    }
    return out;
}
void install(const Scene& strokes,bool white){
    using namespace engine::document;
    auto d=std::make_unique<Document>(Uuid{93});int n=int(strokes.size());
    for(int i=0;i<n;i++){
        Brush b;b.radiusWorld=1;b.rgba={0,0,0,float(opacity)};
        if(white)b.rgba={1,1,1,float(opacity)};else b.rgba[strokes[i].channel]=1;
        engine::input::RawInputStroke path;int tick=0;
        for(auto p:strokes[i].path)path.push_back({{p.x,p.y},1+.05*tick++,p.p,0,0});
        check(d->restore(i+1,i,{std::make_shared<const Ink>(std::move(path),b)},1,1,1,n)==Status::Ok,"restore");
    }
    check(d->finishRestore(1,n)==Status::Ok && d->grant(n+1,n+65536),"restore links");
    world.clear();
    check(engine::tablet::stage(std::move(d),1<<20),"stage fixture");
    engine::tablet::activate(0,0,1);engine::tablet::discard();
    check(world.size()==strokes.size(),"mesh/ID mismatch");
}
double err(double a,double b){return std::abs(a-b);}
} // namespace cov
void captureStroke(){cov::world.emplace_back();}
void captureQuad(float x,float y,float dx,float dy,float len,float r,float s){
    cov::world.back().push_back({{x,y},{x+double(dx)*len,y+double(dy)*len},r,s});
}
static int coverageProbe(){
    using namespace cov;
    Java_com_infinipaint_demo_NativeBridge_suspend(nullptr,nullptr,true);
    std::printf("candidate bytes: tile=%zu site=%zu primitive=%zu; pixel-sites/tile=%d\n",
                sizeof(Tile),sizeof(Val),sizeof(Q),W*H*64);
    Stroke center{{{0,0,1}},0},off{{{1.9,1.3,1}},0},other=center;other.channel=2;
    Stroke apart=other;apart.path[0].x=3.4;
    Stroke thin{{{-18,-5,.2f},{18,5,.2f}},0},taper=thin;taper.path.back().p=1;
    Stroke join{{{-16,-6,.5f},{8,5,1},{-12,4,.4f}},0};
    const std::pair<const char*,Scene> cases[]={
        {"center",{center}},{"off",{off}},{"thin",{thin}},{"taper",{taper}},
        {"coincident",{center,other}},{"reverse",{other,center}},{"separated",{center,apart}},
        {"join",{join}},{"zoom",{join,other}}};
    using Clock=std::chrono::steady_clock;
    for(auto& [name,scene]:cases){
        bool sweep=std::strcmp(name,"zoom")==0;double stats[7]{},step[3]{};
        double candUs=0,v1Us=0;int frames=sweep?33:1;Tile prevC{},prevR{},prevV{};
        for(int f=0;f<frames;f++){
            double z=.1+f*.003125;V origin{40.5,24.5};
            auto view=[&]{Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,-origin.x/z,-origin.y/z,z);};
            install(scene,false);view();auto image=render(spline);auto bg=image.front();
            auto start=Clock::now();
            for(int n=0;n<4;n++)Java_com_infinipaint_demo_NativeBridge_render(nullptr,nullptr,spline);
            glFinish();glCheck("batch");v1Us+=std::chrono::duration<double,std::micro>(Clock::now()-start).count()/4;
            auto geom=world;
            for(auto& stroke:geom)for(auto& q:stroke){
                q.a=origin+q.a*z;q.b=origin+q.b*z;q.r*=z;q.s*=z;
                for(auto p:{q.a,q.b})check(p.x>X+1 && p.x<X+W-1 && p.y>Y+1 && p.y<Y+H-1,"ROI");
            }
            install(scene,true);view();auto white=render(spline);
            start=Clock::now();auto c=candidate(geom,scene);
            candUs+=std::chrono::duration<double,std::micro>(Clock::now()-start).count();
            auto ref=reference(geom,scene,1024),coarse=reference(geom,scene,512);
            Tile v1{};
            for(int h=0;h<W*H;h++){
                int x=X+h%W,y=Y+h/W;auto p=at(image,x,y);
                check(p[3]==255 && at(white,x,y)[3]==255,"opaque v1 target");
                double a=(double(at(white,x,y)[0])-bg[0])/(255-bg[0]);
                v1[h][3]=a;
                for(int k=0;k<5;k++){
                    stats[0]=std::max(stats[0],255*err(ref[h][k],coarse[h][k]));
                    double e=255*err(c[h][k],ref[h][k]);
                    int idx=k<3?1:(k==3?2:3);stats[idx]=std::max(stats[idx],e);
                    if(k<3){
                        double r=ref[h][k]*255+(1-ref[h][3])*bg[k];
                        double b=c[h][k]*255+(1-c[h][3])*bg[k];
                        stats[4]=std::max(stats[4],err(r,p[k]));stats[5]=std::max(stats[5],err(b,p[k]));
                        v1[h][k]=p[k]/255.;
                        if(f){
                            double dc=c[h][k]-prevC[h][k]-(c[h][3]-prevC[h][3])*bg[k]/255.;
                            double dr=ref[h][k]-prevR[h][k]-(ref[h][3]-prevR[h][3])*bg[k]/255.;
                            double dv=v1[h][k]-prevV[h][k];
                            step[0]=std::max(step[0],255*err(dc,dr));step[1]=std::max(step[1],255*err(dv,dr));
                            step[2]=std::max(step[2],255*std::abs(dr));
                        }
                    }
                }
                stats[6]=std::max(stats[6],255*err(ref[h][3],a));
            }
            if(!std::strcmp(name,"center") || !std::strcmp(name,"coincident") ||
               !std::strcmp(name,"reverse") || !std::strcmp(name,"separated")){
                // Both distinct-disc supports fit in this pixel. A marginal
                // coverage product cannot distinguish same vs disjoint supports.
                const double area=std::acos(-1.)*.15*.15;
                const bool two=scene.size()==2, disjoint=!std::strcmp(name,"separated");
                const auto& v=ref[(24-Y)*W+40-X];
                check(err(v[4],area*(disjoint?2:1))<.0001,"disc union integral");
                check(err(v[3],area*(disjoint?1:two?.75:.5))<.0001,"ordered opacity integral");
                if(two){
                    check(err(v[scene.front().channel],area*(disjoint?.5:.25))<.0001 &&
                          err(v[scene.back().channel],area*.5)<.0001,"ordered colors");
                }
            }
            prevC=c;prevR=ref;prevV=v1;
        }
        size_t prim=0;for(auto& st:world)prim+=st.size();
        std::printf("%s n=%d prim=%zu refConv=%.3f C_rgb/a/union=%.3f/%.3f/%.3f V_rgb/a=%.3f/%.3f CvsV_rgb=%.3f stepResidual_C/V=%.3f/%.3f refStep=%.3f us_C/V=%.1f/%.1f\n",
            name,frames,prim,stats[0],stats[1],stats[2],stats[3],stats[4],stats[6],stats[5],
            step[0],step[1],step[2],candUs/frames,v1Us/frames);
        check(stats[0]<.5,"reference convergence > half channel level");
    }
    return 0;
}
