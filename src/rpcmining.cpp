// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2023 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"
#include "main.h"
#include "db.h"
#include "txdb.h"
#include "init.h"
#include "miner.h"
#include "bitcoinrpc.h"
#include "base58.h"

using namespace json_spirit;
using namespace std;

// Key used by getwork/getblocktemplate miners.
// Allocated in InitRPCMining, free'd in ShutdownRPCMining
static CReserveKey* pMiningKey = NULL;

void InitRPCMining()
{
    // getwork/getblocktemplate mining rewards paid here:
    pMiningKey = new CReserveKey(pwalletMain);
}

void ShutdownRPCMining()
{
    delete pMiningKey; pMiningKey = NULL;
}

Value getmininginfo(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getmininginfo\n"
            "Returns an object containing mining-related information.");

    uint64_t nMinWeight = 0, nMaxWeight = 0, nWeight = 0;
    pwalletMain->GetStakeWeight(*pwalletMain, nMinWeight, nMaxWeight, nWeight);

    Object obj, diff, weight;
    obj.push_back(Pair("blocks",        (int)nBestHeight));
    obj.push_back(Pair("currentblocksize",(uint64_t)nLastBlockSize));
    obj.push_back(Pair("currentblocktx",(uint64_t)nLastBlockTx));

    if(nBestHeight >= (!TestNet() ? BLOCK_HEIGHT_FINALPOW: BLOCK_HEIGHT_FINALPOW_TESTNET) )
    {
        diff.push_back(Pair("proof-of-stake",  GetDifficulty(GetLastBlockIndex(pindexBest, true))));
        diff.push_back(Pair("search-interval", (int)nLastCoinStakeSearchInterval));
        obj.push_back(Pair("difficulty",       diff));

        obj.push_back(Pair("netstakeweight",   GetPoSKernelPS()));
        weight.push_back(Pair("minimum",       (uint64_t)nMinWeight));
        weight.push_back(Pair("maximum",       (uint64_t)nMaxWeight));
        weight.push_back(Pair("combined",      (uint64_t)nWeight));
        obj.push_back(Pair("stakeweight",      weight));
        obj.push_back(Pair("stakeinterest",    (uint64_t)GetPIRRewardCoinYear(pwalletMain->GetBalance(),nBestHeight)));
    }
    else
    {
    obj.push_back(Pair("difficulty",    (double)GetDifficulty()));
        obj.push_back(Pair("generate",      GetBoolArg("-gen")));
        obj.push_back(Pair("genproclimit",  (int)GetArg("-genproclimit", -1)));
        obj.push_back(Pair("hashespersec",  gethashespersec(params, false)));
        obj.push_back(Pair("networkhashps", getnetworkhashps(params, false)));
    }

    obj.push_back(Pair("errors",        GetWarnings("statusbar")));
    obj.push_back(Pair("pooledtx",      (uint64_t)mempool.size()));
    obj.push_back(Pair("testnet",       TestNet()));

    return obj;
}

Value getstakinginfo(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getstakinginfo\n"
            "Returns an object containing staking-related information.");

    uint64_t nMinWeight = 0, nMaxWeight = 0, nWeight = 0;
    pwalletMain->GetStakeWeight(*pwalletMain, nMinWeight, nMaxWeight, nWeight);

    uint64_t nNetworkWeight = GetPoSKernelPS();
    bool staking = nLastCoinStakeSearchInterval && nWeight;
    int nExpectedTime = staking ? (nStakeTargetSpacing * 2 * nNetworkWeight / nWeight) : -1;

    Object obj;

    obj.push_back(Pair("enabled", GetBoolArg("-staking", true)));
    obj.push_back(Pair("staking", staking));
    obj.push_back(Pair("errors", GetWarnings("statusbar")));

    obj.push_back(Pair("currentblocksize", (uint64_t)nLastBlockSize));
    obj.push_back(Pair("currentblocktx", (uint64_t)nLastBlockTx));
    obj.push_back(Pair("pooledtx", (uint64_t)mempool.size()));

    obj.push_back(Pair("difficulty", GetDifficulty(GetLastBlockIndex(pindexBest, true))));
    obj.push_back(Pair("search-interval", (int)nLastCoinStakeSearchInterval));

    obj.push_back(Pair("weight", (uint64_t)nWeight));
    obj.push_back(Pair("netstakeweight", (uint64_t)nNetworkWeight));

    obj.push_back(Pair("expectedtime", nExpectedTime));

    return obj;
}

Value getworkex(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "getworkex [data, coinbase]\n"
            "If [data, coinbase] is not specified, returns extended work data.\n"
        );

    if (vNodes.empty())
        throw JSONRPCError(-9, "FlapX is not connected!");

    if (IsInitialBlockDownload())
        throw JSONRPCError(-10, "FlapX is downloading blocks...");

    if (pindexBest->nHeight >= (!TestNet() ? BLOCK_HEIGHT_FINALPOW : BLOCK_HEIGHT_FINALPOW_TESTNET))
        throw JSONRPCError(RPC_MISC_ERROR, "No more PoW blocks");

    typedef map<uint256, pair<CBlock*, CScript> > mapNewBlock_t;
    static mapNewBlock_t mapNewBlock;
    static vector<CBlock*> vNewBlock;
    // static CReserveKey reservekey(pwalletMain);

    if (params.size() == 0)
    {
        // Update block
        static unsigned int nTransactionsUpdatedLast;
        static CBlockIndex* pindexPrev;
        static int64_t nStart;
        static CBlock* pblock;
        if (pindexPrev != pindexBest ||
            (nTransactionsUpdated != nTransactionsUpdatedLast && GetTime() - nStart > 60))
        {
            if (pindexPrev != pindexBest)
            {
                // Deallocate old blocks since they're obsolete now
                mapNewBlock.clear();
                BOOST_FOREACH(CBlock* pblock, vNewBlock)
                    delete pblock;
                vNewBlock.clear();
            }
            nTransactionsUpdatedLast = nTransactionsUpdated;
            pindexPrev = pindexBest;
            nStart = GetTime();

            // Create new block
            pblock = CreateNewBlock(*pMiningKey);
            if (!pblock)
                throw JSONRPCError(-7, "Out of memory");
            vNewBlock.push_back(pblock);
        }

        // Update nTime
        pblock->nTime = max(pindexPrev->GetPastTimeLimit()+1, GetAdjustedTime());
        pblock->nNonce = 0;

        // Update nExtraNonce
        static unsigned int nExtraNonce = 0;
        IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

        // Save
        mapNewBlock[pblock->hashMerkleRoot] = make_pair(pblock, pblock->vtx[0].vin[0].scriptSig);

        // Prebuild hash buffers
        char pmidstate[32];
        char pdata[128];
        char phash1[64];
        FormatHashBuffers(pblock, pmidstate, pdata, phash1);

        uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

        CTransaction coinbaseTx = pblock->vtx[0];
        std::vector<uint256> merkle = pblock->GetMerkleBranch(0);

        Object result;
        result.push_back(Pair("data",     HexStr(BEGIN(pdata), END(pdata))));
        result.push_back(Pair("target",   HexStr(BEGIN(hashTarget), END(hashTarget))));

        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << coinbaseTx;
        result.push_back(Pair("coinbase", HexStr(ssTx.begin(), ssTx.end())));

        Array merkle_arr;
        printf("DEBUG: merkle size %" PRIszu "\n", merkle.size());

        BOOST_FOREACH(uint256 merkleh, merkle) {
            printf("%s\n", merkleh.ToString().c_str());
            merkle_arr.push_back(HexStr(BEGIN(merkleh), END(merkleh)));
        }

        result.push_back(Pair("merkle", merkle_arr));


        return result;
    }
    else
    {
        // Parse parameters
        vector<unsigned char> vchData = ParseHex(params[0].get_str());
        vector<unsigned char> coinbase;

        if(params.size() == 2)
            coinbase = ParseHex(params[1].get_str());

        if (vchData.size() != 128)
            throw JSONRPCError(-8, "Invalid parameter");

        CBlock* pdata = (CBlock*)&vchData[0];

        // Byte reverse
        for (int i = 0; i < 128/4; i++)
            ((unsigned int*)pdata)[i] = ByteReverse(((unsigned int*)pdata)[i]);

        // Get saved block
        if (!mapNewBlock.count(pdata->hashMerkleRoot))
            return false;
        CBlock* pblock = mapNewBlock[pdata->hashMerkleRoot].first;

        pblock->nTime = pdata->nTime;
        pblock->nNonce = pdata->nNonce;

        if(coinbase.size() == 0)
            pblock->vtx[0].vin[0].scriptSig = mapNewBlock[pdata->hashMerkleRoot].second;
        else
            CDataStream(coinbase, SER_NETWORK, PROTOCOL_VERSION) >> pblock->vtx[0]; // FIXME - HACK!

        pblock->hashMerkleRoot = pblock->BuildMerkleTree();

        return CheckWork(pblock, *pwalletMain, *pMiningKey);
    }
}


Value getwork(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "getwork [data]\n"
            "If [data] is not specified, returns formatted hash data to work on:\n"
            "  \"midstate\" : precomputed hash state after hashing the first half of the data (DEPRECATED)\n" // deprecated
            "  \"data\" : block data\n"
            "  \"hash1\" : formatted hash buffer for second hash (DEPRECATED)\n" // deprecated
            "  \"target\" : little endian hash target\n"
            "If [data] is specified, tries to solve the block and returns true if it was successful.");

    if (!TestNet() && vNodes.empty())
        throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "FlapX is not connected!");

    if (IsInitialBlockDownload())
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "FlapX is downloading blocks...");

    if (pindexBest->nHeight >= (!TestNet() ? BLOCK_HEIGHT_FINALPOW : BLOCK_HEIGHT_FINALPOW_TESTNET))
        throw JSONRPCError(RPC_MISC_ERROR, "No more PoW blocks");

    typedef map<uint256, pair<CBlock*, CScript> > mapNewBlock_t;
    static mapNewBlock_t mapNewBlock;    // FIXME: thread safety
    static vector<CBlock*> vNewBlock;
    // static CReserveKey reservekey(pwalletMain);

    if (params.size() == 0)
    {
        // Update block
        static unsigned int nTransactionsUpdatedLast;
        static CBlockIndex* pindexPrev;
        static int64_t nStart;
        static CBlock* pblock;
        if (pindexPrev != pindexBest ||
            (nTransactionsUpdated != nTransactionsUpdatedLast && GetTime() - nStart > 60))
        {
            if (pindexPrev != pindexBest)
            {
                // Deallocate old blocks since they're obsolete now
                mapNewBlock.clear();
                BOOST_FOREACH(CBlock* pblock, vNewBlock)
                    delete pblock;
                vNewBlock.clear();
            }

            // Clear pindexPrev so future getworks make a new block, despite any failures from here on
            pindexPrev = NULL;

            // Store the pindexBest used before CreateNewBlock, to avoid races
            nTransactionsUpdatedLast = nTransactionsUpdated;
            CBlockIndex* pindexPrevNew = pindexBest;
            nStart = GetTime();

            // Create new block
            pblock = CreateNewBlock(*pMiningKey);
            if (!pblock)
                throw JSONRPCError(RPC_OUT_OF_MEMORY, "Out of memory");
            vNewBlock.push_back(pblock);

            // Need to update only after we know CreateNewBlock succeeded
            pindexPrev = pindexPrevNew;
        }

        // Update nTime
        pblock->UpdateTime(pindexPrev);
        pblock->nNonce = 0;

        // Update nExtraNonce
        static unsigned int nExtraNonce = 0;
        IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

        // Save
        mapNewBlock[pblock->hashMerkleRoot] = make_pair(pblock, pblock->vtx[0].vin[0].scriptSig);

        // Pre-build hash buffers
        char pmidstate[32];
        char pdata[128];
        char phash1[64];
        FormatHashBuffers(pblock, pmidstate, pdata, phash1);

        uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

        Object result;
        result.push_back(Pair("midstate", HexStr(BEGIN(pmidstate), END(pmidstate)))); // deprecated
        result.push_back(Pair("data",     HexStr(BEGIN(pdata), END(pdata))));
        result.push_back(Pair("hash1",    HexStr(BEGIN(phash1), END(phash1)))); // deprecated
        result.push_back(Pair("target",   HexStr(BEGIN(hashTarget), END(hashTarget))));
        result.push_back(Pair("algorithm", "scrypt:1024,1,1"));  // Litecoin: specify that we should use the scrypt algorithm
        return result;
    }
    else
    {
        // Parse parameters
        vector<unsigned char> vchData = ParseHex(params[0].get_str());
        if (vchData.size() != 128)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter");
        CBlock* pdata = (CBlock*)&vchData[0];

        // Byte reverse
        for (int i = 0; i < 128/4; i++)
            ((unsigned int*)pdata)[i] = ByteReverse(((unsigned int*)pdata)[i]);

        // Get saved block
        if (!mapNewBlock.count(pdata->hashMerkleRoot))
            return false;
        CBlock* pblock = mapNewBlock[pdata->hashMerkleRoot].first;

        pblock->nTime = pdata->nTime;
        pblock->nNonce = pdata->nNonce;
        pblock->vtx[0].vin[0].scriptSig = mapNewBlock[pdata->hashMerkleRoot].second;
        pblock->hashMerkleRoot = pblock->BuildMerkleTree();

        return CheckWork(pblock, *pwalletMain, *pMiningKey);
    }
}


Value getblocktemplate(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "getblocktemplate [params]\n"
            "If [params] does not contain a \"data\" key, returns data needed to construct a block to work on:\n"
            "  \"version\" : block version\n"
            "  \"previousblockhash\" : hash of current highest block\n"
            "  \"transactions\" : contents of non-coinbase transactions that should be included in the next block\n"
            "  \"coinbaseaux\" : data that should be included in coinbase\n"
            "  \"coinbasevalue\" : maximum allowable input to coinbase transaction, including the generation award and transaction fees\n"
            "  \"target\" : hash target\n"
            "  \"mintime\" : minimum timestamp appropriate for next block\n"
            "  \"curtime\" : current timestamp\n"
            "  \"mutable\" : list of ways the block template may be changed\n"
            "  \"noncerange\" : range of valid nonces\n"
            "  \"sigoplimit\" : limit of sigops in blocks\n"
            "  \"sizelimit\" : limit of block size\n"
            "  \"bits\" : compressed target of next block\n"
            "  \"height\" : height of the next block\n"
            "If [params] does contain a \"data\" key, tries to solve the block and returns null if it was successful (and \"rejected\" if not)\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.");

    std::string strMode = "template";
    if (params.size() > 0)
    {
        const Object& oparam = params[0].get_obj();

        const Value& modeval = find_value(oparam, "mode");
        if (modeval.type() == str_type)
            strMode = modeval.get_str();
        else if (modeval.type() == null_type)
        {
            /* Do nothing */
        }
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");
    }

    if (strMode != "template")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");

    if (!TestNet() && vNodes.empty())
        throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "FlapX is not connected!");

    if (IsInitialBlockDownload())
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "FlapX is downloading blocks...");


    if (pindexBest->nHeight >= (!TestNet() ? BLOCK_HEIGHT_FINALPOW : BLOCK_HEIGHT_FINALPOW_TESTNET))
        throw JSONRPCError(RPC_MISC_ERROR, "No more PoW blocks");

    // static CReserveKey reservekey(pwalletMain);

    // Update block
    static unsigned int nTransactionsUpdatedLast;
    static CBlockIndex* pindexPrev;
    static int64_t nStart;
    static CBlock* pblock;
    if (pindexPrev != pindexBest ||
        (nTransactionsUpdated != nTransactionsUpdatedLast && GetTime() - nStart > 5))
    {
        // Clear pindexPrev so future calls make a new block, despite any failures from here on
        pindexPrev = NULL;

        // Store the pindexBest used before CreateNewBlock, to avoid races
        nTransactionsUpdatedLast = nTransactionsUpdated;
        CBlockIndex* pindexPrevNew = pindexBest;
        nStart = GetTime();

        // Create new block
        if(pblock)
        {
            delete pblock;
            pblock = NULL;
        }
        pblock = CreateNewBlock(*pMiningKey);
        if (!pblock)
            throw JSONRPCError(RPC_OUT_OF_MEMORY, "Out of memory");

        // Need to update only after we know CreateNewBlock succeeded
        pindexPrev = pindexPrevNew;
    }

    // Update nTime
    pblock->UpdateTime(pindexPrev);
    pblock->nNonce = 0;

    Array transactions;
    map<uint256, int64_t> setTxIndex;
    int i = 0;
    CTxDB txdb("r");
    BOOST_FOREACH (CTransaction& tx, pblock->vtx)
    {
        uint256 txHash = tx.GetHash();
        setTxIndex[txHash] = i++;

        if (tx.IsCoinBase() || tx.IsCoinStake())
            continue;

        Object entry;

        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << tx;
        entry.push_back(Pair("data", HexStr(ssTx.begin(), ssTx.end())));

        entry.push_back(Pair("hash", txHash.GetHex()));

        MapPrevTx mapInputs;
        map<uint256, CTxIndex> mapUnused;
        bool fInvalid = false;
        if (tx.FetchInputs(txdb, mapUnused, false, false, mapInputs, fInvalid))
        {
            entry.push_back(Pair("fee", (int64_t)(tx.GetValueIn(mapInputs) - tx.GetValueOut())));

            Array deps;
            BOOST_FOREACH (MapPrevTx::value_type& inp, mapInputs)
            {
                if (setTxIndex.count(inp.first))
                    deps.push_back(setTxIndex[inp.first]);
            }
            entry.push_back(Pair("depends", deps));

            int64_t nSigOps = GetLegacySigOpCount(tx);
            nSigOps += GetP2SHSigOpCount(tx, mapInputs);
            entry.push_back(Pair("sigops", nSigOps));
        }

        transactions.push_back(entry);
    }

    Object aux;
    aux.push_back(Pair("flags", HexStr(COINBASE_FLAGS.begin(), COINBASE_FLAGS.end())));

    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

    static Array aMutable;
    if (aMutable.empty())
    {
        aMutable.push_back("time");
        aMutable.push_back("transactions");
        aMutable.push_back("prevblock");
    }

    Object result;
    result.push_back(Pair("version", pblock->nVersion));
    result.push_back(Pair("previousblockhash", pblock->hashPrevBlock.GetHex()));
    result.push_back(Pair("transactions", transactions));
    result.push_back(Pair("coinbaseaux", aux));
    result.push_back(Pair("coinbasevalue", (int64_t)pblock->vtx[0].vout[0].nValue));
    result.push_back(Pair("target", hashTarget.GetHex()));
    result.push_back(Pair("mintime", (int64_t)pindexPrev->GetPastTimeLimit()+1));
    result.push_back(Pair("mutable", aMutable));
    result.push_back(Pair("noncerange", "00000000ffffffff"));
    result.push_back(Pair("sigoplimit", (int64_t)MAX_BLOCK_SIGOPS));
    result.push_back(Pair("sizelimit", (int64_t)MAX_BLOCK_SIZE));
    result.push_back(Pair("curtime", (int64_t)pblock->nTime));
    result.push_back(Pair("bits", HexBits(pblock->nBits)));
    result.push_back(Pair("height", (int64_t)(pindexPrev->nHeight+1)));

    return result;
}

Value submitblock(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "submitblock <hex data> [optional-params-obj]\n"
            "[optional-params-obj] parameter is currently ignored.\n"
            "Attempts to submit new block to network.\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.");

    vector<unsigned char> blockData(ParseHex(params[0].get_str()));
    CDataStream ssBlock(blockData, SER_NETWORK, PROTOCOL_VERSION);
    CBlock block;
    try {
        ssBlock >> block;
    }
    catch (std::exception &e) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");
    }

    if (params.size() > 1)
    {
        const Object& oparam = params[1].get_obj();

        const Value& coinstake_v = find_value(oparam, "coinstake");
        if (coinstake_v.type() == str_type)
        {
            vector<unsigned char> txData(ParseHex(coinstake_v.get_str()));
            CDataStream ssTx(txData, SER_NETWORK, PROTOCOL_VERSION);
            CTransaction txCoinStake;
            try {
                ssTx >> txCoinStake;
            }
            catch (std::exception &e) {
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Coinstake decode failed");
            }

            block.vtx.insert(block.vtx.begin() + 1, txCoinStake);
            block.hashMerkleRoot = block.BuildMerkleTree();

            CPubKey pubkey;
            if (!pMiningKey->GetReservedKey(pubkey))
                throw JSONRPCError(RPC_MISC_ERROR, "GetReservedKey failed");

            CKey key;
            if (!pwalletMain->GetKey(pubkey.GetID(), key))
                throw JSONRPCError(RPC_MISC_ERROR, "GetKey failed");

            if (!key.Sign(block.GetHash(), block.vchBlockSig))
                throw JSONRPCError(RPC_MISC_ERROR, "Sign failed");
        }
    }

    bool fAccepted = ProcessBlock(NULL, &block);
    if (!fAccepted)
        return "rejected";

    return Value::null;
}


// ---------------------------------------------------------------------------
// Merged mining (AuxPoW) for pools: createauxblock / submitauxblock and the
// older getauxblock. A pool asks for a FlapX block, puts its hash in the
// coinbase of the parent (Litecoin) block it is mining, and when a parent
// block hash meets FlapX's target, submits the AuxPoW proof here.
// ---------------------------------------------------------------------------

// Blocks handed out to pools, by block hash (cleared when the tip changes)
static map<uint256, CBlock*> mapAuxBlocks;
static vector<CBlock*> vAuxBlocks;

static void CheckAuxMiningAllowed()
{
    if (!TestNet() && vNodes.empty())
        throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "FlapX is not connected!");
    if (IsInitialBlockDownload())
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "FlapX is downloading blocks...");
    if (pindexBest->nHeight + 1 < Params().AuxpowStartHeight())
        throw JSONRPCError(RPC_MISC_ERROR, strprintf("Merged mining starts at block %d", Params().AuxpowStartHeight()));
    if (pindexBest->nHeight >= (!TestNet() ? BLOCK_HEIGHT_FINALPOW : BLOCK_HEIGHT_FINALPOW_TESTNET))
        throw JSONRPCError(RPC_MISC_ERROR, "No more PoW blocks");
}

// A new block to merge-mine, paying to scriptPayout (or the wallet's mining
// key if it's empty). Reused until the tip changes, or for up to a minute
// while new transactions arrive.
static CBlock* GetAuxBlock(const CScript& scriptPayout)
{
    static CBlockIndex* pindexPrev = NULL;
    static unsigned int nTransactionsUpdatedLast = 0;
    static int64_t nStart = 0;
    static map<CScript, CBlock*> mapCurrent;   // newest block per payout script

    if (pindexPrev != pindexBest)
    {
        mapAuxBlocks.clear();
        mapCurrent.clear();
        BOOST_FOREACH(CBlock* pblock, vAuxBlocks)
            delete pblock;
        vAuxBlocks.clear();
    }
    else if (nTransactionsUpdated != nTransactionsUpdatedLast && GetTime() - nStart > 60)
        mapCurrent.clear();   // make a new block with the new transactions

    if (mapCurrent.count(scriptPayout))
        return mapCurrent[scriptPayout];

    CBlockIndex* pindexPrevNew = pindexBest;
    nTransactionsUpdatedLast = nTransactionsUpdated;
    nStart = GetTime();

    CBlock* pblock = CreateNewBlock(*pMiningKey);
    if (!pblock)
        throw JSONRPCError(RPC_OUT_OF_MEMORY, "Out of memory");
    if (!scriptPayout.empty())
        pblock->vtx[0].vout[0].scriptPubKey = scriptPayout;

    // Merge-mined block: base version 2 (as all FlapX PoW blocks), FlapX's
    // chain ID and the AuxPoW flag (the AuxPoW itself comes with the submit)
    pblock->nVersion = 2 | (Params().AuxpowChainId() * BLOCK_VERSION_CHAIN_START);
    pblock->UpdateTime(pindexPrevNew);
    pblock->nNonce = 0;
    static unsigned int nExtraNonce = 0;
    IncrementExtraNonce(pblock, pindexPrevNew, nExtraNonce);
    pblock->SetAuxPow(new CAuxPow());

    pindexPrev = pindexPrevNew;
    vAuxBlocks.push_back(pblock);
    mapCurrent[scriptPayout] = pblock;
    mapAuxBlocks[pblock->GetHash()] = pblock;
    return pblock;
}

static Object AuxBlockToJSON(const CBlock* pblock)
{
    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
    Object result;
    result.push_back(Pair("hash", pblock->GetHash().GetHex()));
    result.push_back(Pair("chainid", pblock->GetChainID()));
    result.push_back(Pair("previousblockhash", pblock->hashPrevBlock.GetHex()));
    result.push_back(Pair("coinbasevalue", (boost::int64_t)pblock->vtx[0].vout[0].nValue));
    result.push_back(Pair("bits", strprintf("%08x", pblock->nBits)));
    result.push_back(Pair("height", pindexBest->nHeight + 1));
    // target as little-endian hex (as getauxblock in Namecoin / Dogecoin)
    result.push_back(Pair("target", HexStr(BEGIN(hashTarget), END(hashTarget))));
    result.push_back(Pair("_target", HexStr(BEGIN(hashTarget), END(hashTarget))));
    return result;
}

static bool SubmitAuxBlock(const string& strHash, const string& strAuxPow)
{
    uint256 hash;
    hash.SetHex(strHash);
    if (!mapAuxBlocks.count(hash))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Block hash unknown (stale or never handed out)");

    vector<unsigned char> vchAuxPow = ParseHex(strAuxPow);
    CDataStream ss(vchAuxPow, SER_NETWORK, PROTOCOL_VERSION);
    CAuxPow* pow = new CAuxPow();
    try {
        ss >> *pow;
    } catch (std::exception& e) {
        delete pow;
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "AuxPoW decode failed");
    }

    CBlock* pblock = mapAuxBlocks[hash];
    pblock->SetAuxPow(pow);
    return CheckWork(pblock, *pwalletMain, *pMiningKey);
}

Value createauxblock(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "createauxblock <address>\n"
            "Creates a block to merge-mine, paying to <address>. Returns:\n"
            "  \"hash\" : the block hash to put in the parent block's coinbase\n"
            "  \"chainid\" : FlapX's chain ID\n"
            "  \"previousblockhash\", \"coinbasevalue\", \"bits\", \"height\"\n"
            "  \"_target\" : the target the parent block hash must meet (little endian)\n"
            "Submit the solution with submitauxblock.");

    CBitcoinAddress address(params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid FlapX address");
    CScript scriptPayout;
    scriptPayout.SetDestination(address.Get());

    CheckAuxMiningAllowed();
    return AuxBlockToJSON(GetAuxBlock(scriptPayout));
}

Value submitauxblock(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 2)
        throw runtime_error(
            "submitauxblock <hash> <auxpow>\n"
            "Submits a solved merge-mined block: <hash> from createauxblock, <auxpow>\n"
            "the serialised AuxPoW (hex). Returns true if the block was accepted.");

    return SubmitAuxBlock(params[0].get_str(), params[1].get_str());
}

Value getauxblock(const Array& params, bool fHelp)
{
    if (fHelp || (params.size() != 0 && params.size() != 2))
        throw runtime_error(
            "getauxblock [<hash> <auxpow>]\n"
            "Without arguments: creates a block to merge-mine, paying to this wallet\n"
            "(same result as createauxblock). With <hash> and <auxpow>: submits the\n"
            "solution and returns true if the block was accepted.");

    if (params.size() == 0)
    {
        CheckAuxMiningAllowed();
        return AuxBlockToJSON(GetAuxBlock(CScript()));
    }
    return SubmitAuxBlock(params[0].get_str(), params[1].get_str());
}
