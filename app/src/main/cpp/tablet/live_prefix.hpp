#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <utility>

namespace engine::tablet {
// Single writer, immutable published prefix. A page may receive new elements
// only beyond every published count; existing elements never change. The
// immutable directory is replaced only when the reserved page count grows.
// A reader owns its directory and freezes its count, not the writer's vector.
// Budget is an RAII charge whose lifetime extends past actual storage release.
template<class T, class Budget, size_t PageSize=512>
class LivePrefix {
    static_assert(std::is_trivially_copyable_v<T> && PageSize>0);
    struct Page : Budget {
        std::array<T,PageSize> values;
        // Reserve potential late VBO growth for this pinned page as well.
        // This is conservative logical credit, not allocated GPU storage.
        Page():Budget(sizeof(Page)+64+2*PageSize*sizeof(T)) {}
    };
    using PagePtr=std::shared_ptr<Page>;
    static constexpr size_t GroupSize=64;
    struct Group : Budget {
        std::array<PagePtr,GroupSize> pages;
        Group():Budget(sizeof(Group)+64) {}
    };
    using GroupPtr=std::shared_ptr<const Group>;
    struct Directory : Budget {
        std::vector<GroupPtr> groups;
        size_t pageCount=0;
        explicit Directory(size_t n):Budget(sizeof(Directory)+64+n*sizeof(GroupPtr)) {groups.reserve(n);}
        Page& at(size_t index) const noexcept {
            return *groups[index/GroupSize]->pages[index%GroupSize];
        }
    };
    std::shared_ptr<const Directory> directory_;
    size_t count_=0;
public:
    class View {
        friend class LivePrefix;
        std::shared_ptr<const Directory> directory;
        size_t count=0;
        View(std::shared_ptr<const Directory> d,size_t n):directory(std::move(d)),count(n) {}
    public:
        View()=default;
        size_t size() const noexcept {return count;}
        template<class F> void visit(size_t offset,F&& f) const {
            assert(offset<=count);
            while(offset<count) {
                const size_t local=offset%PageSize;
                const size_t n=std::min(count-offset,PageSize-local);
                f(offset,std::span<const T>(directory->at(offset/PageSize).values.data()+local,n));
                offset+=n;
            }
        }
    };
    // Fallible preparation only. Failure leaves the previous directory/count
    // intact. New pages are reserved before the input transaction commits.
    void reserve(size_t extra) {
        if(extra>SIZE_MAX-count_) throw std::length_error("live prefix size");
        const size_t total=count_+extra;
        const size_t needed=total/PageSize+(total%PageSize!=0);
        const size_t old=directory_?directory_->pageCount:0;
        if(needed<=old) return;
        // Copy the small root and at most one partially filled pointer group,
        // never the whole page directory or already published vertex bytes.
        const size_t groups=needed/GroupSize+(needed%GroupSize!=0);
        auto next=std::make_shared<Directory>(groups);
        if(directory_) next->groups.insert(next->groups.end(),directory_->groups.begin(),directory_->groups.end());
        std::shared_ptr<Group> group;
        for(size_t i=old;i<needed;++i) {
            const size_t local=i%GroupSize;
            if(!local) {
                group=std::make_shared<Group>();next->groups.push_back(group);
            } else if(!group) {
                group=std::make_shared<Group>();group->pages=next->groups.back()->pages;
                next->groups.back()=group;
            }
            group->pages[local]=std::make_shared<Page>();
        }
        next->pageCount=needed;
        directory_=std::move(next);
    }
    // Copy only new elements from the existing v1 builder. No allocation or
    // mutation of already published bytes, even if the builder reallocates.
    void extend(std::span<const T> all) noexcept {
        assert(all.size()>=count_);
        assert(all.empty() || (directory_ && (all.size()-1)/PageSize<directory_->pageCount));
        while(count_<all.size()) {
            const size_t local=count_%PageSize;
            const size_t n=std::min(all.size()-count_,PageSize-local);
            std::copy_n(all.data()+count_,n,directory_->at(count_/PageSize).values.data()+local);
            count_+=n;
        }
    }
    View view() const noexcept {return {directory_,count_};}
};
}
