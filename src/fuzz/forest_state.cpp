#include "fuzz.h"
#include "state.h"
#include <cassert>

using namespace utreexo;

FUZZ(forest_state)
{
    FUZZ_CONSUME(uint64_t, num_leaves)
    ForestState state(num_leaves);

    state.NumRoots();
    state.NumRows();

    state.RootPositions();

    FUZZ_CONSUME(uint16_t, num_targets)
    FUZZ_CONSUME_VEC(uint64_t, targets, num_targets);
    state.ProofPositions(targets);
    std::sort(targets.begin(), targets.end());
    state.ProofPositions(targets);
}
