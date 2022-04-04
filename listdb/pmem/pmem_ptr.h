#ifndef LISTDB_PMEM_PMEM_PTR_H_
#define LISTDB_PMEM_PMEM_PTR_H_

#include "listdb/pmem/pmem.h"

class PmemPtr {
 public:
  PmemPtr() : data_(0) { }

  PmemPtr(int16_t pool_id, uint64_t offset);

  PmemPtr(int16_t pool_id, char* vaddr);

  PmemPtr(uint64_t dump);

  void* get();

  template <typename T>
  T* get();
  
  uint64_t dump();

  int16_t pool_id();

  uint64_t offset();

  static uint64_t Encode(const int16_t pool_id, const uint64_t offset);

 private:
  uint64_t data_;
};

PmemPtr::PmemPtr(const int16_t pool_id, const uint64_t offset) : data_(Encode(pool_id, offset)) { }

PmemPtr::PmemPtr(int16_t pool_id, char* vaddr) {
  uint64_t offset = (uintptr_t) vaddr - (uintptr_t) Pmem::pool(pool_id).handle();
  data_ = Encode(pool_id, offset);
}

PmemPtr::PmemPtr(const uint64_t dump) : data_(dump) { }

inline void* PmemPtr::get() {
  if (data_ == 0) {
    return nullptr;
  }
  //int *p2 = (int *)(((uintptr_t)p1 & ((1ull << 48) - 1)) |
  //  ~(((uintptr_t)p1 & (1ull << 47)) - 1));
  static const uintptr_t kMask = 0x0000ffffffffffff;
  const int16_t pool_id = (data_ >> 48);
  const uint64_t offset = (data_ & kMask);
  return (void*) ((uintptr_t) Pmem::pool(pool_id).handle() + offset);
}

template <typename T>
inline T* PmemPtr::get() {
  return (T*) get();
}

inline uint64_t PmemPtr::dump() {
  return data_;
}

inline int16_t PmemPtr::pool_id() {
  int16_t pool_id = (data_ >> 48);
  return pool_id;
}

inline uint64_t PmemPtr::offset() {
  static const uintptr_t kMask = 0x0000ffffffffffff;
  const uint64_t offset = (data_ & kMask);
  return offset;
}

inline uint64_t PmemPtr::Encode(const int16_t pool_id, const uint64_t offset) {
  return ((uint64_t) pool_id << 48) | offset;
}

#endif  // LISTDB_PMEM_PMEM_PTR_H_