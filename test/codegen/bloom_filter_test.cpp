//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// bloom_filter_test.cpp
//
// Identification: test/codegen/bloom_filter_test.cpp
//
// Copyright (c) 2015-17, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <unordered_set>
#include <vector>
#include <cstdlib>

#include "codegen/codegen.h"
#include "codegen/function_builder.h"
#include "codegen/lang/if.h"
#include "codegen/lang/loop.h"
#include "codegen/proxy/bloom_filter_proxy.h"
#include "codegen/testing_codegen_util.h"
#include "sql/testing_sql_util.h"
#include "optimizer/optimizer.h"
#include "common/timer.h"
#include "concurrency/transaction_manager_factory.h"
#include "executor/plan_executor.h"
#include "planner/hash_join_plan.h"

namespace peloton {
namespace test {

class BloomFilterCodegenTest : public PelotonTest {
public:
  BloomFilterCodegenTest() {
    // Create test db
    auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();
    auto txn = txn_manager.BeginTransaction();
    catalog::Catalog::GetInstance()->CreateDatabase(DEFAULT_DB_NAME, txn);
    txn_manager.CommitTransaction(txn);
  }
  
  ~BloomFilterCodegenTest() {
    // Drop test db
    auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();
    auto txn = txn_manager.BeginTransaction();
    catalog::Catalog::GetInstance()->DropDatabaseWithName(DEFAULT_DB_NAME, txn);
    txn_manager.CommitTransaction(txn);
  }
  
  int UpDivide(int num1, int num2) {
    return (num1 + num2 - 1) / num2;
  }
  
  void InsertTuple(const std::vector<int> &vals, storage::DataTable *table,
                   concurrency::Transaction* txn);
  
  void CreateTable(std::string table_name, int tuple_size,
                   concurrency::Transaction* txn);
};

TEST_F(BloomFilterCodegenTest, FalsePositiveRateTest) {
  codegen::CodeContext code_context;
  codegen::CodeGen codegen(code_context);

  // Generate an array of distinct random numbers.
  // Insert the first half into the bloom filter and
  // use the second half to test the false positive rate
  const int size = 100000;
  std::unordered_set<int> number_set;
  while (number_set.size() != size) {
    number_set.insert(rand());
  }
  std::vector<int> numbers(number_set.begin(), number_set.end());

  // Build the test function that has the following logic:
  // define @TestBloomFilter(BloomFilter* bloom_filter, i32* numbers, i32 size,
  //                         i32* false_positive_cnt) {
  //   // Insert the first half into the bloom filter
  //   for (i32 i = 0; i < size / 2; i++) {
  //      bloom_filter.Add(numbers[i]);
  //   }
  //   // Test the second half and measure false positive cnt
  //   for (i32 i = size / 2; i < size; i++) {
  //      if (bloom_filter.Contains) {
  //         *false_positive_cnt ++;
  //      }
  //   }
  // }
  codegen::FunctionBuilder func{
      code_context,
      "TestBloomFilter",
      codegen.VoidType(),
      {{"bloom_filter",
        codegen::BloomFilterProxy::GetType(codegen)->getPointerTo()},
       {"numbers", codegen.Int32Type()->getPointerTo()},
       {"size", codegen.Int32Type()},
       {"false_positive_cnt", codegen.Int32Type()->getPointerTo()}}};
  {
    llvm::Value *bloom_filter = func.GetArgumentByPosition(0);
    llvm::Value *number_array = func.GetArgumentByPosition(1);
    llvm::Value *size_val = func.GetArgumentByPosition(2);
    llvm::Value *false_positive_cnt = func.GetArgumentByPosition(3);
    llvm::Value *index = codegen.Const32(0);
    llvm::Value *half_size = codegen->CreateUDiv(size_val, codegen.Const32(2));
    llvm::Value *finish_cond = codegen->CreateICmpULT(index, half_size);
    
    // Loop that inserts the first half of array into the bloom filter
    codegen::lang::Loop insert_loop{codegen, finish_cond, {{"i", index}}};
    {
      index = insert_loop.GetLoopVar(0);
      
      // Get numbers[i]
      llvm::Value *number = codegen->CreateLoad(
          codegen->CreateInBoundsGEP(codegen.Int32Type(), number_array, index));
      codegen::Value number_val{codegen::type::Type(type::INTEGER, false),
                                number};
      // Insert numbers[i] into bloom filter
      codegen::BloomFilter::Add(codegen, bloom_filter, {number_val});

      index = codegen->CreateAdd(index, codegen.Const32(1));
      insert_loop.LoopEnd(codegen->CreateICmpULT(index, half_size), {index});
    }

    // Loop that test the false positive rate
    finish_cond = codegen->CreateICmpULT(half_size, size_val);
    codegen::lang::Loop test_loop{codegen, finish_cond, {{"i", half_size}}};
    {
      index = test_loop.GetLoopVar(0);

      // Get numbers[i]
      llvm::Value *number = codegen->CreateLoad(
          codegen->CreateInBoundsGEP(codegen.Int32Type(), number_array, index));
      codegen::Value number_val{codegen::type::Type(type::INTEGER, false),
                                number};
      
      // Test if numbers[i] is contained in bloom filter
      llvm::Value *contains =
          codegen::BloomFilter::Contains(codegen, bloom_filter, {number_val});
      codegen::lang::If if_contains{codegen, contains};
      {
        codegen->CreateStore(
            codegen->CreateAdd(codegen->CreateLoad(false_positive_cnt),
                               codegen.Const32(1)), false_positive_cnt);
      }
      if_contains.EndIf();

      index = codegen->CreateAdd(index, codegen.Const32(1));
      test_loop.LoopEnd(codegen->CreateICmpULT(index, size_val), {index});
    }

    func.ReturnAndFinish();
  }

  ASSERT_TRUE(code_context.Compile());

  typedef void (*ftype)(codegen::BloomFilter * bloom_filter, int *, int, int *);
  ftype f = (ftype)code_context.GetRawFunctionPointer(func.GetFunction());

  codegen::BloomFilter bloom_filter;
  bloom_filter.Init(size / 2);
  int num_false_positive = 0;

  // Run the function
  f(&bloom_filter, &numbers[0], size, &num_false_positive);
  double actual_FPR = (double)num_false_positive / (size / 2);
  double expected_FPR = codegen::BloomFilter::kFalsePositiveRate;
  LOG_DEBUG("Expected FPR %f, Actula FPR: %f", expected_FPR, actual_FPR);

  // Difference should be within 10%
  EXPECT_LT(expected_FPR * 0.9, actual_FPR);
  EXPECT_LT(actual_FPR, expected_FPR * 1.1);

  bloom_filter.Destroy();
}
  
  
// Testing whether bloom filter can improve the performance of hash join
// when the hash table is bigger than L3 cache and selectivity is low
TEST_F(BloomFilterCodegenTest, PerformanceTest) {
  std::unique_ptr<optimizer::AbstractOptimizer> optimizer(
    new optimizer::Optimizer());
  auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();
  auto *catalog = catalog::Catalog::GetInstance();
  
  auto *txn = txn_manager.BeginTransaction();
  
  // Initialize tables. test1 is the inner table from which we build the
  // hash table. test2 is the outer table will probe the hash table.
  const std::string table1_name = "test1";
  const std::string table2_name = "test2";
  const int table1_tuple_size = 512;
  const int table2_tuple_size = 8;
  const int bigint_size = 8;
  CreateTable(table1_name, table1_tuple_size, txn);
  CreateTable(table2_name, table2_tuple_size, txn);

  int L3_cache_size = 6291456;
//  const int L3_cache_size = 600000;
  const int table1_target_size = L3_cache_size * 10;
  const double selectivity = 0.1;
  const int outer_to_inner_ratio = 4;

  // Load the test1 until its size is bigger than a certain ratio of L3 cache.
  // For example, if the ratio is 10, every hash probe will have 90% chance
  // to be a cache miss
  int curr_size = 0;
  std::vector<int> numbers;
  std::unordered_set<int> number_set;
  auto *table1 = catalog->GetTableWithName(DEFAULT_DB_NAME, table1_name, txn);
  while (curr_size < table1_target_size) {
    int random = rand();
    numbers.push_back(random);
    number_set.insert(random);
    
    std::vector<int> vals(UpDivide(table1_tuple_size, bigint_size), random);
    InsertTuple(vals, table1, txn);
    
    curr_size += table1_tuple_size;
  }
  
  LOG_INFO("Finish populating test1");
  
  // Load the inner table which contains twice tuples as the outer table
  auto *table2 = catalog->GetTableWithName(DEFAULT_DB_NAME, table2_name, txn);
  unsigned outer_table_cardinality = numbers.size() * outer_to_inner_ratio;
  for (unsigned i = 0; i < outer_table_cardinality; i++) {
    int number;
    if (rand() % 100 < selectivity * 100) {
      // Pick a random number from the inner table
      number = numbers[rand() % numbers.size()];
    } else {
      // Pick a random number that is not in inner table
      do {
        number = rand();
      } while (number_set.count(number) == 1);
    }
    std::vector<int> vals(UpDivide(table2_tuple_size, bigint_size), number);
    InsertTuple(vals, table2, txn);
  }
  
  LOG_INFO("Finish populating test2");
  
  
  // Get a microsecond resolution timer
  Timer<std::ratio<1, 1000000>> timer;
  
  // Build and execute the join plan
  std::string query = "SELECT * FROM test1 as t1, test2 as t2 "
                                  "WHERE t1.c0 = t2.c0";
  int num_iter = 5;
  // Execute plan with bloom filter disabled
  for (int i = 0; i < num_iter; i++) {
    auto plan1 = TestingSQLUtil::GeneratePlanWithOptimizer(optimizer, query, txn);
    const_cast<planner::AbstractPlan*>(plan1->GetChild(0))
      ->SetCardinality(numbers.size());
    dynamic_cast<planner::HashJoinPlan*>(plan1.get())->SetBloomFilterFlag(false);
    std::vector<StatementResult> result;
    executor::ExecuteResult exe_result;
    timer.Start();
    executor::PlanExecutor::ExecutePlan(plan1, txn, {}, result, {}, exe_result);
    timer.Stop();
    double durable1 = timer.GetDuration();
    timer.Reset();
    LOG_INFO("Execution time without bloom filter: %f", durable1);
  }
  
  // Execute plan with bloom filter enabled
  for (int i = 0; i < num_iter; i++) {
    auto plan2 = TestingSQLUtil::GeneratePlanWithOptimizer(optimizer, query, txn);
    const_cast<planner::AbstractPlan*>(plan2->GetChild(0))
      ->SetCardinality(numbers.size());
    dynamic_cast<planner::HashJoinPlan*>(plan2.get())->SetBloomFilterFlag(true);
    std::vector<StatementResult> result;
    executor::ExecuteResult exe_result;
    timer.Start();
    executor::PlanExecutor::ExecutePlan(plan2, txn, {}, result, {}, exe_result);
    timer.Stop();
    double durable2 = timer.GetDuration();
    timer.Reset();
    LOG_INFO("Execution time with bloom filter: %f", durable2);
  }
  
  txn_manager.CommitTransaction(txn);
}
  
void BloomFilterCodegenTest::CreateTable(std::string table_name, int tuple_size,
                 concurrency::Transaction* txn) {
  int curr_size = 0;
  size_t bigint_size = type::Type::GetTypeSize(type::TypeId::BIGINT);
  std::vector<catalog::Column> cols;
  while (curr_size < tuple_size) {
    cols.push_back(catalog::Column{type::TypeId::BIGINT, bigint_size,
      "c" + std::to_string(curr_size / bigint_size), true});
    curr_size += bigint_size;
  }
  auto *catalog = catalog::Catalog::GetInstance();
  catalog->CreateTable(DEFAULT_DB_NAME, table_name,
      std::make_unique<catalog::Schema>(cols), txn);
}

void BloomFilterCodegenTest::InsertTuple(const std::vector<int> &vals,
                                         storage::DataTable *table,
                                         concurrency::Transaction* txn) {
  auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();
  storage::Tuple tuple{table->GetSchema(), true};
  for (unsigned i = 0; i < vals.size(); i++) {
    tuple.SetValue(i, type::ValueFactory::GetBigIntValue(vals[i]));
  }
  ItemPointer *index_entry_ptr = nullptr;
  auto tuple_slot_id = table->InsertTuple(&tuple, txn, &index_entry_ptr);
  PL_ASSERT(tuple_slot_id.block != INVALID_OID);
  PL_ASSERT(tuple_slot_id.offset != INVALID_OID);
  txn_manager.PerformInsert(txn, tuple_slot_id, index_entry_ptr);
}


}  // namespace test
}  // namespace peloton