// Copyright (c) 2016 The Zcash developers
// Copyright (c) 2017-2018 The SnowGem developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "key_io.h"
#include "wallet/wallet.h"
#include "zcash/JoinSplit.hpp"
#include "zcash/Note.hpp"
#include "zcash/NoteEncryption.hpp"
#include "zcash/zip32.h"

CWalletTx GetValidReceive(ZCJoinSplit& params,
                          const libzcash::SproutSpendingKey& sk,
                          CAmount value,
                          bool randomInputs,
                          int32_t version = 2);
libzcash::SproutNote GetNote(ZCJoinSplit& params,
                             const libzcash::SproutSpendingKey& sk,
                             const CTransaction& tx,
                             size_t js,
                             size_t n);
CWalletTx GetValidSpend(ZCJoinSplit& params,
                        const libzcash::SproutSpendingKey& sk,
                        const libzcash::SproutNote& note,
                        CAmount value);

CWalletTx GetValidSproutReceive(ZCJoinSplit& params,
                                const libzcash::SproutSpendingKey& sk,
                                CAmount value,
                                bool randomInputs,
                                uint32_t versionGroupId = SAPLING_VERSION_GROUP_ID,
                                int32_t version = SAPLING_TX_VERSION);
CWalletTx GetInvalidCommitmentSproutReceive(ZCJoinSplit& params,
                                            const libzcash::SproutSpendingKey& sk,
                                            CAmount value,
                                            bool randomInputs,
                                            uint32_t versionGroupId = SAPLING_VERSION_GROUP_ID,
                                            int32_t version = SAPLING_TX_VERSION);
libzcash::SproutNote GetSproutNote(ZCJoinSplit& params,
                                   const libzcash::SproutSpendingKey& sk,
                                   const CTransaction& tx,
                                   size_t js,
                                   size_t n);
CWalletTx GetValidSproutSpend(ZCJoinSplit& params,
                              const libzcash::SproutSpendingKey& sk,
                              const libzcash::SproutNote& note,
                              CAmount value);

// Sapling
static const std::string T_SECRET_REGTEST = "cND2ZvtabDbJ1gucx9GWH6XT9kgTAqfb6cotPt5Q5CyxVDhid2EN";

struct TestSaplingNote {
    libzcash::SaplingNote note;
    SaplingMerkleTree tree;
};

const Consensus::Params& RegtestActivateSapling();

void RegtestDeactivateSapling();

const Consensus::Params& RegtestActivateBlossom(bool updatePow, int blossomActivationHeight = Consensus::NetworkUpgrade::ALWAYS_ACTIVE);

void RegtestDeactivateBlossom();

libzcash::SaplingExtendedSpendingKey GetTestMasterSaplingSpendingKey();

CKey AddTestCKeyToKeyStore(CBasicKeyStore& keyStore);

/**
 * Generate a dummy SaplingNote and a SaplingMerkleTree with that note's commitment.
 */
TestSaplingNote GetTestSaplingNote(const libzcash::SaplingPaymentAddress& pa, CAmount value);

CWalletTx GetValidSaplingReceive(const Consensus::Params& consensusParams,
                                 CBasicKeyStore& keyStore,
                                 const libzcash::SaplingExtendedSpendingKey& sk,
                                 CAmount value);