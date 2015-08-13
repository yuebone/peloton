/*-------------------------------------------------------------------------
 *
 * merge_join.h
 * file description
 *
 * Copyright(c) 2015, CMU
 *
 * /peloton/src/executor/merge_join_executor.h
 *
 *-------------------------------------------------------------------------
 */

#pragma once

#include "backend/executor/abstract_join_executor.h"
#include "backend/planner/merge_join_node.h"

#include <vector>

namespace peloton {
namespace executor {

class MergeJoinExecutor : public AbstractJoinExecutor {
  MergeJoinExecutor(const MergeJoinExecutor &) = delete;
  MergeJoinExecutor &operator=(const MergeJoinExecutor &) = delete;

 public:
  explicit MergeJoinExecutor(planner::AbstractPlanNode *node,
                                  ExecutorContext *executor_context);

 protected:
  bool DInit();

  bool DExecute();

  bool left_end_;

  bool right_end_;

 private:

  size_t Advance(LogicalTile *tile, size_t start_row, bool is_left);

  /** @beief a vector of join clauses
   * Get this from plan node while init */
  const std::vector<planner::MergeJoinNode::JoinClause> *join_clauses_;

};

}  // namespace executor
}  // namespace peloton