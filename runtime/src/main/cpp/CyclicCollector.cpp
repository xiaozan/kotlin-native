/*
 * Copyright 2010-2020 JetBrains s.r.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef KONAN_NO_THREADS
#define WITH_WORKERS 1
#endif

#include "Alloc.h"
#include "Atomic.h"
#include "KAssert.h"
#include "Memory.h"
#include "Natives.h"
#include "Porting.h"
#include "Types.h"

#if WITH_WORKERS
#include <pthread.h>
#endif

#if WITH_WORKERS

// Define to 1 to print collector traces.
#define TRACE_COLLECTOR 0

#if TRACE_COLLECTOR
#define COLLECTOR_LOG(...) konan::consolePrintf(__VA_ARGS__);
#else
#define COLLECTOR_LOG(...)
#endif

/**
 * Theory of operations:
 *
 * Kotlin/Native runtime has incremental cyclic garbage collection for the shared objects,
 * such as `AtomicReference` and `FreezableAtomicReference` instances (further known as the atomic rootset).
 * We perform such analysis by iterating over the transitive closure of the atomic rootset, and computing
 * aggregated inner reference counter for rootset elements over this transitive closure.
 * Collector runs in its own thread and is started by an explicit request or after certain time interval since last
 * collection passes, thus its operation does not affect UI responsiveness in most cases.
 * Atomic rootset is built by maintaining the linked list of all atomic and freezable atomic references objects.
 * Elements whose transitive closure inner reference count matches the actual reference count are ones
 * belonging to the garbage cycles and thus can be discarded.
 * If during computations of the aggregated RC there were modifications in the reference counts of
 * elements of the atomic rootset:
 *   - if it is being increased, then someone already got an external reference to this element, thus we may not
 *     end up matching the inner reference count anyway
 *   - if it is being decreased and object become garbage, it will be collected next time
 * If transitive closure of the atomic rootset mutates, it could only happen via changing the atomics references,
 * as all elements of this closure are frozen.
 * To handle such mutations we keep collector flag, which is cleared before analysis and set on every
 * atomic reference value update. If flag's value changes - collector restarts its analysis.
 * There are some complications in this algorithm due to delayed reference counting: namely we have to execute
 * callback on each worker which will take into account reference counts coming from the stack references of such
 * a worker.
 * It means, we could perform actual collection only after all registered workers completed rendezvouz which performs
 * such an accounting. As we have to perform such an operation anyway, we also release objects found by the collector
 * on a rendezvouz callback, but not on the main thread, to keep UI responsive.
 */
namespace {

class Locker {
  pthread_mutex_t* lock_;

 public:
  Locker(pthread_mutex_t* alock): lock_(alock) {
    pthread_mutex_lock(lock_);
  }

  ~Locker() {
    pthread_mutex_unlock(lock_);
  }
};

template <typename func>
inline void traverseObjectFields(ObjHeader* obj, func process) {
  RuntimeAssert(obj != nullptr, "Must be non null");
  const TypeInfo* typeInfo = obj->type_info();
  if (typeInfo != theArrayTypeInfo) {
    for (int index = 0; index < typeInfo->objOffsetsCount_; index++) {
      ObjHeader** location = reinterpret_cast<ObjHeader**>(
          reinterpret_cast<uintptr_t>(obj) + typeInfo->objOffsets_[index]);
      process(location);
    }
  } else {
    ArrayHeader* array = obj->array();
    for (int index = 0; index < array->count_; index++) {
      process(ArrayAddressOfElementAt(array, index));
    }
  }
}

#define CHECK_CALL(call, message) RuntimeCheck((call) == 0, message)

class CyclicCollector {
  pthread_mutex_t lock_;
  pthread_cond_t cond_;
  pthread_t gcThread_;

  int currentAliveWorkers_;
  int gcRunning_;
  int mutatedAtomics_;
  int pendingRelease_;
  bool shallCollectGarbage_;
  bool shallRunCollector_;
  bool terminateCollector_;
  int32_t currentTick_;
  int32_t lastTick_;
  int64_t lastTimestampUs_;
  KStdUnorderedMap<ObjHeader*, int> rootsRefCounts_;
  KStdUnorderedSet<void*> alreadySeenWorkers_;
  void* mainWorker_;
  KStdVector<ObjHeader*> rootset_;
  KStdUnorderedSet<ObjHeader*> toRelease_;

 public:
  CyclicCollector() {
    CHECK_CALL(pthread_mutex_init(&lock_, nullptr), "Cannot init collector mutex")
    CHECK_CALL(pthread_cond_init(&cond_, nullptr), "Cannot init collector condition")
    CHECK_CALL(pthread_create(&gcThread_, nullptr, gcWorkerRoutine, this), "Cannot start collector thread")
  }

  ~CyclicCollector() {
    {
      Locker locker(&lock_);
      terminateCollector_ = true;
      shallRunCollector_ = true;
      CHECK_CALL(pthread_cond_signal(&cond_), "Cannot signal collector")
    }
    // TODO: improve waiting for collector termination.
    while (atomicGet(&terminateCollector_)) {   }
    releasePendingUnlocked(nullptr);
    pthread_cond_destroy(&cond_);
    pthread_mutex_destroy(&lock_);
  }

  static void* gcWorkerRoutine(void* argument) {
    CyclicCollector* thiz = reinterpret_cast<CyclicCollector*>(argument);
    thiz->gcProcessor();
    return nullptr;
  }

  static void addAtomicRootCallback(void* argument, ObjHeader* root) {
    CyclicCollector* thiz = reinterpret_cast<CyclicCollector*>(argument);
    thiz->addAtomicRoot(root);
  }

  void addAtomicRoot(ObjHeader* root) {
    // No need to lock changes in the rootset, we invalidate analysis on updates.
    rootset_.push_back(root);
  }

  bool isAtomicReference(ObjHeader* obj) {
    return (obj->type_info()->flags_ & TF_LEAK_DETECTOR_CANDIDATE) != 0 && obj->container()->frozen();
  }

  void gcProcessor() {
     {
       Locker locker(&lock_);
       KStdDeque<ObjHeader*> toVisit;
       KStdUnorderedSet<ObjHeader*> visited;
       while (!terminateCollector_) {
         CHECK_CALL(pthread_cond_wait(&cond_, &lock_), "Cannot wait collector condition")
         if (!shallRunCollector_) continue;
         atomicSet(&gcRunning_, 1);
         alreadySeenWorkers_.clear();
        restart:
         atomicSet(&mutatedAtomics_, 0);
         rootset_.clear();
         visited.clear();
         toVisit.clear();
         rootsRefCounts_.clear();
         GC_AtomicRootsWalk(addAtomicRootCallback, this);
         for (auto* root: rootset_) {
           COLLECTOR_LOG("process %p\n", root);
           RuntimeAssert(toVisit.size() == 0, "Must be clear");
           traverseObjectFields(root, [&toVisit, &visited](ObjHeader** location) {
             ObjHeader* ref = *location;
             if (ref != nullptr && visited.count(ref) == 0) {
               toVisit.push_back(ref);
               COLLECTOR_LOG("adding %p for visiting\n", ref);
             }
           });
           if (atomicGet(&mutatedAtomics_) != 0) {
             COLLECTOR_LOG("restarted during visiting collect")
             goto restart;
           }
           while (toVisit.size() > 0)  {
             auto* obj = toVisit.front();
             toVisit.pop_front();
             COLLECTOR_LOG("visit %p\n", obj);
             if (isAtomicReference(obj)) {
               rootsRefCounts_[obj]++;
             }
             visited.insert(obj);
             if (atomicGet(&mutatedAtomics_) != 0) {
               COLLECTOR_LOG("restarted during rootset visit")
               goto restart;
             }
             traverseObjectFields(obj, [&toVisit, &visited](ObjHeader** location) {
               ObjHeader* ref = *location;
               if (ref != nullptr && visited.count(ref) == 0) {
                 toVisit.push_back(ref);
               }
             });
           }
         }
         for (auto it: rootsRefCounts_) {
           COLLECTOR_LOG("for %p inner %d actual %d\n", it.first, it.second, it.first->container()->refCount());
           // All references are inner. Actually we compare the number of counted
           // inner references minus number of stack references with the number of non-stack references
           // actualized in the object's reference counter.
           if (it.second == it.first->container()->refCount()) {
             COLLECTOR_LOG("adding %p to release candidates\n", it.first);
             toRelease_.insert(it.first);
           }
           if (atomicGet(&mutatedAtomics_) != 0) {
             COLLECTOR_LOG("restarted during rootset analysis")
             toRelease_.clear();
             goto restart;
           }
         }
         if (toRelease_.size() > 0)
           atomicSet(&pendingRelease_, 1);
         atomicSet(&gcRunning_, 0);
         shallRunCollector_ = false;
       }
       terminateCollector_ = false;
     }
  }

  void addWorker(void* worker) {
    Locker lock(&lock_);
    currentAliveWorkers_++;
    if (mainWorker_ == nullptr) mainWorker_ = worker;
  }

  void removeWorker(void* worker) {
    Locker lock(&lock_);
    // When exiting the worker - we shall collect the cyclic garbage here.
    shallCollectGarbage_ = true;
    rendezvouzLocked(worker);
    currentAliveWorkers_--;
  }

  // TODO: this mechanism doesn't allow proper handling of references passed from one stack
  // to another between rendezvouz points.
  void addRoot(ObjHeader* obj) {
    COLLECTOR_LOG("add root %p\n", obj);
    // Actually taking the lock when mutating atomic rootset is the most important part here,
    // setting reference counter is optional.
    Locker lock(&lock_);
    rootsRefCounts_[obj] = 0;
  }

  void removeRoot(ObjHeader* obj) {
    COLLECTOR_LOG("remove root %p\n", obj);
    Locker lock(&lock_);
    rootsRefCounts_.erase(obj);
    toRelease_.erase(obj);
  }

  void mutateRoot(ObjHeader* newValue) {
    // TODO: consider optimization, when clearing value in atomic reference shall not lead
    //   to invalidation of collector analysis state.
    atomicSet(&mutatedAtomics_, 1);
  }

  bool checkIfShallCollect() {
    auto tick = atomicAdd(&currentTick_, 1);
    if (shallCollectGarbage_) return true;
    auto delta = tick - atomicGet(&lastTick_);
    if (delta > 10 || delta < 0) {
      auto currentTimestampUs = konan::getTimeMicros();
      if (currentTimestampUs - atomicGet(&lastTimestampUs_) > 10000) {
        Locker locker(&lock_);
        lastTick_ = currentTick_;
        lastTimestampUs_ = currentTimestampUs;
        shallCollectGarbage_ = true;
        return true;
      }
    }
    return false;
  }

  static void heapCounterCallback(void* argument, ObjHeader* obj) {
    CyclicCollector* collector = reinterpret_cast<CyclicCollector*>(argument);
    collector->countLocked(obj, 1);
  }

  static void stackCounterCallback(void* argument, ObjHeader* obj) {
    CyclicCollector* collector = reinterpret_cast<CyclicCollector*>(argument);
    collector->countLocked(obj, -1);
  }

  void countLocked(ObjHeader* obj, int delta) {
    if (isAtomicReference(obj)) {
      rootsRefCounts_[obj] += delta;
    }
  }

  void rendezvouzLocked(void* worker) {
    if (alreadySeenWorkers_.count(worker) > 0) {
      return;
    }
    GC_StackWalk(stackCounterCallback, this);
    alreadySeenWorkers_.insert(worker);
    if (alreadySeenWorkers_.size() == currentAliveWorkers_) {
       // All workers processed, initiate GC.
       shallRunCollector_ = true;
       CHECK_CALL(pthread_cond_signal(&cond_), "Cannot signal collector")
     }
  }

  void releasePendingUnlocked(void* worker) {
    // We are not doing that on the UI thread, as taking lock is slow, unless
    // it happens on deinit of the collector or if there are no other workers.
    if (atomicGet(&pendingRelease_) != 0 && (worker != mainWorker_ || currentAliveWorkers_ == 1)) {
      Locker locker(&lock_);
      COLLECTOR_LOG("clearing %d release candidates on %p\n", toRelease_.size(), worker);
      for (auto* it: toRelease_) {
        COLLECTOR_LOG("clear references in %p\n", it)
        traverseObjectFields(it, [](ObjHeader** location) {
          ZeroHeapRef(location);
        });
      }
      toRelease_.clear();
      atomicSet(&pendingRelease_, 0);
    }
  }

  void rendezvouz(void* worker) {
    if (atomicGet(&gcRunning_) != 0) return;
    releasePendingUnlocked(worker);
    if (!checkIfShallCollect()) return;
    Locker locker(&lock_);
    rendezvouzLocked(worker);
  }

  void scheduleGarbageCollect() {
    Locker lock(&lock_);
    shallCollectGarbage_ = true;
  }
};

CyclicCollector* cyclicCollector = nullptr;

}  // namespace

#endif  // WITH_WORKERS

void cyclicInit() {
#if WITH_WORKERS
  RuntimeAssert(cyclicCollector == nullptr, "Must be not yet inited");
  cyclicCollector = konanConstructInstance<CyclicCollector>();
#endif
}

void cyclicDeinit() {
#if WITH_WORKERS
  RuntimeAssert(cyclicCollector != nullptr, "Must be inited");
  konanDestructInstance(cyclicCollector);
  cyclicCollector = nullptr;
#endif  // WITH_WORKERS
}

void cyclicAddWorker(void* worker) {
#if WITH_WORKERS
  cyclicCollector->addWorker(worker);
#endif  // WITH_WORKERS
}

void cyclicRemoveWorker(void* worker) {
#if WITH_WORKERS
  cyclicCollector->removeWorker(worker);
#endif  // WITH_WORKERS
}

void cyclicRendezvouz(void* worker) {
#if WITH_WORKERS
  cyclicCollector->rendezvouz(worker);
#endif  // WITH_WORKERS
}

void cyclicScheduleGarbageCollect() {
#if WITH_WORKERS
  cyclicCollector->scheduleGarbageCollect();
#endif  // WITH_WORKERS
}

void cyclicAddAtomicRoot(ObjHeader* obj) {
#if WITH_WORKERS
  if (cyclicCollector)
    cyclicCollector->addRoot(obj);
#endif  // WITH_WORKERS
}

void cyclicRemoveAtomicRoot(ObjHeader* obj) {
#if WITH_WORKERS
  if (cyclicCollector)
    cyclicCollector->removeRoot(obj);
#endif  // WITH_WORKERS
}

void cyclicMutateAtomicRoot(ObjHeader* newValue) {
#if WITH_WORKERS
  if (cyclicCollector)
    cyclicCollector->mutateRoot(newValue);
#endif  // WITH_WORKERS
}