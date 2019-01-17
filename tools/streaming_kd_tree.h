#pragma once

#include <vector>
#include <ostream>
#include <glm/glm.hpp>
#include "sparsepp/spp.h"
#include "kd_tree.h"
#include "rsf_file.h"

// TODO: These nodes are getting pretty large now with adding all
// this info.
#pragma pack(1)
struct StreamingKdNode {
	// Interior node, splitting position along the axis
	float split_pos;

	// Leaf and Interior node, offset in 'primitive_indices' to its contained prims,
	// or for interior nodes the surfel LOD representative location in the 'surfels' array
	uint32_t prim_indices_offset;

	// Interior node: low bit indicates whether this child
	// is in the subtree or not (1 if not inside)
	uint32_t right_child;

	// Both: low 2 bits indicate whether this is a leaf or interior node,
	// for interior nodes these indicate which splitting plane was used (x/y/z)
	// Leaf node: number of primitives in the leaf
	// Interior node: left child id, the low third bit indicates if this child
	// is not in the subtree (1 if not inside)
	uint32_t num_prims;

	// Interior node
	StreamingKdNode(float split_pos, uint32_t lod_prim,
			AXIS split_axis);
	// Leaf node
	StreamingKdNode(uint32_t nprims, uint32_t prim_offset);

	void set_left_child(uint32_t left_child, bool external);
	void set_right_child(uint32_t right_child, bool external);

	uint32_t right_child_index() const;
	uint32_t left_child_index() const;

	uint32_t get_num_prims() const;
	uint32_t get_prim_indices_offset() const;
	AXIS split_axis() const;
	bool is_leaf() const;
};

struct KdSubTree {
	// The root node id gives the node id of this subtree's
	// parent node, will be 0 if this is the root of all trees,
	// or non-zero if this is a subtree of some other subtree
	uint32_t root_id;
	Box subtree_bounds;

	std::vector<StreamingKdNode> nodes;
	std::vector<uint32_t> primitive_indices;
	// The LOD and original surfels for this subtree
	std::vector<Surfel> surfels;

	/* Build the kd sub-tree containing the passed subtree nodes,
	 * assumes that subtree_nodes[0] is the tree root
	 */
	KdSubTree(const Box &bounds,
			std::vector<uint32_t> subtree_nodes,
			const std::vector<StreamingKdNode> &all_nodes,
			const std::vector<uint32_t> &prim_indices,
			const std::vector<Surfel> &surfels);

private:
	uint32_t rebuild_subtree(const uint32_t current_node,
			const std::vector<uint32_t> &subtree_nodes,
			const std::vector<StreamingKdNode> &all_nodes,
			const std::vector<uint32_t> &prim_indices,
			const std::vector<Surfel> &all_surfels);
};

/* A median-split kd tree, configured for a streaming LOD
 * use case. Interior nodes contain single surfel primitives
 * which are representative of their contained primitives,
 * and the tree is fragmented into multiple files containing sub-trees
 * to allow loading resolution subsets of the data
 */
struct StreamingSplatKdTree {
	// The surfels input data along with any generated LOD surfels
	std::vector<Surfel> surfels;
	std::vector<Box> bounds;
	// A single kd-tree built over the entire dataset
	std::vector<StreamingKdNode> nodes;
	std::vector<uint32_t> primitive_indices;
	// Extra info for the build step, so we can easily track
	// the subtree bounds
	std::vector<Box> all_node_bounds;

	int max_depth, tree_depth;
	int min_prims;

	/* Build the streaming splat kd tree on the geometry, given
	 * some size limit (in bytes) to constain each subtree file to
	 * when storing primitives of size primitive_size
	 */
	StreamingSplatKdTree(const std::vector<Surfel> &surfels);
	/* Split the kd-tree up into sets of subtrees to constrain the
	 * output files for each subtree to some desired tree depth.
	 */
	std::vector<KdSubTree> build_subtrees(size_t subtree_depth) const;

private:
	/* Recursively build the tree, returns this node's index in the
	 * nodes vector when it's written in
	 */
	uint32_t build_tree(const Box &node_bounds,
			const std::vector<uint32_t> &contained_prims,
			const int depth);
};

