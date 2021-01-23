#include <iostream>
#include <ram_forest.h>

// RamForest::Node
const Hash& RamForest::Node::GetHash() const
{
    return this->m_hash;
}

void RamForest::Node::ReHash()
{
    // get the children hashes
    uint64_t left_child_pos = this->m_forest_state.Child(this->m_position, 0),
             right_child_pos = this->m_forest_state.Child(this->m_position, 1);
    Hash left_child_hash, right_child_hash;
    m_forest->Read(left_child_hash, left_child_pos);
    m_forest->Read(right_child_hash, right_child_pos);

    // compute the hash
    Accumulator::ParentHash(m_hash, left_child_hash, right_child_hash);

    // write hash back
    uint8_t row = this->m_forest_state.DetectRow(this->m_position);
    uint64_t offset = this->m_forest_state.RowOffset(this->m_position);
    std::vector<Hash>& rowData = this->m_forest->m_data.at(row);
    rowData[this->m_position - offset] = this->m_hash;
}

NodePtr<Accumulator::Node> RamForest::Node::Parent() const
{
    uint64_t parent_pos = this->m_forest_state.Parent(this->m_position);

    // Check if this node is a root.
    // If so return nullptr becauce roots do not have parents.
    uint8_t row = this->m_forest_state.DetectRow(this->m_position);
    bool row_has_root = this->m_forest_state.HasRoot(row);
    bool is_root = this->m_forest_state.RootPosition(row) == this->m_position;
    if (row_has_root && is_root) {
        return nullptr;
    }

    // Return the parent of this node.
    auto node = NodePtr<RamForest::Node>(m_forest->m_nodepool);
    node->m_forest_state = m_forest_state;
    node->m_forest = m_forest;
    node->m_position = parent_pos;
    return node;
}

// RamForest

bool RamForest::Read(Hash& hash, uint64_t pos) const
{
    uint8_t row = this->m_state.DetectRow(pos);
    uint64_t offset = this->m_state.RowOffset(pos);

    if (row >= this->m_data.size()) {
        // not enough rows
        std::cout << "not enough rows " << pos << std::endl;
        return false;
    }

    std::vector<Hash> rowData = this->m_data.at(row);

    if ((pos - offset) >= rowData.size()) {
        // row not big enough
        std::cout << "row not big enough " << pos << " " << offset << " " << +row << " " << rowData.size() << std::endl;
        return false;
    }

    hash = rowData.at(pos - offset);
    return true;
}

void RamForest::SwapRange(uint64_t from, uint64_t to, uint64_t range)
{
    uint8_t row = this->m_state.DetectRow(from);
    uint64_t offset_from = this->m_state.RowOffset(from);
    uint64_t offset_to = this->m_state.RowOffset(to);
    std::vector<Hash>& rowData = this->m_data.at(row);

    for (uint64_t i = 0; i < range; ++i) {
        std::swap(rowData[(from - offset_from) + i], rowData[(to - offset_to) + i]);

        // Swap postions in the position map if we are on the bottom.
        if (row == 0) std::swap(m_posmap[rowData[(from - offset_from) + i]], m_posmap[rowData[(to - offset_to) + i]]);
    }
}

NodePtr<Accumulator::Node> RamForest::SwapSubTrees(uint64_t from, uint64_t to)
{
    // posA and posB are on the same row
    uint8_t row = this->m_state.DetectRow(from);
    from = this->m_state.LeftDescendant(from, row);
    to = this->m_state.LeftDescendant(to, row);

    for (uint64_t range = 1 << row; range != 0; range >>= 1) {
        this->SwapRange(from, to, range);
        from = this->m_state.Parent(from);
        to = this->m_state.Parent(to);
    }
    auto node = NodePtr<RamForest::Node>(m_nodepool);
    node->m_forest_state = m_state;
    node->m_forest = this;
    node->m_position = to;

    return node;
}

NodePtr<Accumulator::Node> RamForest::MergeRoot(uint64_t parent_pos, Hash parent_hash)
{
    assert(m_roots.size() >= 2);

    m_roots.pop_back();
    m_roots.pop_back();
    // compute row
    uint8_t row = m_state.DetectRow(parent_pos);
    assert(m_data.size() > row);

    // add hash to forest
    m_data.at(row).push_back(parent_hash);

    auto node = NodePtr<RamForest::Node>(m_nodepool);
    // TODO: should we set m_forest_state on the node?
    node->m_forest = this;
    node->m_position = parent_pos;
    node->m_hash = m_data.at(row).back();
    m_roots.push_back(node);

    return m_roots.back();
}

NodePtr<Accumulator::Node> RamForest::NewLeaf(const Leaf& leaf)
{
    // append new hash on row 0 (as a leaf)
    this->m_data.at(0).push_back(leaf.first);

    NodePtr<RamForest::Node> new_root(m_nodepool);
    new_root->m_forest = this;
    new_root->m_position = m_state.m_num_leaves;
    new_root->m_hash = leaf.first;
    m_roots.push_back(new_root);

    m_posmap[leaf.first] = new_root->m_position;

    return this->m_roots.back();
}

void RamForest::FinalizeRemove(const ForestState next_state)
{
    assert(next_state.m_num_leaves <= m_state.m_num_leaves);

    // Remove deleted leaf hashes from the position map.
    for (uint64_t pos = next_state.m_num_leaves; pos < m_state.m_num_leaves; ++pos) {
        Hash to_erase;

        bool ok = Read(to_erase, pos);
        assert(ok);

        m_posmap.erase(to_erase);
    }

    uint64_t num_leaves = next_state.m_num_leaves;
    // Go through each row and resize the row vectors for the next forest state.
    for (uint8_t row = 0; row < this->m_state.NumRows(); ++row) {
        this->m_data.at(row).resize(num_leaves);
        // Compute the number of nodes in the next row.
        num_leaves >>= 1;
    }

    // Compute the positions of the new roots in the current state.
    std::vector<uint64_t> new_positions = this->m_state.RootPositions(next_state.m_num_leaves);

    // Select the new roots.
    std::vector<NodePtr<Accumulator::Node>> new_roots;
    new_roots.reserve(new_positions.size());

    for (uint64_t new_pos : new_positions) {
        auto new_root = NodePtr<RamForest::Node>(m_nodepool);
        new_root->m_forest = this;
        new_root->m_position = new_pos;
        bool ok = Read(new_root->m_hash, new_pos);
        assert(ok);
        new_roots.push_back(new_root);
    }

    this->m_roots = new_roots;
}

bool RamForest::Prove(BatchProof& proof, const std::vector<Hash>& targetHashes) const
{
    // Figure out the positions of the target hashes via the position map.
    std::vector<uint64_t> targets;
    targets.reserve(targetHashes.size());
    for (const Hash& hash : targetHashes) {
        auto posmap_it = m_posmap.find(hash);
        if (posmap_it == m_posmap.end()) {
            // TODO: error
            return false;
        }
        targets.push_back(posmap_it->second);
    }

    // TODO: do sanity checks on the target positions.

    auto proof_positions = this->m_state.ProofPositions(targets);
    std::vector<Hash> proof_hashes(proof_positions.first.size());
    for (int i = 0; i < proof_hashes.size(); i++) {
        Read(proof_hashes[i], proof_positions.first[i]);
    }

    proof = BatchProof(targets, proof_hashes);
    return true;
}

void RamForest::Add(const std::vector<Leaf>& leaves)
{
    // Preallocate data with the required size.
    ForestState next_state(this->m_state.m_num_leaves + leaves.size());
    for (uint8_t row = 0; row <= next_state.NumRows(); ++row) {
        if (row >= this->m_data.size()) {
            m_data.push_back(std::vector<Hash>());
        }

        m_data.at(row).reserve(next_state.m_num_leaves >> row);
    }
    assert(m_data.size() > next_state.NumRows());

    Accumulator::Add(leaves);

    assert(m_posmap.size() == m_state.m_num_leaves);
}
