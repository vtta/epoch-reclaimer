# Simple Epoch Based Memory Reclaimer

[![Build Status](https://dev.azure.com/haoxiangpeng/epoch-reclaimer/_apis/build/status/XiangpengHao.epoch-reclaimer?branchName=master)](https://dev.azure.com/haoxiangpeng/epoch-reclaimer/_build/latest?definitionId=1&branchName=master)

Code adapted from [PMwCAS](https://github.com/microsoft/pmwcas) with a few new features, all bugs are mine.

## Features

- Small, simple, feature-compelete

- Epoch Manager + Garbage List

- Header-only

- Experimental persistent memory support

## Build

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE={Debug|Release} -DPMEM={0|1} ..
make tests
```

## Usage

```c++
struct MockItem {
 public:
  MockItem() {}

  static void Destroy(void* destroyContext, void* p) {
    // some complex memory cleanup
  }

  /*
   * Usage one: manual epoch management
   * */ 
  void SolveNP(){
    epoch_manager_->Protect();

    // some algorithms to solve SAT

    epoch_manager_->Unprotect(); 
  }


  /*
   * Usage two: use epoch guard to automatically protect and unprotect
   * Makes it easy to ensure epoch protection boundaries tightly adhere to stack life
   * time even with complex control flow.
   * */
  void SolveP(){
    EpochGuard guard();

    // Some algorithms
  }
};


EpochManager epoch_manager_;
epoch_manager_.Initialize()

GarbageList garbage_list_;
// 1024 is the number of addresses that can be held aside for pointer stability.
garbage_list_.Initialize(&epoch_manager_, 1024);

// MockItem::Destory is the deallocation callback
garbage_list_.Push(new MockItem(), MockItem::Destroy, nullptr);
garbage_list_.Push(new MockItem(), MockItem::Destroy, nullptr);
```


## Persistent Memory support

We require [PMDK](https://pmem.io/pmdk/) to support safe and efficient persistent memory operations.

### Recovery

```c++
pool_ = pmemobj_open(pool_name, layout_name, pool_size, CREATE_MODE_RW);

// to create a new garbage list
garbage_list_.Initialize(&epoch_manager_, pool_, 1024);

// to recover an existing garbage list
garbage_list_.Recovery(&epoch_manager_, pool_);
```

### Reserve Memory

Some persistent memory allocator, e.g. PMDK's, requires applications to pass a pre-existing memory location to store the pointer to the allocated memory.

For example:
```c++
void* mem = nullptr;
posix_memalign(&mem, CACHELINE_SIZE, size);
```
It's not safe to store the `mem` on the stack, thus requires the application to mantain an `allocation list`, 
or significantly change the implementation, so there is this function:

```c++
void* mem = garbage_list_.ReserveMemory();
posix_memalign(&mem, CACHELINE_SIZE, size);
```

After the `mem` is hand back to the data structure, the reserved memory slot should be cleared, otherwise it will be reclaimed on recovery:

```c++
garbage_list_.ResetItem(mem);
```

#### Caveat
Although both `ReserveMemory` and `ResetItem` is crash/thread safe, they are typically protected under a transaction,
 because these functions implicitly implied ownership transfer which requires multi-cache line operations.

