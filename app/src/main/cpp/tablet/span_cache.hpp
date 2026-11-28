#pragma once
#include "tablet/span_store.hpp"
namespace engine::tablet::spans {
// Fixed-size, worker-owned derived cache. Only named durable entries survive
// startup; dirty entries are distinct and never trusted as durable. One
// staging file, atomic rename, no directory walk or allocation on navigation.
class Cache {
    static constexpr size_t slots=256;
    struct Slot {uint64_t bytes=0,age=0;bool durable=false;};
    Stats& stats_;uint64_t limit_,used_=0,clock_=0,staging_=0;
    int dir_=-1;bool temporary_=false;std::array<char,4096> path_{};
    std::array<Slot,slots> entries_{};
    static constexpr const char* stageName="building.tmp";
    static auto name(size_t i,bool durable){std::array<char,24> b{};std::snprintf(b.data(),b.size(),"%s-%03zu.ipv",durable?"saved":"live",i);return b;}
    void usage()noexcept{stats_.bytes=used_+staging_;peak(stats_.peakBytes,used_+staging_);}
    void unlinkName(const char* n){if(::unlinkat(dir_,n,0)&&errno!=ENOENT)throw Failure::Io;}
    void erase(size_t i,bool eviction){auto& e=entries_[i];if(!e.bytes)return;unlinkName(name(i,e.durable).data());used_-=e.bytes;e={};usage();if(eviction)++stats_.evictions;}
    size_t victim(size_t exclude)const noexcept {size_t best=slots;for(size_t i=0;i<slots;++i)if(i!=exclude&&entries_[i].bytes&&(best==slots||entries_[i].age<entries_[best].age))best=i;return best;}
    void reserve(uint64_t bytes,size_t target){if(bytes>limit_)throw Failure::Capacity;while(used_>limit_-bytes){auto i=victim(target);if(i==slots)throw Failure::Capacity;erase(i,true);}staging_=bytes;usage();}
    void abandon()noexcept{if(dir_>=0){if(::unlinkat(dir_,stageName,0)&&errno!=ENOENT){++stats_.io;return;}}staging_=0;usage();}
public:
    struct Ready {File file;Manifest manifest;size_t slot=0;};
    explicit Cache(Stats& stats,uint64_t limit=diskCeiling):stats_(stats),limit_(std::min(limit,diskCeiling)){}
    ~Cache(){close();}Cache(const Cache&)=delete;Cache&operator=(const Cache&)=delete;
    bool init(const char* root,bool temporary=false)noexcept{
        if(dir_>=0)return true;
        try{
            if(!root||!*root)throw Failure::Io;
            const auto n=std::snprintf(path_.data(),path_.size(),"%s/%s",root,temporary?"ip-span-XXXXXX":"ip-spans-v1");if(n<0||size_t(n)>=path_.size())throw Failure::Io;
            if(temporary){if(!::mkdtemp(path_.data()))throw Failure::Io;temporary_=true;}
            else if(::mkdir(path_.data(),0700)&&errno!=EEXIST)throw Failure::Io;
            dir_=::open(path_.data(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dir_<0)throw Failure::Io;
            unlinkName(stageName);entries_.fill({});used_=staging_=0;
            for(size_t i=0;i<slots;++i){unlinkName(name(i,false).data());const auto file=name(i,true);struct stat st{};
                if(::fstatat(dir_,file.data(),&st,AT_SYMLINK_NOFOLLOW)){if(errno==ENOENT)continue;throw Failure::Io;}
                if(!S_ISREG(st.st_mode)||st.st_size<off_t(pageBytes*2)||uint64_t(st.st_size)>limit_||st.st_size%pageBytes){unlinkName(file.data());continue;}
                const auto size=uint64_t(st.st_size);if(used_>limit_-size){unlinkName(file.data());++stats_.evictions;continue;}
                entries_[i]={size,++clock_,true};used_+=size;
            }
            usage();return true;
        }catch(const Failure& f){note(stats_,f);close();return false;}
    }
    void close()noexcept{
        if(dir_>=0){abandon();for(size_t i=0;i<slots;++i)if(entries_[i].bytes&&(!entries_[i].durable||temporary_)){auto n=name(i,entries_[i].durable);(void)::unlinkat(dir_,n.data(),0);}::close(dir_);dir_=-1;}
        if(temporary_){(void)::rmdir(path_.data());temporary_=false;}
        entries_.fill({});used_=staging_=0;usage();
    }
    static size_t slotFor(const Key& key)noexcept {uint64_t h=uint64_t(key.id)*11400714819323198485ull+uint64_t(key.geometry);for(auto b:key.doc)h=(h^b)*1099511628211ull;for(auto b:key.digest)h=(h^b)*1099511628211ull;return size_t((h^(h>>32))%slots);}
    bool ready()const noexcept{return dir_>=0;}
    // No build or replay on lookup. A checksum/source/mesh mismatch cannot
    // become a successful visible cut. Rebuild only from authoritative input.
    std::optional<Ready> lookup(const Key& key,document::Revision durable,uint32_t vertices,const document::wire::Header& header,uint32_t samples){
        if(dir_<0)return {};
        const auto i=slotFor(key);auto& e=entries_[i];if(!e.bytes){++stats_.misses;return {};}
        try{
            const auto fileName=name(i,e.durable);File f(::openat(dir_,fileName.data(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW));if(!f)throw Failure::Io;
            auto m=readManifest(read(f,0,stats_));
            if(!(m.key==key)||m.root.vertices!=vertices||m.durable>durable){++stats_.misses;return {};}
            demand(m.canonical==header&&m.samples==samples&&bool(m.durable)==e.durable&&f.size()==e.bytes&&e.bytes==uint64_t(m.pages)*pageBytes);
            e.age=++clock_;++stats_.hits;return Ready{std::move(f),m,i};
        }catch(const Failure& f){note(stats_,f);++stats_.misses;try{erase(i,false);}catch(const Failure& x){note(stats_,x);}return {};}
    }
    template<class Stop,class Match> std::optional<Ready> create(const Key& key,document::Revision durable,const document::Ink& ink,double dx,double dy,uint32_t vertices,Stop& stop,Match& match){
        if(dir_<0)return {};
        const auto start=Clock::now();const auto i=slotFor(key);bool committed=false;
        try{
            stopped(stop());if(staging_)throw Failure::Io;erase(i,true);unlinkName(stageName);
            struct statvfs v{};if(::fstatvfs(dir_,&v)||!v.f_frsize)throw Failure::Io;
            constexpr uint64_t keepFree=32ull<<20;const uint64_t available=uint64_t(v.f_bavail)>UINT64_MAX/v.f_frsize?UINT64_MAX:uint64_t(v.f_bavail)*v.f_frsize;
            auto admit=[&](uint64_t bytes){if(available<keepFree||bytes>available-keepFree)throw Failure::Capacity;reserve(bytes,i);};
            File f(::openat(dir_,stageName,O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));if(!f)throw Failure::Io;
            auto m=build(f,key,durable,ink,dx,dy,vertices,stats_,stop,match,admit);
            stopped(stop());demand(f.size()==staging_&&staging_==uint64_t(m.pages)*pageBytes);
            if(durable)f.sync();
            stopped(stop());
            const auto dest=name(i,durable!=0);if(::renameat(dir_,stageName,dir_,dest.data()))throw Failure::Io;
            entries_[i]={staging_,++clock_,durable!=0};used_+=staging_;staging_=0;usage();committed=true;
            // A crash may lose a disposable cache entry, never source data.
            if(durable&&::fsync(dir_))throw Failure::Io;
            ++stats_.builds;const auto ns=elapsed(start);stats_.buildNs+=ns;peak(stats_.buildMaxNs,ns);
            return Ready{std::move(f),m,i};
        }catch(const Failure& f){note(stats_,f);if(f==Failure::Cancelled)++stats_.buildCancelled;else ++stats_.buildFailures;}
        catch(...){++stats_.buildFailures;++stats_.invalid;}
        if(committed){try{erase(i,false);}catch(const Failure& f){note(stats_,f);}}
        abandon();const auto ns=elapsed(start);stats_.buildNs+=ns;peak(stats_.buildMaxNs,ns);return {};
    }
    // Promotion is optional. It is allowed only after the actual source
    // acknowledgement and with the same fully validated source/mesh identity.
    // Failure drops the derived entry rather than weakening the protocol.
    bool promote(Ready& r,document::Revision durable){
        if(!durable||r.manifest.durable||durable<r.manifest.key.geometry)return true;
        try{
            const auto from=name(r.slot,false),to=name(r.slot,true);File f(::openat(dir_,from.data(),O_RDWR|O_CLOEXEC|O_NOFOLLOW));if(!f)throw Failure::Io;
            auto m=readManifest(read(f,0,stats_));demand(m.key==r.manifest.key&&m.durable==0&&f.size()==entries_[r.slot].bytes);
            m.durable=durable;auto p=manifestPage(m);write(f,0,p,stats_);f.sync();if(::renameat(dir_,from.data(),dir_,to.data()))throw Failure::Io;
            entries_[r.slot].durable=true;r.manifest=m;if(::fsync(dir_))throw Failure::Io;++stats_.promotions;return true;
        }catch(const Failure& f){note(stats_,f);invalidate(r.slot);return false;}
    }
    void invalidate(size_t slot)noexcept{if(slot>=slots)return;try{erase(slot,false);}catch(const Failure& f){note(stats_,f);}}
};
}
