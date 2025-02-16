// Copyright (c) 2018 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "consensus/upgrades.h"

/**
 * General information about each network upgrade.
 * Ordered by Consensus::UpgradeIndex.
 */
const struct NUInfo NetworkUpgradeInfo[Consensus::MAX_NETWORK_UPGRADES] = {
    {
        /*.nBranchId =*/0,
        /*.strName =*/"Sprout",
        /*.strInfo =*/"The Zcash network at launch",
    },
    {
        /*.nBranchId =*/0x74736554,
        /*.strName =*/"Test dummy",
        /*.strInfo =*/"Test dummy info",
    },
    {
        /*.nBranchId =*/0x5ba81b19,
        /*.strName =*/"Overwinter",
        /*.strInfo =*/"See https://z.cash/upgrade/overwinter.html for details.",
    },
    {
        /*.nBranchId =*/0x76b809bb,
        /*.strName =*/"Sapling",
        /*.strInfo =*/"See https://z.cash/upgrade/sapling.html for details.",
    },
    {
        /*.nBranchId =*/0,
        /*.strName =*/"Difa",
        /*.strInfo =*/"SnowGem Blocksize and LWMA update",
    },
    {
        /*.nBranchId =*/0x76b809bb,
        /*.strName =*/"Alfheimr",
        /*.strInfo =*/"Masternodevoting, increase tx size to 400k",
    },
    {
        /*.nBranchId =*/0x76b809bb,
        /*.strName =*/"Knowhere",
        /*.strInfo =*/"Update payment reward",
    },
    {
        /*.nBranchId =*/0x76b809bb,
        /*.strName =*/"Wakanda",
        /*.strInfo =*/"Update new block consensus",
    },
    {
        /*.nBranchId =*/0x76b809bb,
        /*.strName =*/"Atlantis",
        /*.strInfo =*/"Dropping private transactions",
    },
    {
        /*.nBranchId =*/0x76b809bb,
        /*.strName =*/"Morag",
        /*.strInfo =*/"Gemlink",
    },
    {
        /*.nBranchId =*/0x76b809bb,
        /*.strName =*/"Xandar",
        /*.strInfo =*/"Locking mn funds",
    },
    {
        /*.nBranchId =*/0x76b809bb,
        /*.strName =*/"Latveria",
        /*.strInfo =*/"Locking blacklist",
    },
    {
        /*.nBranchId =*/0x76b809bb,
        /*.strName =*/"Krakoa",
        /*.strInfo =*/"Decrease min blocktime to 8 seconds",
    },
    {
        /*.nBranchId =*/0x76b809bb,
        /*.strName =*/"Latverion",
        /*.strInfo =*/"Decrease collateral locking time to 7 days",
    }};

const uint32_t SPROUT_BRANCH_ID = NetworkUpgradeInfo[Consensus::BASE_SPROUT].nBranchId;

UpgradeState NetworkUpgradeState(
    int nHeight,
    const Consensus::Params& params,
    Consensus::UpgradeIndex idx)
{
    assert(nHeight >= 0);
    assert(idx >= Consensus::BASE_SPROUT && idx < Consensus::MAX_NETWORK_UPGRADES);
    auto nActivationHeight = params.vUpgrades[idx].nActivationHeight;

    if (nActivationHeight == Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT) {
        return UPGRADE_DISABLED;
    } else if (nHeight >= nActivationHeight) {
        // From ZIP 200:
        //
        // ACTIVATION_HEIGHT
        //     The non-zero block height at which the network upgrade rules will come
        //     into effect, and be enforced as part of the blockchain consensus.
        //
        //     For removal of ambiguity, the block at height ACTIVATION_HEIGHT - 1 is
        //     subject to the pre-upgrade consensus rules, and would be the last common
        //     block in the event of a persistent pre-upgrade branch.
        return UPGRADE_ACTIVE;
    } else {
        return UPGRADE_PENDING;
    }
}

bool NetworkUpgradeActive(
    int nHeight,
    const Consensus::Params& params,
    Consensus::UpgradeIndex idx)
{
    return NetworkUpgradeState(nHeight, params, idx) == UPGRADE_ACTIVE;
}

int CurrentEpoch(int nHeight, const Consensus::Params& params)
{
    for (auto idxInt = Consensus::MAX_NETWORK_UPGRADES - 1; idxInt >= Consensus::BASE_SPROUT; idxInt--) {
        if (NetworkUpgradeActive(nHeight, params, Consensus::UpgradeIndex(idxInt))) {
            return idxInt;
        }
    }
    // Base case
    return Consensus::BASE_SPROUT;
}

uint32_t CurrentEpochBranchId(int nHeight, const Consensus::Params& params)
{
    return NetworkUpgradeInfo[CurrentEpoch(nHeight, params)].nBranchId;
}


uint32_t PrevEpochBranchId(uint32_t currentBranchId, const Consensus::Params& params)
{
    for (int idx = Consensus::BASE_SPROUT + 1; idx < Consensus::MAX_NETWORK_UPGRADES; idx++) {
        if (currentBranchId == NetworkUpgradeInfo[idx].nBranchId) {
            return NetworkUpgradeInfo[idx - 1].nBranchId;
        }
    }
    // Base case
    return NetworkUpgradeInfo[Consensus::BASE_SPROUT].nBranchId;
}

bool IsConsensusBranchId(int branchId)
{
    for (int idx = Consensus::BASE_SPROUT; idx < Consensus::MAX_NETWORK_UPGRADES; idx++) {
        if (branchId == NetworkUpgradeInfo[idx].nBranchId) {
            return true;
        }
    }
    return false;
}

bool IsActivationHeight(
    int nHeight,
    const Consensus::Params& params,
    Consensus::UpgradeIndex idx)
{
    assert(idx >= Consensus::BASE_SPROUT && idx < Consensus::MAX_NETWORK_UPGRADES);

    // Don't count Sprout as an activation height
    if (idx == Consensus::BASE_SPROUT) {
        return false;
    }

    return nHeight >= 0 && nHeight == params.vUpgrades[idx].nActivationHeight;
}

bool IsActivationHeightForAnyUpgrade(
    int nHeight,
    const Consensus::Params& params)
{
    if (nHeight < 0) {
        return false;
    }

    // Don't count Sprout as an activation height
    for (int idx = Consensus::BASE_SPROUT + 1; idx < Consensus::MAX_NETWORK_UPGRADES; idx++) {
        if (nHeight == params.vUpgrades[idx].nActivationHeight)
            return true;
    }

    return false;
}

std::optional<int> NextEpoch(int nHeight, const Consensus::Params& params)
{
    if (nHeight < 0) {
        return std::nullopt;
    }

    // Sprout is never pending
    for (auto idx = Consensus::BASE_SPROUT + 1; idx < Consensus::MAX_NETWORK_UPGRADES; idx++) {
        if (NetworkUpgradeState(nHeight, params, Consensus::UpgradeIndex(idx)) == UPGRADE_PENDING) {
            return idx;
        }
    }

    return std::nullopt;
}

std::optional<int> NextActivationHeight(
    int nHeight,
    const Consensus::Params& params)
{
    auto idx = NextEpoch(nHeight, params);
    if (idx) {
        return params.vUpgrades[idx.value()].nActivationHeight;
    }
    return std::nullopt;
}
