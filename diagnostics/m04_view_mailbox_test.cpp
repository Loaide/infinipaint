#include "tablet/view_intent.hpp"
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
using engine::tablet::ViewMailbox;
int main() {
    ViewMailbox mailbox;
    auto initial=mailbox.latch();
    assert(initial.sequence==0 && initial.zoom==1);
    mailbox.viewport(1800,2880);
    auto capture=mailbox.latch();
    std::atomic<bool> done=false;
    std::thread writer([&]{for(int i=1;i<=100000;++i) mailbox.publish(i,-i,2);done=true;});
    uint64_t previous=0;
    do {
        auto snapshot=mailbox.latch();
        assert(snapshot.sequence>=previous);
        assert(snapshot.x==-snapshot.y);
        assert(snapshot.width==1800 && snapshot.height==2880 && snapshot.mappingGeneration==1);
        if(snapshot.sequence>1) assert(snapshot.zoom==2 && snapshot.x==double(snapshot.sequence-1));
        previous=snapshot.sequence;
    } while(!done.load());
    writer.join();
    auto latest=mailbox.latch();
    assert(latest.sequence==100001 && latest.x==100000);
    assert(capture.x==0 && capture.sequence==1 && capture.zoom==1);
    mailbox.viewport(2880,1800);
    auto rotated=mailbox.latch();
    assert(rotated.width==2880 && rotated.height==1800 && rotated.sequence==100002 && rotated.mappingGeneration==2);
    std::cout<<"PASS ViewMailbox: 100000 publications, coherent snapshots, latest-wins, immutable captured value, mapping generation\n";
}
