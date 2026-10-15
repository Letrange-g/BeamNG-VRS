#include "framebuffer_reuse.hpp"
#include <cassert>
#include <iostream>
using namespace beamng_framebuffer;
int main(){
 IdleCache c;Key k{1,2,3,0,100,80,1,{4,5}};
 c.created(10,k);assert(!c.take(k)); // Active/in-flight entries cannot be reused.
 assert(c.release(10));
 auto mismatch=[&](Key other){assert(!c.take(other));};
 auto x=k;x.device++;mismatch(x);x=k;x.pass++;mismatch(x);
 x=k;x.rateView++;mismatch(x);x=k;x.flags++;mismatch(x);
 x=k;x.width++;mismatch(x);x=k;x.height++;mismatch(x);
 x=k;x.layers++;mismatch(x);x=k;std::swap(x.views[0],x.views[1]);mismatch(x);
 assert(c.take(k)==10 && c.idle()==0);assert(!c.take(k));
 // Invalidate a live variant: its owner still destroys it; it cannot be parked.
 assert(c.invalidate([](const Key&){return true;}).empty());assert(!c.release(10));
 c.created(11,k);assert(c.release(11));
 const auto deleted=c.invalidate([](const Key& k){return k.views[0]==4;});
 assert(deleted==std::vector<Id>{11} && !c.take(k));
 // Handle reuse after invalidation cannot recover an old framebuffer.
 c.created(12,k);assert(c.release(12));assert(c.take(k)==12);
 assert(c.release(12));
 for(unsigned i=1;i<IdleCache::capacity;++i){c.created(100+i,k);assert(c.release(100+i));}
 c.created(999,k);assert(!c.release(999));assert(c.idle()==IdleCache::capacity);
 assert(c.invalidate([](const Key& k){return k.device==1;}).size()==IdleCache::capacity);
 assert(c.idle()==0);
 std::cout<<"Framebuffer idle reuse, exact keys, invalidation and capacity passed\n";
}
