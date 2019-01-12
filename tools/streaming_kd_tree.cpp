#include <iostream>
#include <unordered_map>
#include <stack>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/ext.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>
#include <glm/gtx/component_wise.hpp>
#include "kd_tree.h"
#include "streaming_kd_tree.h"

StreamingKdNode::StreamingKdNode(float split_pos, uint32_t lod_prim, AXIS split_axis)
	: split_pos(split_pos),
	prim_indices_offset(lod_prim),
	right_child(static_cast<uint32_t>(split_axis))
{}
StreamingKdNode::StreamingKdNode(uint32_t nprims, uint32_t prim_offset)
	: prim_indices_offset(prim_offset),
	num_prims(3 | (nprims << 2))
{}
void StreamingKdNode::set_right_child(uint32_t r) {
	right_child |= (r << 2);
}
uint32_t StreamingKdNode::get_num_prims() const {
	return num_prims >> 2;
}
uint32_t StreamingKdNode::right_child_index() const {
	return right_child >> 2;
}
AXIS StreamingKdNode::split_axis() const {
	return static_cast<AXIS>(num_prims & 3);
}
bool StreamingKdNode::is_leaf() const {
	return (num_prims & 3) == 3;
}

// The LOD surfel right now is just the average of the contained primitives
Surfel compute_lod_surfel(const std::vector<uint32_t> &contained_prims,
		const std::vector<Surfel> &surfels)
{
	Surfel lod;
	std::memset(reinterpret_cast<char*>(&lod), 0, sizeof(Surfel));

	for (const auto &p : contained_prims) {
		const Surfel &s = surfels[p];
		lod.x += s.x;
		lod.y += s.y;
		lod.z += s.z;

		lod.nx += s.nx;
		lod.ny += s.ny;
		lod.nz += s.nz;

		lod.r += s.r;
		lod.g += s.g;
		lod.b += s.b;
	}
	lod.x /= contained_prims.size();
	lod.y /= contained_prims.size();
	lod.z /= contained_prims.size();

	lod.nx /= contained_prims.size();
	lod.ny /= contained_prims.size();
	lod.nz /= contained_prims.size();

	lod.r /= contained_prims.size();
	lod.g /= contained_prims.size();
	lod.b /= contained_prims.size();
	return lod;
}

KdSubTree::KdSubTree(const Box &bounds, uint32_t root_id,
		std::vector<StreamingKdNode> subtree_nodes,
		const std::vector<uint32_t> &prim_indices,
		const std::vector<Surfel> &all_surfels)
	: subtree_bounds(bounds),
	root_id(root_id),
	nodes(std::move(subtree_nodes))
{
	// We need to build a new primitive list and primitive indices array
	// specific to this subtree.
	for (auto &n : nodes) {
		if (n.is_leaf()) {
			// Copy over the leaf node primitive indices
			const size_t prim = primitive_indices.size();
			for (size_t i = 0; i < n.get_num_prims(); ++i) {
				const size_t s_idx = prim_indices[n.prim_indices_offset + i];
				primitive_indices.push_back(s_idx);
			}
			n.prim_indices_offset = prim;
		}
	}
}

/* We define sub-tree similarity as the avg. number of surfels shared
 * between the trees. If the subtrees share no surfels, the returned
 * similarity value will be 1, otherwise it will be higher, indicating
 * some amount of surfels are shared between the trees.
 */
float subtree_similarity(const KdSubTree &a, const KdSubTree &b) {
	std::unordered_map<uint32_t, size_t> shared_surfels;
	for (const auto &i : a.primitive_indices) {
		shared_surfels[i]++;
	}
	for (const auto &i : b.primitive_indices) {
		shared_surfels[i]++;
	}
	float similarity = 0;
	for (const auto &s : shared_surfels) {
		similarity += s.second;
	}
	return similarity / shared_surfels.size();
}

SubTreeGroup::SubTreeGroup(std::vector<KdSubTree> insubtrees, const std::vector<Surfel> &all_surfels)
	: subtrees(std::move(insubtrees))
{
	// TODO: Find which surfels we need from the allsurfels list for this set of
	// subtrees and add them to our surfels listing. We then need to re-map
	// the primitive indices arrays in each subtree to point to this new array
	std::unordered_map<size_t, size_t> surfel_indices;
	for (auto &tree : subtrees) {
		for (auto &n : tree.nodes) {
			if (!n.is_leaf()) {
				// Interior nodes have unique generated LOD surfels, and will not
				// share between each other or leaf nodes, so just copy it over
				const size_t prim = surfels.size();
				surfels.push_back(all_surfels[n.prim_indices_offset]);
				n.prim_indices_offset = prim;
			} else {
				// Find if we've already copied some of the surfels used by
				// this leaf node, and can just reference those.
				for (size_t i = 0; i < n.get_num_prims(); ++i) {
					size_t s_idx = tree.primitive_indices[n.prim_indices_offset + i];
					auto fnd = surfel_indices.find(s_idx);
					if (fnd != surfel_indices.end()) {
						s_idx = fnd->second;
					} else {
						const size_t new_idx = surfels.size();
						surfels.push_back(all_surfels[s_idx]);
						s_idx = new_idx;
					}
					tree.primitive_indices[n.prim_indices_offset + i] = s_idx;
				}
			}
		}
	}
}

StreamingSplatKdTree::StreamingSplatKdTree(const std::vector<Surfel> &insurfels)
	: surfels(insurfels),
	max_depth(8 + 1.3 * std::log2(insurfels.size())),
	tree_depth(0),
	min_prims(128)

{
	if (surfels.size() > std::pow(2, 30)) {
		std::cout << "Too many surfels in one streaming kd tree!\n";
		throw std::runtime_error("Too many surfels for one streaming tree");
	}

	std::vector<uint32_t> contained_prims(surfels.size(), 0);
	std::iota(contained_prims.begin(), contained_prims.end(), 0);

	Box tree_bounds;
	for (const auto &s : surfels) { 
		const Box b = surfel_bounds(glm::vec3(s.x, s.y, s.z),
				glm::vec3(s.nx, s.ny, s.nz), s.radius);
		bounds.push_back(b);
		tree_bounds.box_union(b);
	}
	build_tree(tree_bounds, contained_prims, 0);
}
uint32_t StreamingSplatKdTree::build_tree(const Box &node_bounds,
		const std::vector<uint32_t> &contained_prims,
		const int depth)
{
	tree_depth = std::max(tree_depth, depth);

	// We've hit max depth or the prim threshold, so make a leaf
	if (depth >= max_depth || contained_prims.size() <= min_prims) {
		StreamingKdNode node(contained_prims.size(), primitive_indices.size());
		std::copy(contained_prims.begin(), contained_prims.end(),
				std::back_inserter(primitive_indices));
		const uint32_t node_index = nodes.size();
		nodes.push_back(node);
		all_node_bounds.push_back(node_bounds);
		return node_index;
	}

	// We're making an interior node, find the median point and
	// split the objects
	Box centroid_bounds;
	std::vector<glm::vec3> centroids;
	for (const auto &p : contained_prims) {
		centroids.push_back(bounds[p].center());
		centroid_bounds.extend(centroids.back());
	}

	const AXIS split_axis = centroid_bounds.longest_axis();
	std::sort(centroids.begin(), centroids.end(),
		[&](const glm::vec3 &a, const glm::vec3 &b) {
			return a[split_axis] < b[split_axis];
		});
	const float split_pos = centroids[centroids.size() / 2][split_axis];

	// Boxes for left/right child nodes
	Box left_box = node_bounds;
	left_box.upper[split_axis] = split_pos;
	Box right_box = node_bounds;
	right_box.lower[split_axis] = split_pos;
	// Collect primitives for the left/right children
	std::vector<uint32_t> left_prims, right_prims;
	for (const auto &p : contained_prims) {
		if (bounds[p].lower[split_axis] <= split_pos) {
			left_prims.push_back(p);
		}
		if (bounds[p].upper[split_axis] >= split_pos) {
			right_prims.push_back(p);
		}
	}

	++num_inner;
	StreamingKdNode inner(split_pos, surfels.size(), split_axis);
	// TODO: I think this is ok, since we put the LOD surfels at the end after
	// the real surfels, they won't show up accidentally as a "real" surfel
	Surfel lod_surfel = compute_lod_surfel(contained_prims, surfels);
	lod_surfel.radius = glm::compMax(node_bounds.center() - node_bounds.lower) / 2.0;
	surfels.push_back(lod_surfel);

	const uint32_t inner_idx = nodes.size();
	nodes.push_back(inner);
	all_node_bounds.push_back(node_bounds);

	// Build left child, will be placed after this inner node
	build_tree(left_box, left_prims, depth + 1);
	// Build right child
	const uint32_t right_child = build_tree(right_box, right_prims, depth + 1);
	nodes[inner_idx].set_right_child(right_child);
	return inner_idx;
}
std::vector<SubTreeGroup> StreamingSplatKdTree::build_subtrees(size_t subtree_depth) const {
	std::cout << "Tree depth: " << tree_depth
		<< ", max subtree depth: " << subtree_depth
		<< "\n";
	std::cout << "Number of inner nodes: " << num_inner << "\n";

	// Here we want to do a breadth-first traversal down the tree, to try and group
	// files by their level

	// TODO: What we actually want to do in the end to save some disk space
	// is to get together the list of kd trees, without making the surfel copies
	// yet. Then, we bundle the subtrees into files, and collect the surfel subsets
	// used by all the trees, and store the surfels once. This will help a lot with
	// reducing the duplication of surfels we have right now. Then we re-map the
	// primitive indices of each subtree to reference inside this shared surfel list
	// The bundled subtrees should also be grouped based on how many surfels are shared
	// between them, so that we create files that minimize the amount of duplication
	// we have to do of surfels between files.
	//
	// To measure similarity we can count how many times the same primitive index
	// shows up, and then take the average of all prim indices repetition counts.
	// The higher this average is, the more similar the subtrees are.
	//
	// TODO: Actually, we should just group subtrees which are neighbors of each other,
	// or are within the same level. Wouldn't this kind of be the same effect
	// as measuring surfel similarities between them? It may be more expensive to
	// find the similarities though.

	std::vector<KdSubTree> subtrees;
	std::stack<size_t> todo;
	todo.push(0);
	while (!todo.empty()) {
		std::vector<size_t> subtree_nodes, current_level, next_level;
		next_level.push_back(todo.top());
		const Box subtree_bounds = all_node_bounds[todo.top()];
		todo.pop();

		// Traverse the node's children and add them to the subtree
		// until we hit the depth limit
		for (size_t depth = 0; depth < subtree_depth && !next_level.empty(); ++depth) {
			current_level = next_level;
			next_level.clear();
			for (const auto &id : current_level) {
				subtree_nodes.push_back(id);
				const StreamingKdNode &node = nodes[id];
				if (!node.is_leaf()) {
					next_level.push_back(id + 1);
					next_level.push_back(node.right_child_index());
				}
			}
		}

		// The next level have to go into other subtrees, they didn't fit here
		std::cout << "Remaining for next level: {";
		for (const auto &id : next_level) {
			std::cout << id << ", ";
			todo.push(id);
		}
		std::cout << "}\n";

		std::vector<StreamingKdNode> subtree_node_list;
		subtree_node_list.reserve(subtree_nodes.size());
		std::cout << "Nodes in subtree: {";
		for (const auto &i : subtree_nodes) {
			std::cout << i << ", ";
			subtree_node_list.push_back(nodes[i]);
		}
		std::cout << "}\n";
		subtrees.emplace_back(subtree_bounds, subtree_nodes[0],
				subtree_node_list, primitive_indices, surfels);
	}

	std::vector<SubTreeGroup> tree_groups;
	std::cout << "Computing similarities\n" << std::flush;

	float max_sim = 0;
	for (size_t i = 0; i < subtrees.size(); ++i) {
		const auto &a = subtrees[i];
		float avg_sim = 0;
		for (size_t j = 0; j < subtrees.size(); ++j) {
			const auto &b = subtrees[i];
			float sim = subtree_similarity(a, b);
			max_sim = std::max(max_sim, sim);
			avg_sim += sim;
		}
		std::cout << "Avg sim for " << i << " = "
			<< avg_sim / subtrees.size() << "\n" << std::flush;
	}
	std::cout << "Max similarity: " << max_sim << "\n" << std::flush;

	return tree_groups;
}

