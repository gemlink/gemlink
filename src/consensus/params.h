// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_PARAMS_H
#define BITCOIN_CONSENSUS_PARAMS_H

#include "key_constants.h"
#include "uint256.h"
#include <boost/optional.hpp>
#include <optional>
int32_t MAX_BLOCK_SIZE(int32_t height);

namespace Consensus
{
/**
 * Index into Params.vUpgrades and NetworkUpgradeInfo
 *
 * Being array indices, these MUST be numbered consecutively.
 *
 * The order of these indices MUST match the order of the upgrades on-chain, as
 * several functions depend on the enum being sorted.
 */
enum UpgradeIndex {
    // Sprout must be first
    BASE_SPROUT,
    UPGRADE_TESTDUMMY,
    UPGRADE_OVERWINTER,
    UPGRADE_SAPLING,
    UPGRADE_DIFA,
    UPGRADE_ALFHEIMR,
    UPGRADE_KNOWHERE,
    UPGRADE_WAKANDA,
    UPGRADE_ATLANTIS,
    UPGRADE_MORAG,
    UPGRADE_XANDAR,
    UPGRADE_LATVERIA,

    // NOTE: Also add new upgrades to NetworkUpgradeInfo in upgrades.cpp
    MAX_NETWORK_UPGRADES
};


struct EHparameters {
    unsigned char n;
    unsigned char k;
    unsigned short int nSolSize;
    char pers[9];
};


// EH sol size = (pow(2, k) * ((n/(k+1))+1)) / 8;
static const EHparameters eh200_9 = {200, 9, 1344, "ZcashPoW"};
static const EHparameters eh144_5 = {144, 5, 100, "sngemPoW"};
static const EHparameters eh96_5 = {96, 5, 68, "ZcashPoW"};
static const EHparameters eh48_5 = {48, 5, 36, "ZcashPoW"};
static const unsigned int MAX_EH_PARAM_LIST_LEN = 2;

struct NetworkUpgrade {
    /**
     * The first protocol version which will understand the new consensus rules
     */
    int nProtocolVersion;

    /**
     * Height of the first block for which the new consensus rules will be active
     */
    int nActivationHeight;

    /**
     * Special value for nActivationHeight indicating that the upgrade is always active.
     * This is useful for testing, as it means tests don't need to deal with the activation
     * process (namely, faking a chain of somewhat-arbitrary length).
     *
     * New blockchains that want to enable upgrade rules from the beginning can also use
     * this value. However, additional care must be taken to ensure the genesis block
     * satisfies the enabled rules.
     */
    static constexpr int ALWAYS_ACTIVE = 0;

    /**
     * Special value for nActivationHeight indicating that the upgrade will never activate.
     * This is useful when adding upgrade code that has a testnet activation height, but
     * should remain disabled on mainnet.
     */
    static constexpr int NO_ACTIVATION_HEIGHT = -1;

    /**
     * The hash of the block at height nActivationHeight, if known. This is set manually
     * after a network upgrade activates.
     *
     * We use this in IsInitialBlockDownload to detect whether we are potentially being
     * fed a fake alternate chain. We use NU activation blocks for this purpose instead of
     * the checkpoint blocks, because network upgrades (should) have significantly more
     * scrutiny than regular releases. nMinimumChainWork MUST be set to at least the chain
     * work of this block, otherwise this detection will have false positives.
     */
    std::optional<uint256> hashActivationBlock;
};

/**
 * Parameters that influence chain consensus.
 */
struct Params {
    uint256 hashGenesisBlock;

    bool fCoinbaseMustBeProtected;

    /** Needs to evenly divide MAX_SUBSIDY to avoid rounding errors. */
    int nSubsidySlowStartInterval;
    /**
     * Shift based on a linear ramp for slow start:
     *
     * MAX_SUBSIDY*(t_s/2 + t_r) = MAX_SUBSIDY*t_h  Coin balance
     *              t_s   + t_r  = t_h + t_c        Block balance
     *
     * t_s = nSubsidySlowStartInterval
     * t_r = number of blocks between end of slow start and first halving
     * t_h = nSubsidyHalvingInterval
     * t_c = SubsidySlowStartShift()
     */
    int SubsidySlowStartShift() const { return nSubsidySlowStartInterval / 2; }
    int nSubsidyHalvingInterval;
    int nDelayHalvingBlocks;
    int GetLastFoundersRewardBlockHeight() const
    {
        return nSubsidyHalvingInterval + SubsidySlowStartShift() - 1;
        // return 99999999;
    }

    int GetLastDevelopersRewardBlockHeight() const
    {
        return 99999999;
    }

    int GetLastTreasuryRewardBlockHeight() const
    {
        return vUpgrades[Consensus::UPGRADE_MORAG].nActivationHeight - 1;
    }

    int GetFoundersRewardRepeatInterval() const
    {
        return nSubsidyHalvingInterval + SubsidySlowStartShift() - 1;
    }
    /** Used to check majorities for block version upgrade */
    int nMajorityEnforceBlockUpgrade;
    int nMajorityRejectBlockOutdated;
    int nMajorityWindow;
    NetworkUpgrade vUpgrades[MAX_NETWORK_UPGRADES];
    /** Proof of work parameters */
    uint256 powLimit;
    uint256 powLimitTop;
    std::optional<uint32_t> nPowAllowMinDifficultyBlocksAfterHeight;
    int64_t nPowAveragingWindow;
    int64_t nPowMaxAdjustDown;
    int64_t nPowMaxAdjustUp;
    int64_t nPowTargetSpacing;
    int64_t nTimeshiftPriv;
    int64_t nProposalEstablishmentTime;
    int nMasternodePaymentsStartBlock;
    int nMasternodePaymentsIncreasePeriod; // in blocks
    EHparameters eh_epoch_1 = eh200_9;
    EHparameters eh_epoch_2 = eh144_5;
    unsigned int eh_epoch_1_endtime = 0;   // it's time, not height
    unsigned int eh_epoch_2_starttime = 0; // it's time, not height

    int64_t AveragingWindowTimespan() const { return nPowAveragingWindow * nPowTargetSpacing; }
    int64_t MinActualTimespan() const { return (AveragingWindowTimespan() * (100 - nPowMaxAdjustUp)) / 100; }
    int64_t MaxActualTimespan() const { return (AveragingWindowTimespan() * (100 + nPowMaxAdjustDown)) / 100; }
    EHparameters eh_epoch_1_params() const { return eh_epoch_1; }
    EHparameters eh_epoch_2_params() const { return eh_epoch_2; }
    unsigned int eh_epoch_1_end() const { return eh_epoch_1_endtime; }
    unsigned int eh_epoch_2_start() const { return eh_epoch_2_starttime; }
    bool NetworkUpgradeActive(int nHeight, Consensus::UpgradeIndex idx) const;
    int validEHparameterList(EHparameters* ehparams, unsigned int blocktime) const;

    uint256 nMinimumChainWork;

    /** Parameters for LWMA3 **/
    int64_t nZawyLWMA3AveragingWindow; // N
};

} // namespace Consensus

#endif // BITCOIN_CONSENSUS_PARAMS_H
