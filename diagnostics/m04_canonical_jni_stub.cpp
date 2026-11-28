// Unused session calls only: link real StorePort range JNI on a host JVM.
// Does not emulate Android SQLite, GL or the document engine.
#include "tablet/session.hpp"
namespace engine::tablet {
void setPhysicalSlice(size_t) {}
bool admitIncoming(size_t,size_t){return false;}
bool stage(std::unique_ptr<document::Document>,size_t){return false;}
void activate(double,double,double) noexcept {}
void discard() noexcept {}
std::shared_ptr<const document::ChangeSet> nextSave(){return {};}
bool acknowledge(const std::shared_ptr<const document::ChangeSet>&){return false;}
}
