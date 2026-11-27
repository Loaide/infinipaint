// Used only by the host pbuffer executable. All hooks are on its render lane.
#include <chrono>
#include <future>
#include <thread>
#include <windows.h>
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
static long long ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
struct HostFrame {
    int seq{}, rev{};
    double pan{};
    long long intent{}, latch{}, returned{}, draws{}, bytes{};
};
static HostFrame record;
static bool traceOn, park;
static std::promise<void> reached, progressed;
void probeLatch(double pan,long long revision) {
    if(traceOn){record.pan=pan;record.rev=int(revision);record.latch=ns();}
}
void probeUnlocked(){
    if(park){
        park=false;
        reached.set_value();
        check(progressed.get_future().wait_for(3s)==std::future_status::ready,"input blocked behind render");
    }
}
void probeWork(long long draws,long long bytes){
    if(traceOn){record.draws=draws;record.bytes=bytes;}
}
static void camera(double x){Java_com_infinipaint_demo_NativeBridge_setCamera(nullptr,nullptr,x,0,1);}
static void submit(){Java_com_infinipaint_demo_NativeBridge_render(nullptr,nullptr,spline);}
static void scene(int count,bool reverse,bool tiny=false) {
    using namespace engine::document;
    auto d=std::make_unique<Document>(Uuid{uint8_t(count),uint8_t(reverse)});
    for(int i=0;i<count;i++){
        Brush b;b.radiusWorld=tiny?1.f:5.f;
        b.rgba=(bool(i%2)!=reverse)?std::array<float,4>{0,0,1,.25f}:std::array<float,4>{1,0,0,.25f};
        engine::input::RawInputStroke point{{{tiny?405.:40.5,tiny?245.:24.5},1,1,0,0}};
        check(d->restore(i+1,i,{std::make_shared<const Ink>(std::move(point),b)},1,1,1,count)==Status::Ok,"restore");
    }
    check(d->finishRestore(1,count)==Status::Ok && d->grant(count+1,count+65536),"fixture links");
    check(engine::tablet::stage(std::move(d),1<<20),"stage");engine::tablet::activate(0,0,tiny?.1:1.);
    engine::tablet::discard();
}
static int m04Probe(){
    int seq=0;
    Java_com_infinipaint_demo_NativeBridge_suspend(nullptr,nullptr,true);
    for(int n:{2,16,64}){
        for(bool reverse:{false,true}){
            scene(n,reverse);
            const auto image=render(spline); // correctness readback outside timed runs
            const auto p=at(image,40,24);
            double rgb[]={double(image.front()[0]),double(image.front()[1]),double(image.front()[2])};
            for(int i=0;i<n;i++)for(int c=0;c<3;c++)
                rgb[c]=.75*rgb[c]+.25*(c==((bool(i%2)!=reverse)?2:0)?255:0);
            for(int c=0;c<3;c++)check(std::abs(rgb[c]-p[c])<=2,"ordered source-over center");
            check(p[3]==255,"alpha");
            std::printf("pixels n=%d reversed=%d center=%u,%u,%u,%u\n",n,int(reverse),p[0],p[1],p[2],p[3]);
        }
        // Same camera sequence per block. Drain between blocks only, never in hooks.
        for(int block=0;block<4;block++){
            traceOn=(block==1 || block==2);
            for(int i=0;i<8;i++){camera(i%2);submit();}
            glFinish();glCheck("warm");
            std::array<long long,48> elapsed{};
            HostFrame worst{};
            ULONG64 cycles0{},cycles1{};
            check(QueryThreadCycleTime(GetCurrentThread(),&cycles0)!=0,"thread cycles");
            for(int i=0;i<48;i++){
                long long start=ns();
                if(traceOn){record={};record.seq=++seq;record.intent=start;}
                camera(i%2);submit();
                if(traceOn)record.returned=ns();
                elapsed[i]=ns()-start;
                if(traceOn){
                    check(record.pan==i%2 && record.rev==1 && record.latch>=start &&
                          record.returned>=record.latch && record.draws>0 && record.bytes==0,"bad join/work");
                    if(record.returned-record.intent>worst.returned-worst.intent)worst=record;
                }
            }
            check(QueryThreadCycleTime(GetCurrentThread(),&cycles1)!=0,"thread cycles");
            glFinish();glCheck("drain");
            std::sort(elapsed.begin(),elapsed.end());
            std::printf("n=%d block=%d trace=%d wall_us_p50/p95/max=%.2f/%.2f/%.2f",n,block,int(traceOn),
                        elapsed[23]/1e3,elapsed[45]/1e3,elapsed[47]/1e3);
            if(traceOn)std::printf(" worst_seq=%d rev=%d pan=%.0f intent_to_latch_us=%.2f latch_to_return_us=%.2f draws=%lld bytes=%lld",
                       worst.seq,worst.rev,worst.pan,(worst.latch-worst.intent)/1e3,
                       (worst.returned-worst.latch)/1e3,worst.draws,worst.bytes);
            std::printf(" caller_cycles/frame=%llu\n",(cycles1-cycles0)/elapsed.size());
        }
    }
    // Reference integrates the ordered optical result over one pixel. Exactly
    // coincident radius .15 discs fit entirely within it; no supersampling noise.
    for(int n:{1,16}){
        scene(n,false,true);
        const auto image=render(spline);
        const auto p=at(image,40,24), bg=image.front();
        double area=std::acos(-1.)*.15*.15, ref[3]{};
        for(int c=0;c<3;c++){
            double inside=bg[c];
            for(int i=0;i<n;i++)inside=.75*inside+.25*(c==(i%2?2:0)?255:0);
            ref[c]=bg[c]+area*(inside-bg[c]);
        }
        std::printf("tiny n=%d actual=%u,%u,%u area_ref=%.1f,%.1f,%.1f\n",
                    n,p[0],p[1],p[2],ref[0],ref[1],ref[2]);
    }
    Java_com_infinipaint_demo_NativeBridge_suspend(nullptr,nullptr,false);
    camera(0);traceOn=park=true;
    auto ready=reached.get_future();
    std::thread input([&]{
        ready.wait();camera(8);begin(17);sample(17,16,18,1,.6f);end(17);
        progressed.set_value();
    });
    submit();input.join();
    check(record.pan==0 && record.rev==1,"mixed snapshot");
    submit();check(record.pan==8 && record.rev==2,"next frame missed camera or commit");
    glCheck("isolation");
    std::puts("park: input+lift completed before release; consecutive latches pan/rev=0/1,8/2");
    return 0;
}
