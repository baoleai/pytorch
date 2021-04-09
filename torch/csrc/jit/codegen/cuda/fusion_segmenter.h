#pragma once

#include <torch/csrc/jit/codegen/cuda/fusion.h>
#include <torch/csrc/jit/codegen/cuda/ir_base_nodes.h>
#include <torch/csrc/jit/codegen/cuda/kernel_cache.h>
#include <torch/csrc/jit/codegen/cuda/scheduler/all_schedulers.h>
#include <torch/csrc/jit/codegen/cuda/scheduler/registry.h>

#include <deque>
#include <list>
#include <unordered_set>
#include <vector>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

class SegmentedGroup;
class SegmentCandidateFinder;

// A directed edge on DAG,
// Wrapper for values, edges between segmented groups which are made up
// of Exprs. Multiple edges can exist between segmented groups.
struct SegmentedEdge {
  SegmentedEdge(SegmentedGroup* from, SegmentedGroup* to, Val* val)
      : from(from), to(to), val(val) {}

  SegmentedGroup* from;
  SegmentedGroup* to;
  Val* val;

  void print() const;
};

std::ostream& operator<<(std::ostream& os, const SegmentedEdge* edge);

//! Groups together expressions which create a segmented group
//! Can be used to produce fusions
class TORCH_CUDA_CU_API SegmentedGroup {
 public:
  SegmentedGroup() = default;

  SegmentedGroup(Expr* expr) {
    exprs_.push_back(expr);
  }

  //! Checks if this group takes original fusion's input
  bool isInputGroup() {
    return !input_vals.empty();
  };

  //! Checks if this group is used any where in the segmented fusion
  bool isConnected() const {
    return !producer_edges.empty() || !consumer_edges.empty() ||
        !output_vals.empty();
  }

  //! returns the id assigned by segment pass
  int groupId() const {
    return group_id_;
  }

  //! Returns inputs that this group shares with the original fusion
  const auto& inputs() const {
    return input_vals;
  }

  //! Returns outputs that this group shares with the original fusion
  const auto& outputs() const {
    return output_vals;
  }

  //! Returns the schedule heuristic associated with this group
  ScheduleHeuristic heuristic() const {
    return heuristic_;
  }

  //! Returns the exprs that make up this group
  const auto& exprs() const {
    return exprs_;
  }

  //! Debug print function
  void print() const;

 public:
  //! "Ancestor nodes", towards inputs of segmentedDAG
  std::vector<SegmentedEdge*> producer_edges;

  //! "Descendent nodes", towards outputs of segmentedDAG
  std::vector<SegmentedEdge*> consumer_edges;

  //! Composite Fusion inputs in this group
  std::vector<Val*> input_vals;

  //! Composite Fusion outputs in this group
  std::vector<Val*> output_vals;

 private:
  friend class SegmentCandidateFinder;
  friend class SegmentedFusion;
  friend class FusionKernelRuntime;

  //! unique identifier of group in the segmented fusion
  int group_id_ = -1;

  //! The scheduler to use for compiling this group
  ScheduleHeuristic heuristic_ = ScheduleHeuristic::PointWise;

  //! Exprs that make up the group
  std::vector<Expr*> exprs_;

  //! Maximum path distance from an input segmented group required for
  //! Theorem 4.2
  int level_ = -1;

  //! traversal marker, has this node already been processed
  bool visited_ = false;

  //! Did we select another group to merge with
  SegmentedGroup* merge_with_ = nullptr;

  //! if we selected another group to merge, which edge is to be contracted
  SegmentedEdge* merge_through_ = nullptr;

  //! Has this node been merged?
  bool merged_ = false;

 private:
  //! Utility to convert edge vector to value vector
  std::vector<Val*> edgesToVals(const std::vector<SegmentedEdge*>& se_v);

  //! Reset method to call at begining of each
  //!  merge node iteration
  void clearTraversalInfo();

  //! To be called at the very end of segment fusion
  //!  no more segment merging should be done beyond
  void finalize();

  //! Return all segmented groups connected with *this
  std::vector<SegmentedGroup*> getNeighbors();

  //! Utility struct to represent a group connection
  //!  both the group to connect with and the edge
  //!  to connect through
  struct NeighborGroup {
    NeighborGroup(SegmentedGroup* g, SegmentedEdge* e) : group(g), edge(e) {}
    SegmentedGroup* group;
    SegmentedEdge* edge;
  };

  //! TODO: May want to sort this based on size of connections between this and
  //! neighbors as well as if the connection is an output of the fusion (has to
  //! be saved to gmem anyways)
  std::vector<NeighborGroup> getNeighborGroups();

  //! Look at all neighbors of this and return who this could merge with based
  //! on level values of this, neighbors, and merged neighbors of neighbors
  std::vector<NeighborGroup> getMergeCandidates();

  //! Assign schedule heuristic to this group
  void setHeuristic(ScheduleHeuristic sh) {
    heuristic_ = sh;
  }

  //! Assign Id for this group
  void setID(int id) {
    TORCH_INTERNAL_ASSERT(group_id_ == -1);
    group_id_ = id;
  }
};

std::ostream& operator<<(std::ostream& os, const SegmentedGroup* group);

//! Auxiliary class for storing heuristics. The managed data is either
//!  a single scheduler entry for complete fusion,
//!  or a vector of schedulers, one for each segment, for segmented fusion.
class TORCH_CUDA_CU_API FusionHeuristics {
  using SchedulerEntryOwningPtr = std::unique_ptr<SchedulerEntry>;

 public:
  //! Constructor for segmented fusion case. Created with empty list and
  //!  uses emplaceBack for inserting heuristics in order
  explicit FusionHeuristics() = default;

  //! Constructor for complete fusion case, generates the scheduler entry
  //!  for the fusion owning the given expression
  explicit FusionHeuristics(
      ScheduleHeuristic schedule_heuristic,
      ExpressionEvaluator& expr_eval) {
    heuristics_.emplace_back(SchedulerEntry::makeEntry(
        schedule_heuristic, expr_eval.fusion(), expr_eval));
    is_segmented_ = false;
  }

  //! Place a scheduler entry on the list. Applies to segmented fusion only.
  void emplaceBack(SchedulerEntryOwningPtr&& pt) {
    TORCH_INTERNAL_ASSERT(is_segmented_);
    heuristics_.emplace_back(std::move(pt));
  }

  //! Returns list of schedulers for a segmneted fusion.
  const std::vector<SchedulerEntryOwningPtr>& heuristicsList() const {
    return heuristics_;
  }

  //! Returns the single scheduler for a complete fusion.
  SchedulerEntry* singleHeuristics() {
    TORCH_INTERNAL_ASSERT(!is_segmented_);
    return heuristics_.begin()->get();
  }

 private:
  std::vector<SchedulerEntryOwningPtr> heuristics_;
  bool is_segmented_ = true;
};

//! Exported Interface for representing segmented fusion graph
//!   this class owns the segmented groups
class TORCH_CUDA_CU_API SegmentedFusion {
 public:
  explicit SegmentedFusion(const Fusion* fusion);

  //! Is the fusion segmented?
  bool isSegmented() const {
    return !groups_.empty();
  }

  std::vector<SegmentedGroup*>& groups() {
    return groups_;
  }

  std::vector<SegmentedEdge*>& edges() {
    return edges_;
  }

  const std::vector<SegmentedGroup*>& cgroups() const {
    return groups_;
  }

  const std::vector<SegmentedEdge*>& cedges() const {
    return edges_;
  }

  //! Returns the original un-segmented fusion
  Fusion& completeFusion() {
    return fusion_;
  }

  const auto& inputs() const {
    return fusion_.inputs();
  }

  const auto& outputs() const {
    return fusion_.outputs();
  }

  //! Make a clone of the group and convert to fusion
  std::unique_ptr<Fusion> makeFusion(SegmentedGroup* sg);

  //! Make heuristics for all groups in this segmented fusion
  std::unique_ptr<FusionHeuristics> makeHeuristics(
      const at::ArrayRef<IValue>& inputs);

  //! Inline Debug print for segmented fusion
  std::string toString(int verbosity) const;

  //! Debug drawing for graphviz
  void draw();

  //! Debug print for segmented fusions
  void print() const;

  //! API for adding groups
  SegmentedGroup* newGroup();

  //! API shortcut for adding a singleton group
  SegmentedGroup* newGroup(Expr* expr);

  //! API for adding edges
  SegmentedEdge* newEdge(SegmentedGroup* from, SegmentedGroup* to, Val* val);

 protected:
  //! Original full fusion
  Fusion fusion_;

  //! Unique name for segmented fusion
  int segmented_fusion_name_;

  //! States representing segmentation
  std::vector<SegmentedEdge*> edges_;
  std::vector<SegmentedGroup*> groups_;

  //! Owning object to explicitly manage groups and edges
  class Impl {
   public:
    explicit Impl(SegmentedFusion* sf) : owning_fusion_(sf) {}

    SegmentedGroup* makeGroup();
    SegmentedGroup* makeGroup(Expr*);
    SegmentedEdge* makeEdge(SegmentedGroup* from, SegmentedGroup* to, Val* val);
    void cleanUnused();

   private:
    using GroupPtr = std::unique_ptr<SegmentedGroup>;
    using EdgePtr = std::unique_ptr<SegmentedEdge>;
    std::vector<GroupPtr> groups_;
    std::vector<EdgePtr> edges_;
    SegmentedFusion* owning_fusion_;
  };
  Impl impl_;

 protected:
  friend class SegmentCandidateFinder;
  //! Make a heuristics entry for a group and parameters
  std::unique_ptr<SchedulerEntry> makeSchedulerEntry(
      SegmentedGroup* sg,
      ExpressionEvaluator& ee);

  //! Cleanup function to be call at the end of fusion
  //!  segment pass
  void finalize();

  //! Utility to give unique name for each segmented fusion
  static size_t segmentedFusionName() {
    static size_t counter = 0;
    return counter++;
  }
};

//! This is a base class for segmenter analysis
//!  provides the minimal implementation on header so that
//!  a unique_ptr can use this base class
//!  actual implementations of analyses are in the .cpp files
//! TODO: In the next refactor PR, should put segment candidate
//!  finder in .cpp file completely since API doesn't require these
//!  details
class SegmenterAnalysis : public PolymorphicBase {};
class GroupDependencyAnalysis;

// Manual node merging passes
class CombineReductions;

//! Options to configure/debug candidate finder
struct TORCH_CUDA_CU_API SegmentCandidateFinderOptions {
  bool run_combine_reductions = true;
  bool run_herrmann_merge = true;
  bool run_final_merge = true;
};

//!  SegmentCandidateFinder
//!    Responsible for going through DAG and proposing things we could try to
//!    fuse together, calls "canGenerateCode" on these proposed segments to see
//!    if they are valid and we can generate code for them.
//!  FusionSegment
//!    A group of exprs that are segmented together
//!  FusionSegmentConnections
//!    Holds vals and what they connect. In other words it's a val that is an
//!    output of a FusionSegment "from" and an input of FusionSegment "to".
//!    There's nothing preventing from a val being between segments twice.
//!    TODO: make sure there's nothing wrong with segmentation on nodes that
//!    have the same value input twice. i.e. (B = A*A)
//! Selecting segments to propose is based on the theorem 4.2 in the paper which
//! makes sure when segment the segmented graph will be a DAG (assumes Fusion is
//! already a DAG). The segmentation code relies on assumptions of DAG-ness
//! during segmentation, meaning proposed merging of groups must maintain the
//! DAG property of the graph.
//!
//! Julien Herrmann, Yusuf Özkaya, Bora Uçar, Kamer Kaya, Umit Catalyurek.
//! Multilevel Algorithms for Acyclic Partitioning of Directed Acyclic Graphs.
//! SIAM Journal on Scientific Computing, Society for Industrial and Applied
//! Mathematics, 2019, 41 (4), pp.A2117-A2145. ff10.1137/18M1176865ff.
//! ffhal02306566f
class TORCH_CUDA_CU_API SegmentCandidateFinder {
 public:
  // Take a copy of fusion to own
  SegmentCandidateFinder(
      const Fusion* fusion,
      SegmentCandidateFinderOptions options);

  static std::unique_ptr<SegmentedFusion> segment(
      const Fusion* fusion,
      SegmentCandidateFinderOptions options = SegmentCandidateFinderOptions()) {
    SegmentCandidateFinder scf(fusion, options);
    return std::move(scf.segmented_fusion_);
  }

 private:
  void resetTraversal();

  void resetLevels();

  SegmentedGroup* mergeNodes();

  bool codeGenSupportedMerge(SegmentedEdge* edge);

  void findSegments();

  std::unordered_set<SegmentedEdge*> disconnectGroup(SegmentedGroup* group);

  std::vector<SegmentedGroup*>& groups() {
    TORCH_INTERNAL_ASSERT(
        segmented_fusion_ != nullptr, "Segment finder not owinging any fusion");
    return segmented_fusion_->groups();
  }

  std::vector<SegmentedEdge*>& edges() {
    TORCH_INTERNAL_ASSERT(
        segmented_fusion_ != nullptr, "Segment finder not owinging any fusion");
    return segmented_fusion_->edges();
  }

  Fusion& completeFusion() {
    TORCH_INTERNAL_ASSERT(
        segmented_fusion_ != nullptr, "Segment finder not owinging any fusion");
    return segmented_fusion_->completeFusion();
  }

  //! Additional merging iteration, clean up the rest of
  //!  the merging opportunities
  //!  Herrmann et al. is a fast and safe algorithm for finding merge candidates
  //!  but can become too conservative in our use cases because we place
  //!  additional qualifiers on valid merges other than having to generate DAGs,
  //!  i.e. canSchedule. So we need a bruteforce final merging iteration as a
  //!  clean up pass. Cost isn't expected to be high since the graph at this
  //!  stage is already quite merged. Example cf. test_gpu.cpp:
  //!  FusionDAGMerging_CUDA
  //!
  //!  This merging algorithm is based on Theorem 4.1 of Herrmann et al.,
  //!   to check if a producer-consumer pair can be merged into one group,
  //!   it's enough to check if any other consumer of the producer also
  //!   produces the consumer.
  void finalMerge();

  //! Duplicate and add all exprs producing the used
  //!  scalar values in group
  void resolveScalarsInGroup(SegmentedGroup* group);

  //! Utility function to merge a vector of groups in one step,
  //!  need to check for DAG condition before using this method
  SegmentedGroup* mergeAllGivenGroups(
      const std::vector<SegmentedGroup*>& groups);

  //! Utility to remove a group and corresponding edges
  //!  TODO: remove inline versions of this as much as possible
  void eraseGroups(std::unordered_set<SegmentedGroup*>& groups_to_erase);

  void finalize();

  //! Return the resulting heuristic corresponding to the merged
  //!  group built by merging the two groups connected by edge
  ScheduleHeuristic deriveHeuristic(SegmentedGroup* edge);

  GroupDependencyAnalysis* getGroupDependency();

 protected:
  //! These are the merge node heuristic passes, should
  //!  eventually should have a dedicated interface
  //!  instead of keeping adding friends
  friend class CombineReductions;

  //! options to configure and debug the segment process
  SegmentCandidateFinderOptions options_;

  std::deque<SegmentedGroup*> to_visit_;
  std::vector<SegmentedGroup*> next_to_visit_;

  std::unordered_set<SegmentedGroup*> clean_up_groups_;
  std::unordered_set<SegmentedEdge*> clean_up_edges_;

  std::vector<SegmentedGroup*> to_merge_;

  std::unique_ptr<SegmentedFusion> segmented_fusion_;

  std::unique_ptr<SegmenterAnalysis> group_dependency_;
};

TORCH_CUDA_CU_API std::string toString(const SegmentedGroup* group);
TORCH_CUDA_CU_API std::string toString(const SegmentedEdge* edge);
TORCH_CUDA_CU_API std::string toString(const SegmentedFusion* segmented_fusion);
TORCH_CUDA_CU_API std::string toString(
    const SegmentCandidateFinderOptions& segment_options);

} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch
