# Converted PART initial-random folding

Effects import stage 113 emits `GPSM PIRN CNST 1` before any expressions. This is required:
the PART loader folds constant vector/color operands as it reads them. With early PIRN,
each real IRND node captures non-foldability at construction; nested CNST/PQAD/PMSC trees
therefore retain their per-particle evaluation. This does not change the IRND hash, seeds,
distribution, lifetime normalization or exposure policy.

The parser mode is scoped per description, starts in retail mode, and restores its caller
on exit. Swoosh and electric descriptions reset inherited PART mode, preserving their prior
semantics; swoosh PIRN still retains its existing trailing position. Retail and non-PIRN PARTs
retain their original folding and load-time RNG consumption order.

**Legacy late-marker converted PARTs require reimport.** Reading PIRN at the end cannot undo
expressions already folded. Updating the executable alone does not fix those assets. Stage113
is shared with the isolated Ice-culling work and must be reconciled with other stage bumps
before integration. Do not mark a mixed baseline-backed diagnostic fixture as fully reimported.

## Actual parser regression (Linux/Ninja)

After building `metroid_prime_port`:

```sh
python3 tools/test_particle_loader.py build/port-ice-cull build/s-ice-fold-20261026
# Optional generated PART: structural extraction retains exact PIRN/PMRT/PMSC bytes and order,
# then the real CParticleDataFactory parses them without resource/disc dependencies.
build/s-ice-fold-20261026/parser-test /absolute/path/to/2BAEA5A6.PART
```

The test links existing game objects with a test main, without starting a renderer/game.
It exercises early/late/absent markers, nested PQAD and PMSC, sixteen particle seeds,
repeat-age stability, original seed99 folding and fourth-roll RNG order, and recursive
factory calls in all retail/converted parent-child combinations. Converter byte-expectation
suites enforce first-marker ordering, uniqueness and unchanged swoosh output.
