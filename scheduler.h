#include <omp.h>
#include <chrono>
#include <thread>
#include <cstdint>
#include <iostream>

// EXAMPLE USE:
//
// fork_join_scheduler fj;
//
// long fib(long i) {
//   if (i <= 1) return 1;
//   long l,r;
//   fj.pardo([&] () { l = fib(i-1);},
//            [&] () { r = fib(i-2);});
//   return l + r;
// }
//
// fj.start([] () { cout << fib(40) << endl;});

using namespace std;

// Deque from Arora, Blumofe, and Plaxton (SPAA, 1998).
template <typename Job>
struct Deque {
  using qidx = unsigned int;
  using tag_t = unsigned int;

  // use unit for atomic access
  union age_t {
    struct {
      tag_t tag;
      qidx top;
    } pair;
    size_t unit;
  };

  // align to avoid false sharing
  struct alignas(64) padded_job { Job* job;  };

  static int const q_size = 200;
  age_t age;
  qidx bot;
  padded_job deq[q_size];

  inline bool cas(size_t* ptr, size_t oldv, size_t newv) {
    return __sync_bool_compare_and_swap(ptr, oldv, newv);
  }

  inline void fence() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }

  Deque() : bot(0) {
    age.pair.tag = 0;
    age.pair.top = 0;
  }
    
  void push_bottom(Job* job) {
    qidx local_bot;
    local_bot = bot; // atomic load
    deq[local_bot].job = job; // shared store
    local_bot += 1;
    if (local_bot == q_size) {
      cout << "internal error: scheduler queue overflow" << endl;
      abort();
    }
    bot = local_bot; // shared store
    fence();
  }
  
  Job* pop_top() {
    age_t old_age, new_age;
    qidx local_bot;
    Job *job, *result;
    old_age.unit = age.unit; // atomic load

    local_bot = bot; // atomic load
    if (local_bot <= old_age.pair.top)
      result = NULL;
    else {
      job = deq[old_age.pair.top].job; // atomic load
      new_age.unit = old_age.unit;
      new_age.pair.top = new_age.pair.top + 1;
      if (cas(&(age.unit), old_age.unit, new_age.unit))  // cas
	result = job;
      else
	result = NULL;
    }
    return result;
  }

  Job* pop_bottom() {
    age_t old_age, new_age;
    qidx local_bot;
    Job *job, *result;
    local_bot = bot; // atomic load
    if (local_bot == 0) 
      result = NULL;
    else {
      local_bot = local_bot - 1;
      bot = local_bot; // shared store
      fence();
      job = deq[local_bot].job; // atomic load
      old_age.unit = age.unit; // atomic load
      if (local_bot > old_age.pair.top)
	result = job;
      else {
	bot = 0; // shared store
	new_age.pair.top = 0;
	new_age.pair.tag = old_age.pair.tag + 1;
	if ((local_bot == old_age.pair.top) &&
	    cas(&(age.unit), old_age.unit, new_age.unit))
	  result = job;
	else {
	  age.unit = new_age.unit; // shared store
	  result = NULL;
	}
	fence();
      }
    }
  return result;
  }

};

template <typename Job>
struct scheduler {

public:
  // see comments under wait(..)
  static bool const conservative = false;

  scheduler() {
    num_deques = 2*num_workers();
    deques = new Deque<Job>[num_deques];
    attempts = new attempt[num_deques];
    finished_flag = 0;
  }

  ~scheduler() {
    delete[] deques;
    delete[] attempts;
  }

  // Run the job on specified number of threads.
  void run(Job* job, int num_threads = 0) {
    deques[0].push_bottom(job);
    auto finished = [&] () {return finished_flag > 0;};
    if (num_threads > 0 && num_threads < 2 * num_workers())
      omp_set_num_threads(num_threads);
    #pragma omp parallel
    start(finished);

    finished_flag = 0;
  }

  // Push onto local stack.
  void spawn(Job* job) {
    int id = worker_id();
    deques[id].push_bottom(job);
  }

  // Wait for condition: finished().
  template <typename F>
  void wait(F finished) {
    // Conservative avoids deadlock if scheduler is used in conjunction 
    // with user locks enclosing a wait.
    if (conservative) 
      while (!finished()) 
	std::this_thread::yield();
    // If not conservative, schedule within the wait.
    // Can deadlock if a stolen job uses same lock as encloses the wait.
    else start(finished);
  }

  // all scheduler threads quit after this is called.
  void finish() {finished_flag = 1;}

  // pop from local stack
  Job* try_pop() {
    int id = worker_id();
    return deques[id].pop_bottom();
  }

private:

  // align to avoid false sharing
  struct alignas(128) attempt { size_t val; };
  
  int num_deques;
  Deque<Job>* deques;
  attempt* attempts;
  int finished_flag;

  // Start an individual scheduler task.  Runs until finished().
  template <typename F>
  void start(F finished) {
    while (1) {
      Job* job = get_job(finished);
      if (!job) return;
      (*job)();
    }
  }

  Job* try_steal(size_t id) {
    // use hashing to get "random" target
    size_t target = (hash(id) + hash(attempts[id].val)) % num_deques;
    attempts[id].val++;
    return deques[target].pop_top();
  }

  // Find a job, first trying local stack, then random steals.
  template <typename F>
  Job* get_job(F finished) {
    if (finished()) return NULL;
    Job* job = try_pop();
    if (job) return job;
    size_t id = worker_id();
    while (1) {
      // By coupon collector's problem, this should touch all.
      for (int i=0; i <= num_deques * 16; i++) {
	if (finished()) return NULL;
	job = try_steal(id);
	if (job) return job;
      }
      // If havn't found anything, take a breather.
      std::this_thread::sleep_for(std::chrono::nanoseconds(num_deques*100));
    }
  }

  uint64_t hash(uint64_t x) {
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    x = x ^ (x >> 31);
    return x;
  }

  static int num_workers() { return omp_get_max_threads(); }
  static int worker_id() { return omp_get_thread_num(); }
  void set_num_workers(int n) { omp_set_num_threads(n); }

};

struct fork_join_scheduler {
  // Jobs are thunks -- i.e., functions that take no arguments
  // and return nothing.   Could be a lambda, e.g. [] () {}.
  using Job = std::function<void()>;
  
  scheduler<Job>* sched;
  
  fork_join_scheduler() {
    sched = new scheduler<Job>;
  }

  ~fork_join_scheduler() {
    delete sched;
  }

  // Run thunk on given number of threads.
  // 0 means however many the hardware claims it has.
  template <typename J>
  void run(J thunk, int num_threads=0) {
    Job job = [&] () {thunk(); sched->finish();};
    sched->run(&job,num_threads);
  }

  // Fork two thunks and wait until they both finish.
  template <typename L, typename R>
  void pardo(L left, R right) {
    bool right_done = false;
    bool stolen = false;
    Job right_job = [&] () {
      stolen = true; right(); right_done = true;};
    sched->spawn(&right_job);
    left();
    if (!stolen) {
      Job* job = sched->try_pop();
      if (job != &right_job) {
	if (job != NULL) sched->spawn(job);
      } else { right(); return;}
    }
    auto finished = [&] () {return right_done;};
    sched->wait(finished);
  }
  
};
