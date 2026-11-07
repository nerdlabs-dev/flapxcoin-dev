# Merged mining FlapX with Litecoin: design notes

Status: **implemented and tested on testnet**, on the `merge-mined-development`
branch. **Not active on mainnet** (no activation height set). Merged mining is
a hard fork: every node must upgrade before it activates.

## What's done (testnet)

- AuxPoW support (`src/auxpow.h`, `src/auxpow.cpp`), adapted from Dogecoin
  1.8. The parent coinbase and header use the parent chain's (Litecoin)
  format, because FlapX transactions serialise differently.
- Chain ID **0x0F1A**. Block version: base version in bits 0-7, bit 8 = has
  AuxPoW, chain ID in bits 16-31 (same layout as Namecoin / Dogecoin).
- Active on testnet from block 1. On mainnet the old version rule applies
  unchanged until an activation height is set.
- Pool RPC: `createauxblock <address>`, `submitauxblock <hash> <auxpow>`,
  `getauxblock`. `getblock` shows the parent block of a merge-mined block.
- CI test (`contrib/ci/auxpow-test.py`, run on every build of this branch):
  mines merge-mined testnet blocks like a pool (one block with a two-chain
  tree, as when merge-mining DOGE and FLAPX together), checks that five kinds
  of bad proof are each rejected by the right check, reads a transaction
  back from disk, restarts the node and checks again.

## Still to do before mainnet

1. Decide how PoW and PoS count toward chain weight (A / B / C below).
2. Decide how to handle the difficulty jump at activation.
3. Test with real pool software and a Litecoin testnet node as the parent.
4. Check that no other coin merge-mined with Litecoin uses chain ID 0x0F1A.
5. Run the branch on a copy of a synced mainnet node (it must load and
   verify the existing chain exactly as before).
6. Set the mainnet activation height and release.

## What merged mining is

FlapX and Litecoin use the same proof of work (`scrypt_1024_1_1_256`). With
merged mining ("AuxPoW"), a Litecoin miner works on a Litecoin block whose
coinbase also commits to a FlapX block. If the Litecoin block's scrypt hash
meets FlapX's (much lower) target, the miner submits it to FlapX as proof of
work for the FlapX block. The miner gets FlapX rewards for no extra hashing.

## What has to change in FlapX

1. **AuxPoW data in blocks.** Port the AuxPoW code from Namecoin / Dogecoin
   (Dogecoin added it in 2014 to a codebase of the same era as FlapX):
   - `CAuxPow`: the parent (Litecoin) coinbase transaction, its merkle branch
     to the parent block, the merkle branch of FlapX's hash in the merged
     mining tree, and the parent block header.
   - Serialised after the block header when a version flag is set (e.g.
     `nVersion & 0x100`), and stored with the block on disk.
2. **Block version and chain ID.** Merged-mined blocks carry a chain ID in
   the high bits of `nVersion`, so one parent block can't be reused for two
   chains that share an ID. FlapX needs a chain ID that no other coin merge
   mined with Litecoin uses (Dogecoin uses 0x0062, Namecoin 0x0001).
   **To decide.**
3. **Proof-of-work check.** For an AuxPoW block, check the parent block's
   scrypt hash against FlapX's target, then check the AuxPoW proof: the
   coinbase contains the merged mining marker (`fabe6d6d`) followed by the
   merged mining merkle root, the branches link up, and the chain ID is ours.
   Normal (non-merged) PoW blocks stay valid, as in Dogecoin.
4. **Activation height.** Before it, AuxPoW blocks are invalid; from it on,
   both kinds are valid. Pick a height far enough ahead for everyone to
   upgrade.
5. **RPC for pools.** `getauxblock` (and the newer `createauxblock` /
   `submitauxblock` pair that pool software expects).
6. **FlapX specifics (PoW/PoS hybrid, PPCoin-based):**
   - PoW blocks are unsigned (`CheckBlockSignature` requires an empty
     signature for PoW blocks), so AuxPoW blocks need no signing change.
   - `hashProof` for a PoW block is its PoW hash; for an AuxPoW block it must
     be the parent block's scrypt hash. Check every use of `GetPoWHash()`
     (block checks, stake modifier, checkpoints).
   - The block index and `txleveldb` must store AuxPoW blocks correctly.
     Existing blocks have no AuxPoW, so no resync is needed.

## Consequences to decide on before building it

### PoS and chain weight

The node picks between competing chains by total chain trust, where every
block (PoW and PoS) adds `2^256 / (target + 1)`, i.e. its difficulty.
Litecoin-level hashrate would raise PoW difficulty by several orders of
magnitude. Then:

- PoW blocks would outweigh PoS blocks completely: in practice the chain
  would be secured by PoW alone.
- Staking still works and still earns rewards (PoS blocks are still made
  every ~2 minutes), but it adds very little protection against an attack.
- Whoever merge-mines FlapX (a few large pools) would effectively control
  which chain wins.

Options:
- **A. Leave it.** Simplest; security comes from Litecoin's hashrate via the
  pools.
- **B. Cap or scale PoW block trust**, so PoS keeps a real share of chain
  weight. Consensus change; needs careful design and testing.
- **C. Require both**: e.g. a chain must have a minimum share of PoS blocks
  to be accepted. Strongest for PoS, most complex.

**To decide.**

### Difficulty jump at activation

PoW difficulty currently retargets every PoW block (DigiShield, at most +20%
per block). A jump of ~1,000,000× takes about 62 PoW blocks to catch up,
during which merge-mined blocks would come almost instantly. Options: accept
the short burst, or set a higher minimum PoW difficulty from the activation
height.

### Existing FlapX miners

Small scrypt miners on FlapX would be outcompeted for PoW blocks. They'd
need to mine through a pool that merge-mines FlapX.

## Will it bring more wallets online?

Indirectly, and less than it might seem. Each pool runs one FlapX node, and
pools pay out to addresses (which may be exchange or pool accounts). What it
does bring: FlapX listed on Litecoin pools, more miners holding FLAPX, and
more trading. Those holders only run wallets if they have a reason to, which
for FlapX is staking.

## Testing plan

1. Testnet first. The testnet genesis block is verified (hash
   `cc0e6b7f…de492`; its scrypt proof of work meets its target).
2. Run FlapX testnet nodes plus a Litecoin testnet node, and merge-mine with
   pool software that supports auxiliary chains.
3. Test: normal and merge-mined PoW blocks mixed with PoS blocks, reorgs
   across the activation height, wallets that haven't upgraded (they must
   reject AuxPoW blocks, i.e. split off), and restarts reading AuxPoW blocks
   back from disk.

## Rollout (if it goes ahead)

Release with a far-off activation height, publicise it, get the Litecoin
pools that will list FlapX ready, then watch the activation closely.
