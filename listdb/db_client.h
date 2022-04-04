#ifndef LISTDB_DB_CLIENT_H_
#define LISTDB_DB_CLIENT_H_

#include <algorithm>
#include <vector>

#include "listdb/common.h"
#include "listdb/listdb.h"
#include "listdb/util.h"
#include "listdb/lib/random.h"

#define LEVEL_CHECK_PERIOD_FACTOR 1

//#define LOG_NTSTORE
class DBClient {
 public:
  using MemNode = ListDB::MemNode;
  using PmemNode = ListDB::PmemNode;

  DBClient(ListDB* db, int id, int region);

  void SetRegion(int region);

  void Put(const Key& key, const Value& value);

  bool Get(const Key& key, Value* value_out);
  
  //void ReserveLatencyHistory(size_t size);
  
  size_t pmem_get_cnt() { return pmem_get_cnt_; }
  size_t search_visit_cnt() { return search_visit_cnt_; }
  size_t height_visit_cnt(int h) { return height_visit_cnt_[h]; }
  

 private:
  int RandomHeight();

  static int KeyShard(const Key& key);

  PmemPtr LevelLookup(const Key& key, const int pool_id, const int level, BraidedPmemSkipList* skiplist);
  PmemPtr Lookup(const Key& key, const int pool_id, BraidedPmemSkipList* skiplist);
  PmemPtr LookupL1(const Key& key, const int pool_id, BraidedPmemSkipList* skiplist, const int shard);

  ListDB* db_;
  int id_;
  int region_;
  Random rnd_;
  PmemLog* log_[kNumShards];
  //BraidedPmemSkipList* bsl_[kNumShards];
  size_t pmem_get_cnt_ = 0;
  size_t search_visit_cnt_ = 0;
  size_t height_visit_cnt_[kMaxHeight] = {};

#ifdef GROUP_LOGGING
  struct LogItem {
    Key key;
    uint64_t tag;
    Value value;
    MemNode* mem_node;
    //uint64_t offset;
  };
  std::vector<LogItem> log_group_[kNumShards];
  size_t log_group_alloc_size_[kNumShards];
#endif

  //std::vector<std::chrono::duration<double>> latencies_;
};

DBClient::DBClient(ListDB* db, int id, int region) : db_(db), id_(id), region_(region), rnd_(id) {
  for (int i = 0; i < kNumShards; i++) {
    log_[i] = db_->log(region_, i);
  }
}

void DBClient::SetRegion(int region) {
  region_ = region;
  for (int i = 0; i < kNumShards; i++) {
    log_[i] = db_->log(region_, i);
  }
}

void DBClient::Put(const Key& key, const Value& value) {
#ifndef GROUP_LOGGING
  int s = KeyShard(key);

  uint64_t height = RandomHeight();
  size_t iul_entry_size = sizeof(PmemNode) + (height - 1) * sizeof(uint64_t);
  size_t kv_size = key.size() + sizeof(Value);

  // Write log
  auto log_paddr = log_[s]->Allocate(iul_entry_size);
  PmemNode* iul_entry = (PmemNode*) log_paddr.get();
#ifdef LOG_NTSTORE
  _mm_stream_pi((__m64*) &iul_entry->tag, (__m64) height);
  _mm_stream_pi((__m64*) &iul_entry->value, (__m64) value);
  //_mm_sfence();
  _mm_stream_pi((__m64*) &iul_entry->key, (__m64) (uint64_t) key);
#else
  iul_entry->tag = height;
  iul_entry->value = value;
  //clwb(&iul_entry->tag, 16);
  _mm_sfence();
  iul_entry->key = key;
  //clwb(iul_entry, 8);
  clwb(iul_entry, sizeof(PmemNode) - sizeof(uint64_t));
#endif

  // Create skiplist node
  MemNode* node = (MemNode*) malloc(sizeof(MemNode) + (height - 1) * sizeof(uint64_t));
  node->key = key;
  node->tag = height;
  //node->value = value;
  node->value = log_paddr.dump();
  memset((void*) &node->next[0], 0, height * sizeof(uint64_t));

  auto mem = db_->GetWritableMemTable(kv_size, s);
  auto skiplist = mem->skiplist();
  skiplist->Insert(node);
  mem->w_UnRef();
#else
  int s = KeyShard(key);

  uint64_t height = RandomHeight();

  size_t kv_size = key.size() + sizeof(Value);


  // Create skiplist node
  MemNode* node = (MemNode*) malloc(sizeof(MemNode) + (height - 1) * sizeof(uint64_t));
  node->key = key;
  node->tag = height;
  //node->value = value;
  node->value = 0;
  memset(&node->next[0], 0, height * sizeof(uint64_t));

  auto mem = db_->GetWritableMemTable(kv_size, s);
  auto skiplist = mem->skiplist();

  size_t iul_entry_size = sizeof(PmemNode) + (height - 1) * sizeof(uint64_t);
  log_group_[s].emplace_back(LogItem{ key, height, value, node });
  log_group_alloc_size_[s] += iul_entry_size;
  if (log_group_[s].size() > 7) {
    int group_size = log_group_[s].size();
    //size_t log_space = 0;
    //std::vector<size_t> offset;
    //for (int i = 0; i < group_size; i++) {
    //  size_t iul_entry_size = sizeof(PmemNode) + (log_group_[s][i].tag - 1) * sizeof(uint64_t);
    //  offset.push_back(log_space);
    //  log_space += iul_entry_size;
    //}
    // Write log
    auto log_paddr = log_[s]->Allocate(log_group_alloc_size_[s]);
    char* p = (char*) log_paddr.get();
    auto pool_id = log_paddr.pool_id();
    auto pool_offset = log_paddr.offset();

    for (int i = 0; i < group_size; i++) {
      PmemNode* iul_entry = (PmemNode*) (p);
      iul_entry->key = log_group_[s][i].key;
      iul_entry->tag = log_group_[s][i].tag;
      iul_entry->value = log_group_[s][i].value;
      log_group_[s][i].mem_node->value = PmemPtr(pool_id, p).dump();
      p += sizeof(PmemNode) + (log_group_[s][i].tag - 1) * 8;
    }

    clwb(log_paddr.get(), log_group_alloc_size_[s]);
    log_group_[s].clear();
    log_group_alloc_size_[s] = 0;
  }

  skiplist->Insert(node);
  mem->w_UnRef();
#endif

  //size_t log_alloc_size = util::AlignedSize(8, LogWriter::Entry::ComputeAllocSize(key, height));
  //size_t node_alloc_size = util::AlignedSize(8, MemNode::ComputeAllocSize(key, height));

  //put_group_[s].emplace_back(key, value, height, log_alloc_size, node_alloc_size);
  //group_state_[s].kv_size += key.size() + /* value.size() */ 8;
  //group_state_[s].log_alloc_size += log_alloc_size;
  //group_state_[s].node_alloc_size += node_alloc_size;
  //if (group_state_[s].log_alloc_size >= 1024) {
  //  ProcessPutGroup(s);
  //}
}

bool DBClient::Get(const Key& key, Value* value_out) {
  int s = KeyShard(key);
#if 1
  {
    MemTableList* tl = (MemTableList*) db_->GetTableList(0, s);

    auto table = tl->GetFront();
    while (table) {
      if (table->type() == TableType::kMemTable) {
        auto mem = (MemTable*) table;
        auto skiplist = mem->skiplist();
        auto found = skiplist->Lookup(key);
        if (found && found->key == key) {
          *value_out = found->value;
          return true;
        }
      } else if (table->type() == TableType::kPmemTable) {
        break;
      }
      table = table->Next();
    }
#ifdef LOOKUP_CACHE
    {
      auto ht = db_->GetHashTable(s);
      if (ht->Get(key, value_out)) {
        return true;
      }
    }
#endif
    pmem_get_cnt_++;
    while (table) {
      auto pmem = (PmemTable*) table;
      auto skiplist = pmem->skiplist();
      //auto found_paddr = skiplist->Lookup(key, region_);
      auto found_paddr = Lookup(key, region_, skiplist);
      ListDB::PmemNode* found = (ListDB::PmemNode*) found_paddr.get();
      if (found && found->key == key) {
        //fprintf(stdout, "found on pmem\n");
        *value_out = found->value;
        return true;
      }
      table = table->Next();
    }
  }
  {
    // Level 1 Lookup
    auto tl = (PmemTableList*) db_->GetTableList(1, s);
    auto table = tl->GetFront();
    while (table) {
      auto pmem = (PmemTable*) table;
      auto skiplist = pmem->skiplist();
      //auto found_paddr = skiplist->Lookup(key, region_);
      auto found_paddr = LookupL1(key, region_, skiplist, s);
      ListDB::PmemNode* found = (ListDB::PmemNode*) found_paddr.get();
      if (found && found->key == key) {
        //fprintf(stdout, "found on pmem\n");
        *value_out = found->value;
        return true;
      }
      table = table->Next();
    }
  }
#else
  if (1) {
    if (bsl_[s] == nullptr) {
      auto tl = (PmemTableList*) db_->GetTableList(1, s);
      auto table = tl->GetFront();
      auto pmem = (PmemTable*) table;
      auto skiplist = pmem->skiplist();
      bsl_[s] = skiplist;
    }
    bsl_[s]->Lookup(key, region_ + 2);
    return true;
  }
#endif
  return false;
}

inline int DBClient::RandomHeight() {
  static const unsigned int kBranching = 2;
  int height = 1;
#if 1
  if (rnd_.Next() % std::max<int>(1, (kBranching / kNumRegions)) == 0) {
    height++;
    while (height < kMaxHeight && ((rnd_.Next() % kBranching) == 0)) {
      height++;
    }
  }
#else
  while (height < kMaxHeight && ((rnd_.Next() % kBranching) == 0)) {
    height++;
  }
#endif
  return height;
}

inline int DBClient::KeyShard(const Key& key) {
  return key.key_num() % kNumShards;
  //return key.key_num() / kShardSize;
}

PmemPtr DBClient::LevelLookup(const Key& key, const int pool_id, const int level, BraidedPmemSkipList* skiplist) {
  using Node = PmemNode;
  Node* pred = skiplist->head(pool_id);
  uint64_t curr_paddr_dump;
  Node* curr;
  int height = pred->height();

  // NUMA-local upper layers
  for (int i = height - 1; i >= 1; i--) {
    while (true) {
      curr_paddr_dump = pred->next[i];
      curr = (Node*) ((PmemPtr*) &curr_paddr_dump)->get();
      if (curr) {
        if (rnd_.Next() % LEVEL_CHECK_PERIOD_FACTOR == 0) {
          int curr_level = (curr->tag & 0xf00) >> 8;
          if (curr_level > level) {
            fprintf(stdout, "Level 1 detected. Skip to L1 Search.");
            return 0;  // PmemPtr(0).get() == nullptr
          }
        }
        if (curr->key.Compare(key) < 0) {
          pred = curr;
          continue;
        }
      }
      break;
    }
  }

  // Braided bottom layer
  if (pred == skiplist->head(pool_id)) {
    pred = skiplist->head(0);
  }
  while (true) {
    curr_paddr_dump = pred->next[0];
    curr = (Node*) ((PmemPtr*) &curr_paddr_dump)->get();
    if (curr) {
      if (rnd_.Next() % LEVEL_CHECK_PERIOD_FACTOR == 0) {
        int curr_level = (curr->tag & 0xf00) >> 8;
        if (curr_level > level) {
          fprintf(stdout, "Level 1 detected. Skip to L1 Search.");
          return 0;  // PmemPtr(0).get() == nullptr
        }
      }
      if (curr->key.Compare(key) < 0) {
        pred = curr;
        continue;
      }
    }
    //fprintf(stdout, "lookupkey=%zu, curr->key=%zu\n", key, curr->key);
    break;
  }
  return curr_paddr_dump;
}

PmemPtr DBClient::Lookup(const Key& key, const int pool_id, BraidedPmemSkipList* skiplist) {
  using Node = PmemNode;
  Node* pred = skiplist->head(pool_id);
  search_visit_cnt_++;
  height_visit_cnt_[kMaxHeight - 1]++;
  uint64_t curr_paddr_dump;
  Node* curr;
  int height = pred->height();

  // NUMA-local upper layers
  for (int i = height - 1; i >= 1; i--) {
    while (true) {
      curr_paddr_dump = pred->next[i];
      curr = (Node*) ((PmemPtr*) &curr_paddr_dump)->get();
      if (curr) {
        search_visit_cnt_++;
        height_visit_cnt_[i]++;
        if (curr->key.Compare(key) < 0) {
          pred = curr;
          continue;
        }
      }
      break;
    }
  }

  // Braided bottom layer
  if (pred == skiplist->head(pool_id)) {
    if (pool_id != 0) {
      search_visit_cnt_++;
      height_visit_cnt_[kMaxHeight - 1]++;
    }
    pred = skiplist->head(0);
  }
  while (true) {
    curr_paddr_dump = pred->next[0];
    curr = (Node*) ((PmemPtr*) &curr_paddr_dump)->get();
    if (curr) {
      search_visit_cnt_++;
      height_visit_cnt_[0]++;
      if (curr->key.Compare(key) < 0) {
        pred = curr;
        continue;
      }
    }
    //fprintf(stdout, "lookupkey=%zu, curr->key=%zu\n", key, curr->key);
    break;
  }
  return curr_paddr_dump;
}

PmemPtr DBClient::LookupL1(const Key& key, const int pool_id, BraidedPmemSkipList* skiplist, const int shard) {
  using Node = PmemNode;
  Node* pred = skiplist->head(pool_id);
  uint64_t curr_paddr_dump;
  Node* curr;
  int height = pred->height();

  if (0) {
    using MyType1 = std::pair<Key, uint64_t>;
    MyType1 search_key(key, 0);
    auto&& sorted_arr = db_->sorted_arr(pool_id, shard);
    auto found = std::upper_bound(sorted_arr.begin(),
        sorted_arr.end(), search_key,
        [&](const MyType1 &a, const MyType1 &b) { return a.first > b.first; });
    if (found != sorted_arr.end()) {
      //fprintf(stdout, "lookup key: %zu, found dram copy: %zu\n", key, found->first);
      pred = (Node*) ((PmemPtr*) &((*found).second))->get();
      height = pred->height();
    }
  } 

  {
    auto c = db_->lru_cache(shard, pool_id);
    uint64_t lt = c->FindLessThan(key);
    if (lt != 0) {
      pred = (Node*) ((PmemPtr*) &lt)->get();
      height = pred->height();
    }
  }
  search_visit_cnt_++;
  height_visit_cnt_[height - 1]++;

  // NUMA-local upper layers
  for (int i = height - 1; i >= 1; i--) {
    while (true) {
      curr_paddr_dump = pred->next[i];
      curr = (Node*) ((PmemPtr*) &curr_paddr_dump)->get();
      if (curr) {
        search_visit_cnt_++;
        height_visit_cnt_[i]++;
        if (curr->key.Compare(key) < 0) {
          pred = curr;
          continue;
        }
      }
      break;
    }
  }

  // Braided bottom layer
  if (pred == skiplist->head(pool_id)) {
    if (pool_id != 0) {
      search_visit_cnt_++;
      height_visit_cnt_[kMaxHeight - 1]++;
    }
    pred = skiplist->head(0);
  }
  while (true) {
    curr_paddr_dump = pred->next[0];
    curr = (Node*) ((PmemPtr*) &curr_paddr_dump)->get();
    if (curr) {
      search_visit_cnt_++;
      height_visit_cnt_[0]++;
      if (curr->key.Compare(key) < 0) {
        pred = curr;
        continue;
      }
    }
    //fprintf(stdout, "lookupkey=%zu, curr->key=%zu\n", key, curr->key);
    break;
  }
  return curr_paddr_dump;
}

#endif  // LISTDB_DB_CLIENT_H_