#include <glog/logging.h>
#include <gtest/gtest.h>
#include <libpmemobj.h>
#include "../garbage_list.h"
#include "../garbage_list_unsafe.h"

#define CREATE_MODE_RW (S_IWUSR | S_IRUSR)

static const char* pool_name = "garbage_list_pool.data";
static const char* layout_name = "garbagelist";
static const uint64_t pool_size = 1024 * 1024 * 1024;

struct MockItem {
 public:
  MockItem() : deallocations{0} {}

  static void Destroy(void* destroyContext, void* p) {
    ++(reinterpret_cast<MockItem*>(p))->deallocations;
  }

  std::atomic<uint64_t> deallocations;
};

static bool FileExists(const char* pool_path) {
  struct stat buffer;
  return (stat(pool_path, &buffer) == 0);
}

class GarbageListPMTest : public ::testing::Test {
 public:
  GarbageListPMTest() {}

 protected:
  EpochManager epoch_manager_;
  GarbageList garbage_list_;
  std::stringstream out_;
  PMEMobjpool* pool_{nullptr};

  virtual void SetUp() {
    PMEMobjpool* tmp_pool;
    if (!FileExists(pool_name)) {
      tmp_pool =
          pmemobj_create(pool_name, layout_name, pool_size, CREATE_MODE_RW);
      LOG_ASSERT(tmp_pool != nullptr);
    } else {
      tmp_pool = pmemobj_open(pool_name, layout_name);
      LOG_ASSERT(tmp_pool != nullptr);
    }
    pool_ = tmp_pool;

    out_.str("");
    ASSERT_TRUE(epoch_manager_.Initialize());
    ASSERT_TRUE(garbage_list_.Initialize(&epoch_manager_, pool_, 1024));
  }

  virtual void TearDown() {
    EXPECT_TRUE(garbage_list_.Uninitialize());
    EXPECT_TRUE(epoch_manager_.Uninitialize());
    Thread::ClearRegistry(true);
  }
};

TEST_F(GarbageListPMTest, Uninitialize) {
  MockItem items[2];

  EXPECT_TRUE(garbage_list_.Push(&items[0], MockItem::Destroy, nullptr));
  EXPECT_TRUE(garbage_list_.Push(&items[1], MockItem::Destroy, nullptr));
  EXPECT_TRUE(garbage_list_.Uninitialize());
  EXPECT_EQ(1, items[0].deallocations);
  EXPECT_EQ(1, items[1].deallocations);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
