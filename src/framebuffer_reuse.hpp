#pragma once
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

// Idle-only reuse: an entry becomes reusable only when the application destroys
// its original framebuffer (and therefore promises that its GPU uses finished).
// The adapter must invalidate entries BEFORE destroying any referenced view or
// render pass. No custom allocators or pNext chains are admitted by the adapter.
namespace beamng_framebuffer {
using Id=uint64_t;
struct Key {
 Id device=0,pass=0,rateView=0;
 uint32_t flags=0,width=0,height=0,layers=0;
 std::vector<Id> views;
 bool operator==(const Key&)const=default;
};
class IdleCache {
 struct Entry { Id framebuffer; Key key; };
 std::unordered_map<Id,Key> active_;
 std::vector<Entry> idle_;
public:
 static constexpr size_t capacity=64;
 uint64_t hits=0,misses=0;
 void created(Id framebuffer,Key key){active_[framebuffer]=std::move(key);}
 Id take(const Key& key){
  auto i=std::find_if(idle_.begin(),idle_.end(),[&](const Entry& e){return e.key==key;});
  if(i==idle_.end()){++misses;return 0;}
  const auto id=i->framebuffer;
  active_[id]=std::move(i->key);idle_.erase(i);++hits;return id;
 }
 // False means the caller must destroy this framebuffer normally.
 bool release(Id framebuffer){
  auto i=active_.find(framebuffer);if(i==active_.end())return false;
  const bool keep=idle_.size()<capacity;
  if(keep)idle_.push_back({framebuffer,std::move(i->second)});
  active_.erase(i);return keep;
 }
 template<class Predicate> std::vector<Id> invalidate(Predicate matches){
  // Active objects remain owned by their original framebuffer. Forget only
  // their reuse eligibility; their normal destruction path will release them.
  for(auto i=active_.begin();i!=active_.end();)if(matches(i->second))i=active_.erase(i);else ++i;
  std::vector<Id> destroy;
  for(auto i=idle_.begin();i!=idle_.end();)if(matches(i->key)){
   destroy.push_back(i->framebuffer);i=idle_.erase(i);
  }else ++i;
  return destroy;
 }
 size_t idle()const{return idle_.size();}
};
}
