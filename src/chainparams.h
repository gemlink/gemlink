// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CHAINPARAMS_H
#define BITCOIN_CHAINPARAMS_H

#include "chainparamsbase.h"
#include "consensus/params.h"
#include "primitives/block.h"
#include "protocol.h"

#include <vector>

static const unsigned int DEFAULT_REORG_MN_CHECK = 100;
static const unsigned int MASTERNODE_REORG_CHECK = 20;

struct CDNSSeedData {
    std::string name, host;
    CDNSSeedData(const std::string& strName, const std::string& strHost) : name(strName), host(strHost) {}
};

struct SeedSpec6 {
    uint8_t addr[16];
    uint16_t port;
};

typedef std::map<int, uint256> MapCheckpoints;

struct CCheckpointData {
    MapCheckpoints mapCheckpoints;
    int64_t nTimeLastCheckpoint;
    int64_t nTransactionsLastCheckpoint;
    double fTransactionsPerDay;
};


class CBaseKeyConstants : public KeyConstants
{
public:
    std::string NetworkIDString() const { return strNetworkID; }
    const std::vector<unsigned char>& Base58Prefix(Base58Type type) const { return base58Prefixes[type]; }
    const std::string& Bech32HRP(Bech32Type type) const { return bech32HRPs[type]; }

    std::string strNetworkID;
    std::vector<unsigned char> base58Prefixes[KeyConstants::MAX_BASE58_TYPES];
    std::string bech32HRPs[KeyConstants::MAX_BECH32_TYPES];
};

/**
 * CChainParams defines various tweakable parameters of a given instance of the
 * Bitcoin system. There are three: the main network on which people trade goods
 * and services, the public test network which gets reset from time to time and
 * a regression test mode which is intended for private networks only. It has
 * minimal difficulty to ensure that blocks can be found instantly.
 */
class CChainParams : public KeyConstants
{
public:
    const Consensus::Params& GetConsensus() const { return consensus; }
    const CMessageHeader::MessageStartChars& MessageStart() const { return pchMessageStart; }
    const std::vector<unsigned char>& AlertKey() const { return vAlertPubKey; }
    int GetDefaultPort() const { return nDefaultPort; }

    int EnforceBlockUpgradeMajority() const { return nEnforceBlockUpgradeMajority; }

    const CBlock& GenesisBlock() const { return genesis; }
    /** Make miner wait to have peers to avoid wasting work */
    bool MiningRequiresPeers() const { return fMiningRequiresPeers; }
    /** Default value for -checkmempool and -checkblockindex argument */
    bool DefaultConsistencyChecks() const { return fDefaultConsistencyChecks; }
    /** Policy: Filter transactions that do not match well-defined patterns */
    bool RequireStandard() const { return fRequireStandard; }
    int64_t MaxTipAge() const { return nMaxTipAge; }
    int64_t PruneAfterHeight() const { return nPruneAfterHeight; }

    /** The masternode count that we will allow the see-saw reward payments to be off by */
    int MasternodeCountDrift() const { return nMasternodeCountDrift; }
    std::string CurrencyUnits() const { return strCurrencyUnits; }
    uint32_t BIP44CoinType() const { return bip44CoinType; }
    /** Make miner stop after a block is found. In RPC, don't return until nGenProcLimit blocks are generated */
    bool MineBlocksOnDemand() const { return fMineBlocksOnDemand; }
    /** In the future use NetworkIDString() for RPC fields */
    bool TestnetToBeDeprecatedFieldRPC() const { return fTestnetToBeDeprecatedFieldRPC; }
    /** Return the BIP70 network string (main, test or regtest) */
    std::string NetworkIDString() const { return keyConstants.NetworkIDString(); }
    const std::vector<CDNSSeedData>& DNSSeeds() const { return vSeeds; }
    const std::vector<unsigned char>& Base58Prefix(Base58Type type) const { return keyConstants.Base58Prefix(type); }
    const std::string& Bech32HRP(Bech32Type type) const { return keyConstants.Bech32HRP(type); }
    const std::vector<SeedSpec6>& FixedSeeds() const { return vFixedSeeds; }
    const CCheckpointData& Checkpoints() const { return checkpointData; }
    int PoolMaxTransactions() const { return nPoolMaxTransactions; }
    /** Return the founder's reward address and script for a given block height */
    std::string SporkKey() const { return strSporkKey; }
    std::string ObfuscationPoolDummyAddress() const { return strObfuscationPoolDummyAddress; }
    /** Headers first syncing is disabled */
    bool HeadersFirstSyncingActive() const { return fHeadersFirstSyncingActive; };
    int64_t StartMasternodePayments() const { return nStartMasternodePayments; }
    int64_t Budget_Fee_Confirmations() const { return nBudget_Fee_Confirmations; }
    std::string GetFoundersRewardAddressAtHeight(int height) const;
    CScript GetFoundersRewardScriptAtHeight(int height) const;
    std::string GetFoundersRewardAddressAtIndex(int i) const;

    std::string GetTreasuryRewardAddressAtHeight(int height) const;
    CScript GetTreasuryRewardScriptAtHeight(int height) const;
    std::string GetTreasuryRewardAddressAtIndex(int i) const;

    int GetBlacklistTxSize() const;
    bool IsBlocked(int height, COutPoint outpoint) const;

    std::string GetDevelopersRewardAddressAtHeight(int height) const;
    CScript GetDevelopersRewardScriptAtHeight(int height) const;
    std::string GetDevelopersRewardAddressAtIndex(int i) const;

    /** Enforce coinbase consensus rule in regtest mode */
    void SetRegTestCoinbaseMustBeProtected() { consensus.fCoinbaseMustBeProtected = true; }
    bool GetCoinbaseProtected(int height) const;
    int GetNewTimeRule() const { return newTimeRule; }
    int GetMasternodeProtectionBlock() const { return masternodeProtectionBlock; }
    int GetMasternodeCollateral(int height) const;
    int GetReorgNumber(bool isProtected) const
    {
        if (isProtected) {
            return MASTERNODE_REORG_CHECK;
        } else {
            return DEFAULT_REORG_MN_CHECK;
        }
    }

    int GetmnLockBlocks(int height) const;
    std::map<COutPoint, COutPoint> GetBlackList() const
    {
        return vBlacklistTx;
    }

    std::map<COutPoint, COutPoint> GetWhiteist() const
    {
        return vWhitelistTx;
    }

protected:
    CChainParams() {}

    Consensus::Params consensus;
    CMessageHeader::MessageStartChars pchMessageStart;
    int nEnforceBlockUpgradeMajority = 20;
    //! Raw pub key bytes for the broadcast alert signing key.
    std::vector<unsigned char> vAlertPubKey;
    int nDefaultPort = 0;
    long nMaxTipAge = 0;
    uint64_t nPruneAfterHeight = 0;

    std::vector<CDNSSeedData> vSeeds;
    CBaseKeyConstants keyConstants;
    std::string strCurrencyUnits;
    uint32_t bip44CoinType;
    CBlock genesis;
    std::vector<SeedSpec6> vFixedSeeds;
    bool fMiningRequiresPeers = false;
    bool fDefaultConsistencyChecks = false;
    bool fRequireStandard = false;
    bool fMineBlocksOnDemand = false;
    bool fTestnetToBeDeprecatedFieldRPC = false;
    int nMasternodeCountDrift;
    int nPoolMaxTransactions;
    std::string strSporkKey;
    bool fHeadersFirstSyncingActive;
    std::string strObfuscationPoolDummyAddress;
    int64_t nStartMasternodePayments;
    int64_t nBudget_Fee_Confirmations;
    CCheckpointData checkpointData;
    std::vector<std::string> vFoundersRewardAddress;
    std::vector<std::string> vFoundersRewardAddress2;
    std::vector<std::string> vTreasuryRewardAddress;
    std::vector<std::string> vDevelopersRewardAddress;

    std::map<COutPoint, COutPoint> vBlacklistTx;
    std::map<COutPoint, COutPoint> vWhitelistTx;
    int newTimeRule;
    int masternodeProtectionBlock;
    int masternodeCollateral;
    int masternodeCollateralNew;

    int mnLockBlocks;
    int mnExpirationTime;
};

/**
 * Return the currently selected parameters. This won't change after app
 * startup, except for unit tests.
 */
const CChainParams& Params();

/** Return parameters for the given network. */
CChainParams& Params(CBaseChainParams::Network network);

/** Sets the params returned by Params() to those for the given network. */
void SelectParams(CBaseChainParams::Network network);

/**
 * Looks for -regtest or -testnet and then calls SelectParams as appropriate.
 * Returns false if an invalid combination is given.
 */
bool SelectParamsFromCommandLine();

/**
 * Allows modifying the network upgrade regtest parameters.
 */
void UpdateNetworkUpgradeParameters(Consensus::UpgradeIndex idx, int nActivationHeight);
void UpdateRegtestPow(int64_t nPowMaxAdjustDown, int64_t nPowMaxAdjustUp, uint256 powLimit);
#endif // BITCOIN_CHAINPARAMS_H
