#include "garbage_list.h"

bool IGarbageList::Initialize(EpochManager* epoch_manager, size_t size) {
  (epoch_manager);
  (size);
  return true;
}
bool IGarbageList::Uninitialize() { return true; }
GarbageList::Item* GarbageList::Item::GetItemFromRemoved(void* mem) {
  return reinterpret_cast<Item*>((char*)mem - 24);
}
void GarbageList::Item::SetValue(void* removed_item, Epoch epoch,
                                 IGarbageList::DestroyCallback callback,
                                 void* context) {
  assert(this->removal_epoch == invalid_epoch);

#ifdef PMEM
  auto value = _mm256_set_epi64x((int64_t)removed_item, (int64_t)context,
                                 (int64_t)callback, (int64_t)epoch);
  _mm256_stream_si256((__m256i*)(this), value);
#else
  this->destroy_callback = callback;
  this->destroy_callback_context = context;
  this->removed_item = removed_item;
  this->removal_epoch = epoch;
#endif
}
GarbageList::GarbageList()
    : epoch_manager_{}, tail_{}, item_count_{}, items_{} {}
GarbageList::~GarbageList() { Uninitialize(); }

#ifdef PMEM
bool GarbageList::Initialize(EpochManager* epoch_manager, PMEMobjpool* pool_,
                             size_t item_count) {
#else
bool GarbageList::Initialize(EpochManager* epoch_manager, size_t item_count) {
#endif
  if (epoch_manager_) return true;

  if (!epoch_manager) return false;

  if (!item_count || !IS_POWER_OF_TWO(item_count)) {
    return false;
  }

  size_t nItemArraySize = sizeof(*items_) * item_count;

#ifdef PMEM
  // TODO(hao): better error handling
  PMEMoid ptr;
  TX_BEGIN(pool_) {
    // Every PMDK allocation so far will pad to 64 cacheline boundry.
    // To prevent memory leak, pmdk will chain the allocations by adding a
    // 16-byte pointer at the beginning of the requested memory, which breaks
    // the memory alignment. the PMDK_PADDING is to force pad again
    pmemobj_zalloc(pool_, &ptr, nItemArraySize + very_pm::kPMDK_PADDING,
                   TOID_TYPE_NUM(char));
    items_ = (GarbageList::Item*)((char*)pmemobj_direct(ptr) +
                                  very_pm::kPMDK_PADDING);
  }
  TX_END
#else
  posix_memalign((void**)&items_, 64, nItemArraySize);
#endif

  if (!items_) return false;

  for (size_t i = 0; i < item_count; ++i) new (&items_[i]) Item{};

  item_count_ = item_count;
  tail_ = 0;
  epoch_manager_ = epoch_manager;

  return true;
}
bool GarbageList::Uninitialize() {
  if (!epoch_manager_) return true;

  for (size_t i = 0; i < item_count_; ++i) {
    Item& item = items_[i];
    if (item.removed_item) {
      item.destroy_callback(item.destroy_callback_context, item.removed_item);
      item.removed_item = nullptr;
      item.removal_epoch = 0;
    }
  }

#ifdef PMEM
  auto oid = pmemobj_oid((char*)items_ - very_pm::kPMDK_PADDING);
  pmemobj_free(&oid);
#else
  delete items_;
#endif

  items_ = nullptr;
  tail_ = 0;
  item_count_ = 0;
  epoch_manager_ = nullptr;

  return true;
}
bool GarbageList::Push(void* removed_item,
                       IGarbageList::DestroyCallback callback, void* context) {
  Epoch removal_epoch = epoch_manager_->GetCurrentEpoch();

  for (;;) {
    int64_t slot = (tail_.fetch_add(1) - 1) & (item_count_ - 1);

    // Everytime we work through 25% of the capacity of the list roll
    // the epoch over.
    if (((slot << 2) & (item_count_ - 1)) == 0)
      epoch_manager_->BumpCurrentEpoch();

    Item& item = items_[slot];

    Epoch priorItemEpoch = item.removal_epoch;
    if (priorItemEpoch == invalid_epoch) {
      // Someone is modifying this slot. Try elsewhere.
      continue;
    }

    Epoch result = CompareExchange64<Epoch>(&item.removal_epoch, invalid_epoch,
                                            priorItemEpoch);
    if (result != priorItemEpoch) {
      // Someone else is now modifying the slot or it has been
      // replaced with a new item. If someone replaces the old item
      // with a new one of the same epoch number, that's ok.
      continue;
    }

    // Ensure it is safe to free the old entry.
    if (priorItemEpoch) {
      if (!epoch_manager_->IsSafeToReclaim(priorItemEpoch)) {
        // Uh-oh, we couldn't free the old entry. Things aren't looking
        // good, but maybe it was just the result of a race. Replace the
        // epoch number we mangled and try elsewhere.
        *((volatile Epoch*)&item.removal_epoch) = priorItemEpoch;
        continue;
      }
      item.destroy_callback(item.destroy_callback_context, item.removed_item);
    }

    Item stack_item;
    stack_item.destroy_callback = callback;
    stack_item.destroy_callback_context = context;
    stack_item.removed_item = removed_item;
    *((volatile Epoch*)&stack_item.removal_epoch) = removal_epoch;

#ifdef PMEM
    auto value = _mm256_set_epi64x((int64_t)removed_item, (int64_t)context,
                                   (int64_t)callback, (int64_t)removal_epoch);
    _mm256_stream_si256((__m256i*)(items_ + slot), value);
#else
    items_[slot] = stack_item;
#endif
    return true;
  }
}
GarbageList::Item* GarbageList::ReserveItem() {
  Epoch removal_epoch = epoch_manager_->GetCurrentEpoch();
  for (;;) {
    int64_t slot = (tail_.fetch_add(1) - 1) & (item_count_ - 1);

    // Everytime we work through 25% of the capacity of the list roll
    // the epoch over.
    if (((slot << 2) & (item_count_ - 1)) == 0)
      epoch_manager_->BumpCurrentEpoch();

    Item& item = items_[slot];
    Epoch priorItemEpoch = item.removal_epoch;

    Epoch result = CompareExchange64<Epoch>(&item.removal_epoch, invalid_epoch,
                                            priorItemEpoch);
    if (result != priorItemEpoch) {
      // Someone else is now modifying the slot or it has been
      // replaced with a new item. If someone replaces the old item
      // with a new one of the same epoch number, that's ok.
      continue;
    }

    // Ensure it is safe to free the old entry.
    if (priorItemEpoch) {
      if (!epoch_manager_->IsSafeToReclaim(priorItemEpoch)) {
        // Uh-oh, we couldn't free the old entry. Things aren't looking
        // good, but maybe it was just the result of a race. Replace the
        // epoch number we mangled and try elsewhere.
        *((volatile Epoch*)&item.removal_epoch) = priorItemEpoch;
        continue;
      }
      item.destroy_callback(item.destroy_callback_context, item.removed_item);
    }
    return &item;
  }
}
bool GarbageList::ResetItem(GarbageList::Item* item) {
  auto old_epoch = item->removal_epoch;
  assert(old_epoch == invalid_epoch);
  item->removal_epoch = 0;
  item->removed_item = nullptr;
  return true;
}

#ifdef PMEM
bool GarbageList::Recovery(EpochManager* epoch_manager,
                           PMEMobjpool* pmdk_pool) {
  uint32_t reclaimed{0};
  for (size_t i = 0; i < item_count_; ++i) {
    Item& item = items_[i];
    if (item.removed_item != nullptr) {
      item.destroy_callback(item.destroy_callback_context, item.removed_item);
      new (&items_[i]) Item{};
      reclaimed += 1;
    }
  }
#ifdef TEST_BUILD
  LOG(INFO) << "[Garbage List]: reclaimed " << reclaimed << " items."
            << std::endl;
#endif
  tail_ = 0;
  epoch_manager_ = epoch_manager;
  pmdk_pool_ = pmdk_pool;
  return true;
}
int32_t GarbageList::Scavenge() {
  auto max_slot = tail_.load(std::memory_order_relaxed);
  int32_t scavenged = 0;

  for (int64_t slot = 0; slot < item_count_; ++slot) {
    auto& item = items_[slot];
    Epoch priorItemEpoch = item.removal_epoch;
    if (priorItemEpoch == 0 || priorItemEpoch == invalid_epoch) {
      // Someone is modifying this slot. Try elsewhere.
      continue;
    }

    Epoch result = CompareExchange64<Epoch>(&item.removal_epoch, invalid_epoch,
                                            priorItemEpoch);
    if (result != priorItemEpoch) {
      // Someone else is now modifying the slot or it has been
      // replaced with a new item. If someone replaces the old item
      // with a new one of the same epoch number, that's ok.
      continue;
    }

    if (priorItemEpoch) {
      if (!epoch_manager_->IsSafeToReclaim(priorItemEpoch)) {
        // Uh-oh, we couldn't free the old entry. Things aren't looking
        // good, but maybe it was just the result of a race. Replace the
        // epoch number we mangled and try elsewhere.
        *((volatile Epoch*)&item.removal_epoch) = priorItemEpoch;
        continue;
      }
      item.destroy_callback(item.destroy_callback_context, item.removed_item);
    }

    // Now reset the entry
    Item stack_item;
    stack_item.destroy_callback = nullptr;
    stack_item.destroy_callback_context = nullptr;
    stack_item.removed_item = nullptr;
    *((volatile Epoch*)&stack_item.removal_epoch) = 0;
#ifdef PMEM
    auto value =
        _mm256_set_epi64x((int64_t)0, (int64_t)0, (int64_t)0, (int64_t)0);
    _mm256_stream_si256((__m256i*)(items_ + slot), value);
#else
    items_[slot] = stack_item;
#endif
  }

  return scavenged;
}
EpochManager* GarbageList::GetEpoch() { return epoch_manager_; }
#endif
