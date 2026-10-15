#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

// Sample CPU wall time, including mutex waits, but excluding downstream draw
// execution. This is not thread CPU time or a GPU timestamp. One in 1024 hook
// calls is timed; each category has its own thread-local sampling sequence.
namespace beamng_cpu {
enum Category { Draw, Bind, Update, Pass, Count };
struct Counter { std::atomic<uint64_t> samples{0},ns{0},maxNs{0}; };
inline Counter counters[Count];
inline thread_local uint32_t sequences[Count]{};
class Sample {
 using Clock=std::chrono::steady_clock;
 Category category_;
 bool active_;
 Clock::time_point start_;
public:
 Sample(Category category,bool enabled):category_(category),active_(enabled && ((sequences[category]++ & 1023u)==0)){
  if(active_)start_=Clock::now();
 }
 ~Sample(){
  if(!active_)return;
  auto ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start_).count());
  auto& c=counters[category_];c.ns.fetch_add(ns,std::memory_order_relaxed);
  c.samples.fetch_add(1,std::memory_order_relaxed);
  auto old=c.maxNs.load(std::memory_order_relaxed);
  while(old<ns && !c.maxNs.compare_exchange_weak(old,ns,std::memory_order_relaxed)){}
 }
};
}
