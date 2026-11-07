// Copyright (c) 2011 Vince Durham
// Copyright (c) 2013-2014 Dogecoin Developers
// Copyright (c) 2026 FlapX Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Merged mining ("AuxPoW"): a FlapX proof-of-work block can prove its work
// with a Litecoin (parent chain) block whose coinbase commits to the FlapX
// block's hash. Adapted from Dogecoin 1.8 (auxpow.h/.cpp).
//
// The parent coinbase and header are in the parent chain's (Bitcoin/Litecoin)
// format, which differs from FlapX's own transactions (PPCoin time stamp and
// the FlapX comment field), so they have their own classes here.
//
// This header is included from main.h, just before CBlock (which holds an
// AuxPoW), and uses CTxIn / CTxOut from there: include main.h, not this.
#ifndef FLAPX_AUXPOW_H
#define FLAPX_AUXPOW_H

/** A transaction in the parent chain's format (version, inputs, outputs, lock time) */
class CParentTransaction
{
public:
    int nVersion;
    std::vector<CTxIn> vin;
    std::vector<CTxOut> vout;
    unsigned int nLockTime;

    CParentTransaction() { SetNull(); }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(this->nVersion);
        nVersion = this->nVersion;
        READWRITE(vin);
        READWRITE(vout);
        READWRITE(nLockTime);
    )

    void SetNull()
    {
        nVersion = 1;
        vin.clear();
        vout.clear();
        nLockTime = 0;
    }

    uint256 GetHash() const { return SerializeHash(*this); }
};

/** An 80-byte block header in the parent chain's format */
class CParentBlockHeader
{
public:
    int nVersion;
    uint256 hashPrevBlock;
    uint256 hashMerkleRoot;
    unsigned int nTime;
    unsigned int nBits;
    unsigned int nNonce;

    CParentBlockHeader() { SetNull(); }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(this->nVersion);
        nVersion = this->nVersion;
        READWRITE(hashPrevBlock);
        READWRITE(hashMerkleRoot);
        READWRITE(nTime);
        READWRITE(nBits);
        READWRITE(nNonce);
    )

    void SetNull()
    {
        nVersion = 0;
        hashPrevBlock = 0;
        hashMerkleRoot = 0;
        nTime = 0;
        nBits = 0;
        nNonce = 0;
    }

    uint256 GetHash() const { return Hash(BEGIN(nVersion), END(nNonce)); }

    /** scrypt hash of the header: what must meet FlapX's target */
    uint256 GetPoWHash() const;

    /** Chain ID in the parent header's version (0 for Litecoin) */
    int GetChainID() const { return nVersion / BLOCK_VERSION_CHAIN_START; }
};

/** Proof that a FlapX block's hash is committed to in a parent chain block */
class CAuxPow
{
public:
    // The parent block's coinbase and its merkle branch to the parent header
    CParentTransaction coinbaseTx;
    uint256 hashBlock;                       // unused (kept for the standard format)
    std::vector<uint256> vMerkleBranch;
    int nIndex;                              // must be 0: the coinbase
    // Merkle branch from the FlapX block hash to the merged mining root that
    // is in the parent coinbase, and FlapX's slot in that tree
    std::vector<uint256> vChainMerkleBranch;
    int nChainIndex;
    CParentBlockHeader parentBlock;

    CAuxPow() : hashBlock(0), nIndex(0), nChainIndex(0) {}

    // Standard AuxPoW format (as built by merged mining pool software):
    // coinbase tx, block hash, merkle branch, index, chain merkle branch,
    // chain index, parent header
    IMPLEMENT_SERIALIZE
    (
        READWRITE(coinbaseTx);
        READWRITE(hashBlock);
        READWRITE(vMerkleBranch);
        READWRITE(nIndex);
        READWRITE(vChainMerkleBranch);
        READWRITE(nChainIndex);
        READWRITE(parentBlock);
    )

    /** Check that this proves work for the FlapX block hash hashAuxBlock with
     *  chain ID nChainID (not counting the proof of work itself) */
    bool Check(const uint256& hashAuxBlock, int nChainID) const;

    /** The parent block's scrypt hash, checked against FlapX's target */
    uint256 GetParentBlockPoWHash() const { return parentBlock.GetPoWHash(); }
};

/** Expected slot of a chain in the merged mining tree (standard formula) */
int GetExpectedAuxChainIndex(int nNonce, int nChainID, int nMerkleHeight);

#endif // FLAPX_AUXPOW_H
