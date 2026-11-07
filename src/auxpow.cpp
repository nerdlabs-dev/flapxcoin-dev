// Copyright (c) 2011 Vince Durham
// Copyright (c) 2013-2014 Dogecoin Developers
// Copyright (c) 2026 FlapX Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"
#include "scrypt.h"

using namespace std;

// Marks the merged mining root in the parent coinbase: "\xfa\xbe" "mm"
static const unsigned char pchMergedMiningHeader[] = { 0xfa, 0xbe, 'm', 'm' };

uint256 CParentBlockHeader::GetPoWHash() const
{
    uint256 thash;
    scrypt_1024_1_1_256(BEGIN(nVersion), BEGIN(thash));
    return thash;
}

int GetExpectedAuxChainIndex(int nNonce, int nChainID, int nMerkleHeight)
{
    // Pseudo-random but fixed slot for a given nonce, chain and tree size, so
    // the same parent work can't be used twice for one chain
    unsigned int rand = nNonce;
    rand = rand * 1103515245 + 12345;
    rand += nChainID;
    rand = rand * 1103515245 + 12345;
    return rand % (1u << nMerkleHeight);
}

bool CAuxPow::Check(const uint256& hashAuxBlock, int nChainID) const
{
    if (nIndex != 0)
        return error("CAuxPow::Check() : AuxPoW is not a coinbase (generate) transaction");

    if (parentBlock.GetChainID() == nChainID)
        return error("CAuxPow::Check() : parent block has our chain ID");

    if (vChainMerkleBranch.size() > 30)
        return error("CAuxPow::Check() : chain merkle branch too long");

    if (coinbaseTx.vin.empty())
        return error("CAuxPow::Check() : parent coinbase has no inputs");

    // The coinbase must be in the parent block (first transaction)
    if (CBlock::CheckMerkleBranch(coinbaseTx.GetHash(), vMerkleBranch, nIndex) != parentBlock.hashMerkleRoot)
        return error("CAuxPow::Check() : parent merkle root incorrect");

    // The merged mining root (FlapX block hash plus chain branch) must be in
    // the coinbase, right after the merged mining marker
    const uint256 hashRoot = CBlock::CheckMerkleBranch(hashAuxBlock, vChainMerkleBranch, nChainIndex);
    vector<unsigned char> vchRootHash(hashRoot.begin(), hashRoot.end());
    std::reverse(vchRootHash.begin(), vchRootHash.end()); // as written in the coinbase

    const CScript& script = coinbaseTx.vin[0].scriptSig;
    const unsigned char* pHeaderBegin = pchMergedMiningHeader;
    const unsigned char* pHeaderEnd = pchMergedMiningHeader + sizeof(pchMergedMiningHeader);

    CScript::const_iterator pcHead = std::search(script.begin(), script.end(), pHeaderBegin, pHeaderEnd);
    if (pcHead == script.end())
        return error("CAuxPow::Check() : merged mining marker missing from parent coinbase");
    // Only one merged mining root is allowed
    if (std::search(pcHead + 1, script.end(), pHeaderBegin, pHeaderEnd) != script.end())
        return error("CAuxPow::Check() : more than one merged mining marker in parent coinbase");

    CScript::const_iterator pc = std::search(script.begin(), script.end(), vchRootHash.begin(), vchRootHash.end());
    if (pc == script.end())
        return error("CAuxPow::Check() : merged mining root missing from parent coinbase");
    if (pcHead + sizeof(pchMergedMiningHeader) != pc)
        return error("CAuxPow::Check() : merged mining marker is not just before the merged mining root");

    // After the root: tree size and nonce (4 bytes each, little endian)
    pc += vchRootHash.size();
    if (script.end() - pc < 8)
        return error("CAuxPow::Check() : merged mining tree size and nonce missing from parent coinbase");

    unsigned int nSize = (unsigned int)pc[0] | ((unsigned int)pc[1] << 8) | ((unsigned int)pc[2] << 16) | ((unsigned int)pc[3] << 24);
    const unsigned int nMerkleHeight = vChainMerkleBranch.size();
    if (nSize != (1u << nMerkleHeight))
        return error("CAuxPow::Check() : merged mining tree size does not match the chain merkle branch");

    int nNonce = (int)((unsigned int)pc[4] | ((unsigned int)pc[5] << 8) | ((unsigned int)pc[6] << 16) | ((unsigned int)pc[7] << 24));
    if (nChainIndex != GetExpectedAuxChainIndex(nNonce, nChainID, nMerkleHeight))
        return error("CAuxPow::Check() : wrong slot in the merged mining tree");

    return true;
}
