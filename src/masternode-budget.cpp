// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2017-2018 The SnowGem developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "init.h"
#include "main.h"

#include "addrman.h"
#include "masternode-budget.h"
#include "masternode-sync.h"
#include "masternode.h"
#include "masternodeman.h"
#include "util.h"
#include <boost/filesystem.hpp>


#include "key_io.h"

CBudgetManager budget;
CCriticalSection cs_budget;

std::map<uint256, int64_t> askedForSourceProposalOrBudget;
std::vector<CBudgetProposalBroadcast> vecImmatureBudgetProposals;
std::vector<CFinalizedBudgetBroadcast> vecImmatureFinalizedBudgets;

int nSubmittedFinalBudget;

int GetBudgetPaymentCycleBlocks()
{
    // Amount of blocks in a months period of time (using 1 minutes per block) = (60*24*30)
    if (NetworkIdFromCommandLine() == CBaseChainParams::MAIN)
        return (60 * 24 * 30); // 1 month
    // for testing purposes

    return 144; // ten times per day
}

bool IsBudgetCollateralValid(const uint256& nTxCollateralHash, const uint256& nExpectedHash, std::string& strError, int64_t& nTime, int& nConf, bool fBudgetFinalization)
{
    CTransaction txCollateral;
    uint256 nBlockHash;
    if (!GetTransaction(nTxCollateralHash, txCollateral, Params().GetConsensus(), nBlockHash, true)) {
        strError = strprintf("Can't find collateral tx %s", txCollateral.ToString());
        LogPrint("masternode", "CBudgetProposalBroadcast::IsBudgetCollateralValid - %s\n", strError);
        return false;
    }

    if (txCollateral.vout.size() < 1)
        return false;
    if (txCollateral.nLockTime > (unsigned int)chainActive.Height())
        return false;

    CScript findScript;
    findScript << OP_RETURN << ToByteVector(nExpectedHash);

    bool foundOpReturn = false;
    for (const CTxOut o : txCollateral.vout) {
        if (!o.scriptPubKey.IsNormalPaymentScript() && !o.scriptPubKey.IsUnspendable()) {
            strError = strprintf("Invalid Script %s", txCollateral.ToString());
            LogPrint("masternode", "CBudgetProposalBroadcast::IsBudgetCollateralValid - %s\n", strError);
            return false;
        }
        if (fBudgetFinalization) {
            // Collateral for budget finalization
            // Note: there are still old valid budgets out there, but the check for the new 5 PIV finalization collateral
            //       will also cover the old 50 PIV finalization collateral.
            LogPrint("mnbudget", "Final Budget: o.scriptPubKey(%s) == findScript(%s) ?\n", HexStr(o.scriptPubKey), HexStr(findScript));
            if (o.scriptPubKey == findScript) {
                LogPrint("mnbudget", "Final Budget: o.nValue(%ld) >= BUDGET_FEE_TX(%ld) ?\n", o.nValue, BUDGET_FEE_TX);
                if (o.nValue >= BUDGET_FEE_TX) {
                    foundOpReturn = true;
                }
            }
        } else {
            // Collateral for normal budget proposal
            LogPrint("mnbudget", "Normal Budget: o.scriptPubKey(%s) == findScript(%s) ?\n", HexStr(o.scriptPubKey), HexStr(findScript));
            if (o.scriptPubKey == findScript) {
                LogPrint("mnbudget", "Normal Budget: o.nValue(%ld) >= PROPOSAL_FEE_TX(%ld) ?\n", o.nValue, PROPOSAL_FEE_TX);
                if (o.nValue >= PROPOSAL_FEE_TX) {
                    foundOpReturn = true;
                }
            }
        }
    }

    if (!foundOpReturn) {
        strError = strprintf("Couldn't find opReturn %s in %s", nExpectedHash.ToString(), txCollateral.ToString());
        LogPrint("masternode", "CBudgetProposalBroadcast::IsBudgetCollateralValid - %s\n", strError);
        return false;
    }

    // RETRIEVE CONFIRMATIONS AND NTIME
    /*
        - nTime starts as zero and is passed-by-reference out of this function and stored in the external proposal
        - nTime is never validated via the hashing mechanism and comes from a full-validated source (the blockchain)
    */

    int conf = GetIXConfirmations(nTxCollateralHash);
    if (nBlockHash != uint256()) {
        BlockMap::iterator mi = mapBlockIndex.find(nBlockHash);
        if (mi != mapBlockIndex.end() && (*mi).second) {
            CBlockIndex* pindex = (*mi).second;
            if (chainActive.Contains(pindex)) {
                conf += chainActive.Height() - pindex->nHeight + 1;
                nTime = pindex->nTime;
            }
        }
    }

    nConf = conf;

    // if we're syncing we won't have swiftTX information, so accept 1 confirmation
    if (conf >= Params().Budget_Fee_Confirmations()) {
        return true;
    } else {
        strError = strprintf("Collateral requires at least %d confirmations - %d confirmations", Params().Budget_Fee_Confirmations(), conf);
        LogPrint("masternode", "CBudgetProposalBroadcast::IsBudgetCollateralValid - %s - %d confirmations\n", strError, conf);
        return false;
    }
}

void CBudgetManager::CheckOrphanVotes()
{
    LOCK(cs);


    std::string strError = "";
    auto it1 = mapOrphanMasternodeBudgetVotes.begin();
    while (it1 != mapOrphanMasternodeBudgetVotes.end()) {
        if (budget.UpdateProposal(((*it1).second), NULL, strError)) {
            LogPrint("masternode", "%s: Proposal/Budget is known, activating and removing orphan vote\n", __func__);
            mapOrphanMasternodeBudgetVotes.erase(it1);
        }
        ++it1;
    }
    auto it2 = mapOrphanFinalizedBudgetVotes.begin();
    while (it2 != mapOrphanFinalizedBudgetVotes.end()) {
        if (budget.UpdateFinalizedBudget(((*it2).second), NULL, strError)) {
            LogPrint("masternode", "%s: Proposal/Budget is known, activating and removing orphan vote\n", __func__);
            mapOrphanFinalizedBudgetVotes.erase(it2);
        }
        ++it2;
    }
    LogPrint("masternode", "CBudgetManager::CheckOrphanVotes - Done\n");
}

void CBudgetManager::SubmitFinalBudget()
{
    static int nSubmittedHeight = 0; // height at which final budget was submitted last time
    int nCurrentHeight;

    {
        TRY_LOCK(cs_main, locked);
        if (!locked)
            return;
        if (!chainActive.Tip())
            return;
        nCurrentHeight = chainActive.Height();
    }

    int nBlockStart = nCurrentHeight - nCurrentHeight % GetBudgetPaymentCycleBlocks() + GetBudgetPaymentCycleBlocks();
    if (nSubmittedHeight >= nBlockStart) {
        LogPrint("masternode", "CBudgetManager::SubmitFinalBudget - nSubmittedHeight(=%ld) < nBlockStart(=%ld) condition not fulfilled.\n", nSubmittedHeight, nBlockStart);
        return;
    }
    // Submit final budget during the last 2 days before payment for Mainnet, about 9 minutes for Testnet
    int nFinalizationStart = nBlockStart - ((GetBudgetPaymentCycleBlocks() / 30) * 2);
    int nOffsetToStart = nFinalizationStart - nCurrentHeight;

    if (nBlockStart - nCurrentHeight > ((GetBudgetPaymentCycleBlocks() / 30) * 2)) {
        LogPrint("masternode", "CBudgetManager::SubmitFinalBudget - Too early for finalization. Current block is %ld, next Superblock is %ld.\n", nCurrentHeight, nBlockStart);
        LogPrint("masternode", "CBudgetManager::SubmitFinalBudget - First possible block for finalization: %ld. Last possible block for finalization: %ld. You have to wait for %ld block(s) until Budget finalization will be possible\n", nFinalizationStart, nBlockStart, nOffsetToStart);

        return;
    }

    std::vector<CBudgetProposal*> vBudgetProposals = budget.GetBudget();
    std::string strBudgetName = "main";
    std::vector<CTxBudgetPayment> vecTxBudgetPayments;

    for (auto& vBudgetProposal : vBudgetProposals) {
        CTxBudgetPayment txBudgetPayment;
        txBudgetPayment.nProposalHash = vBudgetProposal->GetHash();
        txBudgetPayment.payee = vBudgetProposal->GetPayee();
        txBudgetPayment.nAmount = vBudgetProposal->GetAllotted();
        vecTxBudgetPayments.push_back(txBudgetPayment);
    }

    if (vecTxBudgetPayments.size() < 1) {
        LogPrint("masternode", "CBudgetManager::SubmitFinalBudget - Found No Proposals For Period\n");
        return;
    }

    CFinalizedBudgetBroadcast tempBudget(strBudgetName, nBlockStart, vecTxBudgetPayments, uint256());
    const uint256& budgetHash = tempBudget.GetHash();
    if (HaveSeenFinalizedBudget(budgetHash)) {
        LogPrint("masternode", "%s: Budget already exists - %s\n", __func__, budgetHash.ToString());
        nSubmittedHeight = nCurrentHeight;
        return; // already exists
    }

    // create fee tx
    CTransaction tx;
    uint256 txidCollateral;

    if (!mapCollateralTxids.count(tempBudget.GetHash())) {
        CWalletTx wtx;
        if (!pwalletMain->GetBudgetSystemCollateralTX(wtx, tempBudget.GetHash(), false)) {
            LogPrint("masternode", "CBudgetManager::SubmitFinalBudget - Can't make collateral transaction\n");
            return;
        }

        // Get our change address
        CReserveKey reservekey(pwalletMain);
        // Send the tx to the network. Do NOT use SwiftTx, locking might need too much time to propagate, especially for testnet
        pwalletMain->CommitTransaction(wtx, reservekey, "NO-ix");
        tx = (CTransaction)wtx;
        txidCollateral = tx.GetHash();
        mapCollateralTxids.emplace(budgetHash, txidCollateral);
    } else {
        txidCollateral = mapCollateralTxids[budgetHash];
        return;
    }

    // create the proposal incase we're the first to make it
    CFinalizedBudgetBroadcast finalizedBudgetBroadcast(strBudgetName, nBlockStart, vecTxBudgetPayments, txidCollateral);

    // check
    int nConf = 0;
    int64_t nTime = 0;
    std::string strError = "";
    if (!IsBudgetCollateralValid(txidCollateral, finalizedBudgetBroadcast.GetHash(), strError, nTime, nConf, true)) {
        LogPrint("masternode", "CBudgetManager::SubmitFinalBudget - Invalid finalized budget - %s \n", strError);
        return;
    }

    if (!finalizedBudgetBroadcast.UpdateValid(nCurrentHeight)) {
        LogPrint("masternode", "%s: Invalid finalized budget - %s \n", __func__, finalizedBudgetBroadcast.IsInvalidReason());
        return;
    }

    LOCK(cs);
    AddSeenFinalizedBudget(finalizedBudgetBroadcast);
    finalizedBudgetBroadcast.Relay();
    budget.AddFinalizedBudget(finalizedBudgetBroadcast);
    nSubmittedHeight = nCurrentHeight;
    LogPrint("masternode", "CBudgetManager::SubmitFinalBudget - Done! %s\n", finalizedBudgetBroadcast.GetHash().ToString());
}

//
// CBudgetDB
//

CBudgetDB::CBudgetDB()
{
    pathDB = GetDataDir() / "budget.dat";
    strMagicMessage = "MasternodeBudget";
}

bool CBudgetDB::Write(const CBudgetManager& objToSave)
{
    LOCK(objToSave.cs);

    int64_t nStart = GetTimeMillis();

    // serialize, checksum data up to that point, then append checksum
    CDataStream ssObj(SER_DISK, CLIENT_VERSION);
    ssObj << strMagicMessage;                   // masternode cache file specific magic message
    ssObj << FLATDATA(Params().MessageStart()); // network specific magic number
    ssObj << objToSave;
    uint256 hash = Hash(ssObj.begin(), ssObj.end());
    ssObj << hash;

    // open output file, and associate with CAutoFile
    FILE* file = fopen(pathDB.string().c_str(), "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file %s", __func__, pathDB.string());

    // Write and commit header, data
    try {
        fileout << ssObj;
    } catch (std::exception& e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    fileout.fclose();

    LogPrint("masternode", "Written info to budget.dat  %dms\n", GetTimeMillis() - nStart);

    return true;
}

CBudgetDB::ReadResult CBudgetDB::Read(CBudgetManager& objToLoad, bool fDryRun)
{
    LOCK(objToLoad.cs);

    int64_t nStart = GetTimeMillis();
    // open input file, and associate with CAutoFile
    FILE* file = fopen(pathDB.string().c_str(), "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        error("%s : Failed to open file %s", __func__, pathDB.string());
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = boost::filesystem::file_size(pathDB);
    int dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
        dataSize = 0;
    vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char*)&vchData[0], dataSize);
        filein >> hashIn;
    } catch (std::exception& e) {
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return HashReadError;
    }
    filein.fclose();

    CDataStream ssObj(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssObj.begin(), ssObj.end());
    if (hashIn != hashTmp) {
        error("%s : Checksum mismatch, data corrupted", __func__);
        return IncorrectHash;
    }


    unsigned char pchMsgTmp[4];
    std::string strMagicMessageTmp;
    try {
        // de-serialize file header (masternode cache file specific magic message) and ..
        ssObj >> strMagicMessageTmp;

        // ... verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp) {
            error("%s : Invalid masternode cache magic message", __func__);
            return IncorrectMagicMessage;
        }


        // de-serialize file header (network specific magic number) and ..
        ssObj >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp))) {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }

        // de-serialize data into CBudgetManager object
        ssObj >> objToLoad;
    } catch (std::exception& e) {
        objToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    LogPrint("masternode", "Loaded info from budget.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrint("masternode", "  %s\n", objToLoad.ToString());
    if (!fDryRun) {
        LogPrint("masternode", "Budget manager - cleaning....\n");
        objToLoad.CheckAndRemove();
        LogPrint("masternode", "Budget manager - result:\n");
        LogPrint("masternode", "  %s\n", objToLoad.ToString());
    }

    return Ok;
}

void DumpBudgets()
{
    int64_t nStart = GetTimeMillis();

    CBudgetDB budgetdb;
    CBudgetManager tempBudget;

    LogPrint("masternode", "Verifying budget.dat format...\n");
    CBudgetDB::ReadResult readResult = budgetdb.Read(tempBudget, true);
    // there was an error and it was not an error on file opening => do not proceed
    if (readResult == CBudgetDB::FileError)
        LogPrint("masternode", "Missing budgets file - budget.dat, will try to recreate\n");
    else if (readResult != CBudgetDB::Ok) {
        LogPrint("masternode", "Error reading budget.dat: ");
        if (readResult == CBudgetDB::IncorrectFormat)
            LogPrint("masternode", "magic is ok but data has invalid format, will try to recreate\n");
        else {
            LogPrint("masternode", "file format is unknown or invalid, please fix it manually\n");
            return;
        }
    }
    LogPrint("masternode", "Writting info to budget.dat...\n");
    budgetdb.Write(budget);

    LogPrint("masternode", "Budget dump finished  %dms\n", GetTimeMillis() - nStart);
}

bool CBudgetManager::AddFinalizedBudget(CFinalizedBudget& finalizedBudget)
{
    std::string strError = "";
    if (!finalizedBudget.UpdateValid(GetBestHeight())) {
        LogPrint("masternode", "%s: invalid finalized budget - %s\n", __func__, finalizedBudget.IsInvalidReason());
        return false;
    }

    if (mapFinalizedBudgets.count(finalizedBudget.GetHash())) {
        return false;
    }

    mapFinalizedBudgets.insert(make_pair(finalizedBudget.GetHash(), finalizedBudget));
    return true;
}

bool CBudgetManager::AddProposal(CBudgetProposal& budgetProposal)
{
    LOCK(cs);
    if (!budgetProposal.UpdateValid(GetBestHeight())) {
        LogPrint("masternode", "CBudgetManager::AddProposal - invalid budget proposal - %s\n", budgetProposal.IsInvalidReason());
        return false;
    }

    if (mapProposals.count(budgetProposal.GetHash())) {
        return false;
    }

    mapProposals.insert(make_pair(budgetProposal.GetHash(), budgetProposal));
    LogPrint("masternode", "CBudgetManager::AddProposal - proposal %s added\n", budgetProposal.GetName().c_str());
    return true;
}

void CBudgetManager::CheckAndRemove()
{
    int nCurrentHeight = GetBestHeight();
    std::map<uint256, CFinalizedBudget> tmpMapFinalizedBudgets;
    std::map<uint256, CBudgetProposal> tmpMapProposals;

    LogPrint("mnbudget", "%s: mapFinalizedBudgets cleanup - size before: %d\n", __func__, mapFinalizedBudgets.size());
    for (auto& it : mapFinalizedBudgets) {
        CFinalizedBudget* pfinalizedBudget = &(it.second);
        if (!pfinalizedBudget->UpdateValid(nCurrentHeight)) {
            LogPrint("mnbudget", "%s: Invalid finalized budget: %s\n", __func__, pfinalizedBudget->IsInvalidReason());
        } else {
            LogPrint("mnbudget", "%s: Found valid finalized budget: %s %s\n", __func__,
                     pfinalizedBudget->GetName(), pfinalizedBudget->GetFeeTXHash().ToString());
            pfinalizedBudget->CheckAndVote();
            tmpMapFinalizedBudgets.insert(std::make_pair(pfinalizedBudget->GetHash(), *pfinalizedBudget));
        }
    }

    LogPrint("mnbudget", "%s: mapProposals cleanup - size before: %d\n", __func__, mapProposals.size());
    for (auto& it : mapProposals) {
        CBudgetProposal* pbudgetProposal = &(it.second);
        if (!pbudgetProposal->UpdateValid(nCurrentHeight)) {
            LogPrint("mnbudget", "%s: Invalid budget proposal - %s\n", __func__, pbudgetProposal->IsInvalidReason());
        } else {
            LogPrint("mnbudget", "%s: Found valid budget proposal: %s %s\n", __func__,
                     pbudgetProposal->GetName(), pbudgetProposal->GetFeeTXHash().ToString());
            tmpMapProposals.insert(std::make_pair(pbudgetProposal->GetHash(), *pbudgetProposal));
        }
    }

    // Remove invalid entries by overwriting complete map
    mapFinalizedBudgets.swap(tmpMapFinalizedBudgets);
    mapProposals.swap(tmpMapProposals);

    LogPrint("mnbudget", "%s: mapFinalizedBudgets cleanup - size after: %d\n", __func__, mapFinalizedBudgets.size());
    LogPrint("mnbudget", "%s: mapProposals cleanup - size after: %d\n", __func__, mapProposals.size());
    LogPrint("mnbudget", "%s: PASSED\n", __func__);
}

void CBudgetManager::FillBlockPayee(CMutableTransaction& txNew, CScript& payee)
{
    LOCK(cs);

    CBlockIndex* pindexPrev = chainActive.Tip();
    if (!pindexPrev)
        return;

    int nHighestCount = 0;
    CAmount nAmount = 0;
    const int height = pindexPrev->nHeight;
    // ------- Grab The Highest Count

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while (it != mapFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);
        if (pfinalizedBudget->GetVoteCount() > nHighestCount &&
            height + 1 >= pfinalizedBudget->GetBlockStart() &&
            height + 1 <= pfinalizedBudget->GetBlockEnd() &&
            pfinalizedBudget->GetPayeeAndAmount(height + 1, payee, nAmount)) {
            nHighestCount = pfinalizedBudget->GetVoteCount();
        }

        ++it;
    }

    CAmount blockValue = GetBlockSubsidy(height + 1, Params().GetConsensus());

    // miners get the full amount on these blocks
    txNew.vout[0].nValue = blockValue;

    if ((height + 1 > 0) && (height + 1 <= Params().GetConsensus().GetLastFoundersRewardBlockHeight()) && !Params().GetConsensus().NetworkUpgradeActive(height, Consensus::UPGRADE_MORAG)) {
        CAmount vFoundersReward = 0;

        if (height + 1 < Params().GetConsensus().vUpgrades[Consensus::UPGRADE_OVERWINTER].nActivationHeight) {
            vFoundersReward = txNew.vout[0].nValue / 20;
        } else if (height + 1 < Params().GetConsensus().vUpgrades[Consensus::UPGRADE_KNOWHERE].nActivationHeight) {
            vFoundersReward = txNew.vout[0].nValue * 7.5 / 100;
        } else {
            vFoundersReward = txNew.vout[0].nValue * 15 / 100;
        }

        // And give it to the founders
        txNew.vout.push_back(CTxOut(vFoundersReward, Params().GetFoundersRewardScriptAtHeight(height + 1)));

        // Take some reward away from us
        txNew.vout[0].nValue -= vFoundersReward;
    }

    if ((height + 1 > 0) && (height + 1 <= Params().GetConsensus().GetLastTreasuryRewardBlockHeight())) {
        CAmount vTreasuryReward = 0;
        if (height + 1 >= Params().GetConsensus().vUpgrades[Consensus::UPGRADE_KNOWHERE].nActivationHeight &&
            !NetworkUpgradeActive(height + 1, Params().GetConsensus(), Consensus::UPGRADE_ATLANTIS)) {
            vTreasuryReward = txNew.vout[0].nValue * 5 / 100;
        } else {
            vTreasuryReward = txNew.vout[0].nValue * 10 / 100;
        }

        // Take some reward away from us
        txNew.vout[0].nValue -= vTreasuryReward;

        // And give it to the founders
        if (height + 1 >= Params().GetConsensus().vUpgrades[Consensus::UPGRADE_KNOWHERE].nActivationHeight) {
            txNew.vout.push_back(CTxOut(vTreasuryReward, Params().GetTreasuryRewardScriptAtHeight(height + 1)));
        }
    }

    if (Params().GetConsensus().NetworkUpgradeActive(height + 1, Consensus::UPGRADE_MORAG) &&
        height + 1 <= Params().GetConsensus().GetLastDevelopersRewardBlockHeight()) {
        const CAmount vDevelopersReward = GetDevelopersPayment(height + 1, blockValue);

        // And give it to the founders
        txNew.vout.push_back(CTxOut(vDevelopersReward, Params().GetDevelopersRewardScriptAtHeight(height + 1)));

        // Take some reward away from us
        txNew.vout[0].nValue -= vDevelopersReward;
    }

    if (nHighestCount > 0) {
        txNew.vout.push_back(CTxOut(nAmount, payee));

        CTxDestination address1;
        ExtractDestination(payee, address1);
        KeyIO keyIO(Params());
        LogPrintf("Masternode payment to %s\n", keyIO.EncodeDestination(address1));
    }
}

CFinalizedBudget* CBudgetManager::FindFinalizedBudget(const uint256& nHash)
{
    if (mapFinalizedBudgets.count(nHash))
        return &mapFinalizedBudgets[nHash];

    return NULL;
}

const CBudgetProposal* CBudgetManager::FindProposalByName(const std::string& strProposalName) const
{
    int64_t nYesCountMax = std::numeric_limits<int64_t>::min();
    const CBudgetProposal* pbudgetProposal = nullptr;

    for (const auto& it : mapProposals) {
        const CBudgetProposal& proposal = it.second;
        int64_t nYesCount = proposal.GetYeas() - proposal.GetNays();
        if (proposal.GetName() == strProposalName && nYesCount > nYesCountMax) {
            pbudgetProposal = &proposal;
            nYesCountMax = nYesCount;
        }
    }

    return pbudgetProposal;
}

CBudgetProposal* CBudgetManager::FindProposal(const uint256& nHash)
{
    LOCK(cs);

    if (mapProposals.count(nHash))
        return &mapProposals[nHash];

    return NULL;
}

bool CBudgetManager::IsBudgetPaymentBlock(int nBlockHeight)
{
    int nHighestCount = -1;
    int nFivePercent = mnodeman.CountEnabled(ActiveProtocol()) / 20;

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while (it != mapFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);
        if (pfinalizedBudget->GetVoteCount() > nHighestCount &&
            nBlockHeight >= pfinalizedBudget->GetBlockStart() &&
            nBlockHeight <= pfinalizedBudget->GetBlockEnd()) {
            nHighestCount = pfinalizedBudget->GetVoteCount();
        }

        ++it;
    }

    LogPrint("masternode", "CBudgetManager::IsBudgetPaymentBlock() - nHighestCount: %lli, 5%% of Masternodes: %lli. Number of budgets: %lli\n",
             nHighestCount, nFivePercent, mapFinalizedBudgets.size());

    // If budget doesn't have 5% of the network votes, then we should pay a masternode instead
    if (nHighestCount > nFivePercent)
        return true;

    return false;
}

TrxValidationStatus CBudgetManager::IsTransactionValid(const CTransaction& txNew, int nBlockHeight)
{
    LOCK(cs);

    int nHighestCount = 0;
    TrxValidationStatus transactionStatus = TrxValidationStatus::InValid;
    int nFivePercent = mnodeman.CountEnabled(ActiveProtocol()) / 20;
    std::vector<CFinalizedBudget*> ret;

    // ------- Grab The Highest Count

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while (it != mapFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);
        if (pfinalizedBudget->GetVoteCount() > nHighestCount &&
            nBlockHeight >= pfinalizedBudget->GetBlockStart() &&
            nBlockHeight <= pfinalizedBudget->GetBlockEnd()) {
            nHighestCount = pfinalizedBudget->GetVoteCount();
        }

        ++it;
    }

    LogPrint("masternode", "CBudgetManager::IsTransactionValid() - nHighestCount: %lli, 5%% of Masternodes: %lli mapFinalizedBudgets.size(): %ld\n",
             nHighestCount, nFivePercent, mapFinalizedBudgets.size());
    /*
        If budget doesn't have 5% of the network votes, then we should pay a masternode instead
    */
    if (nHighestCount < nFivePercent)
        return TrxValidationStatus::InValid;

    // check the highest finalized budgets (+/- 10% to assist in consensus)

    it = mapFinalizedBudgets.begin();
    while (it != mapFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);

        if (pfinalizedBudget->GetVoteCount() > nHighestCount - mnodeman.CountEnabled(ActiveProtocol()) / 10) {
            if (nBlockHeight >= pfinalizedBudget->GetBlockStart() && nBlockHeight <= pfinalizedBudget->GetBlockEnd()) {
                transactionStatus = pfinalizedBudget->IsTransactionValid(txNew, nBlockHeight);
                if (transactionStatus == TrxValidationStatus::Valid) {
                    LogPrint("mnbudget", "%s: pfinalizedBudget->IsTransactionValid() passed\n", __func__);
                    return TrxValidationStatus::Valid;
                }
            }
        }

        ++it;
    }

    // we looked through all of the known budgets
    return transactionStatus;
}

std::vector<CBudgetProposal*> CBudgetManager::GetAllProposals()
{
    LOCK(cs);

    std::vector<CBudgetProposal*> vBudgetProposalRet;

    std::map<uint256, CBudgetProposal>::iterator it = mapProposals.begin();
    while (it != mapProposals.end()) {
        (*it).second.CleanAndRemove();

        CBudgetProposal* pbudgetProposal = &((*it).second);
        vBudgetProposalRet.push_back(pbudgetProposal);

        ++it;
    }

    return vBudgetProposalRet;
}

//
// Sort by votes, if there's a tie sort by their feeHash TX
//
struct sortProposalsByVotes {
    bool operator()(const std::pair<CBudgetProposal*, int>& left, const std::pair<CBudgetProposal*, int>& right)
    {
        if (left.second != right.second)
            return (left.second > right.second);
        return (UintToArith256(left.first->GetFeeTXHash()) > UintToArith256(right.first->GetFeeTXHash()));
    }
};

// Need to review this function
std::vector<CBudgetProposal*> CBudgetManager::GetBudget()
{
    LOCK(cs);

    // ------- Sort budgets by Yes Count

    std::vector<std::pair<CBudgetProposal*, int>> vBudgetPorposalsSort;

    std::map<uint256, CBudgetProposal>::iterator it = mapProposals.begin();
    while (it != mapProposals.end()) {
        (*it).second.CleanAndRemove();
        vBudgetPorposalsSort.push_back(make_pair(&((*it).second), (*it).second.GetYeas() - (*it).second.GetNays()));
        ++it;
    }

    std::sort(vBudgetPorposalsSort.begin(), vBudgetPorposalsSort.end(), sortProposalsByVotes());

    // ------- Grab The Budgets In Order

    std::vector<CBudgetProposal*> vBudgetProposalsRet;

    CAmount nBudgetAllocated = 0;
    CBlockIndex* pindexPrev = chainActive.Tip();
    if (pindexPrev == NULL)
        return vBudgetProposalsRet;

    int nBlockStart = pindexPrev->nHeight - pindexPrev->nHeight % GetBudgetPaymentCycleBlocks() + GetBudgetPaymentCycleBlocks();
    int nBlockEnd = nBlockStart + GetBudgetPaymentCycleBlocks() - 1;
    CAmount nTotalBudget = GetTotalBudget(nBlockStart);


    std::vector<std::pair<CBudgetProposal*, int>>::iterator it2 = vBudgetPorposalsSort.begin();
    while (it2 != vBudgetPorposalsSort.end()) {
        CBudgetProposal* pbudgetProposal = (*it2).first;

        LogPrint("masternode", "CBudgetManager::GetBudget() - Processing Budget %s\n", pbudgetProposal->GetName().c_str());
        // prop start/end should be inside this period
        if (pbudgetProposal->IsValid() && pbudgetProposal->GetBlockStart() <= nBlockStart &&
            pbudgetProposal->GetBlockEnd() >= nBlockEnd &&
            pbudgetProposal->GetYeas() - pbudgetProposal->GetNays() > mnodeman.CountEnabled(ActiveProtocol()) / 10 &&
            pbudgetProposal->IsEstablished()) {
            LogPrint("masternode", "CBudgetManager::GetBudget() -   Check 1 passed: valid=%d | %ld <= %ld | %ld >= %ld | Yeas=%d Nays=%d Count=%d | established=%d\n",
                     pbudgetProposal->IsValid(), pbudgetProposal->GetBlockStart(), nBlockStart, pbudgetProposal->GetBlockEnd(),
                     nBlockEnd, pbudgetProposal->GetYeas(), pbudgetProposal->GetNays(), mnodeman.CountEnabled(ActiveProtocol()) / 10,
                     pbudgetProposal->IsEstablished());

            if (pbudgetProposal->GetAmount() + nBudgetAllocated <= nTotalBudget) {
                pbudgetProposal->SetAllotted(pbudgetProposal->GetAmount());
                nBudgetAllocated += pbudgetProposal->GetAmount();
                vBudgetProposalsRet.push_back(pbudgetProposal);
                LogPrint("masternode", "CBudgetManager::GetBudget() -     Check 2 passed: Budget added\n");
            } else {
                pbudgetProposal->SetAllotted(0);
                LogPrint("masternode", "CBudgetManager::GetBudget() -     Check 2 failed: no amount allotted\n");
            }
        } else {
            LogPrint("masternode", "CBudgetManager::GetBudget() -   Check 1 failed: valid=%d | %ld <= %ld | %ld >= %ld | Yeas=%d Nays=%d Count=%d | established=%d\n",
                     pbudgetProposal->IsValid(), pbudgetProposal->GetBlockStart(), nBlockStart, pbudgetProposal->GetBlockEnd(),
                     nBlockEnd, pbudgetProposal->GetYeas(), pbudgetProposal->GetNays(), mnodeman.CountEnabled(ActiveProtocol()) / 10,
                     pbudgetProposal->IsEstablished());
        }

        ++it2;
    }

    return vBudgetProposalsRet;
}

struct sortFinalizedBudgetsByVotes {
    bool operator()(const std::pair<CFinalizedBudget*, int>& left, const std::pair<CFinalizedBudget*, int>& right)
    {
        return left.second > right.second;
    }
};
std::vector<CFinalizedBudget*> CBudgetManager::GetFinalizedBudgets()
{
    LOCK(cs);

    std::vector<CFinalizedBudget*> vFinalizedBudgetsRet;

    // ------- Grab The Budgets In Order
    std::vector<std::pair<CFinalizedBudget*, int>> vFinalizedBudgetsSort;

    // ------- Grab The Budgets In Order

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while (it != mapFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);

        vFinalizedBudgetsSort.push_back(make_pair(pfinalizedBudget, pfinalizedBudget->GetVoteCount()));
        ++it;
    }
    std::sort(vFinalizedBudgetsSort.begin(), vFinalizedBudgetsSort.end(), sortFinalizedBudgetsByVotes());

    std::vector<std::pair<CFinalizedBudget*, int>>::iterator it2 = vFinalizedBudgetsSort.begin();
    while (it2 != vFinalizedBudgetsSort.end()) {
        vFinalizedBudgetsRet.push_back((*it2).first);
        ++it2;
    }

    return vFinalizedBudgetsRet;
}

std::string CBudgetManager::GetRequiredPaymentsString(int nBlockHeight)
{
    LOCK(cs);

    std::string ret = "unknown-budget";

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while (it != mapFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);
        if (nBlockHeight >= pfinalizedBudget->GetBlockStart() && nBlockHeight <= pfinalizedBudget->GetBlockEnd()) {
            CTxBudgetPayment payment;
            if (pfinalizedBudget->GetBudgetPaymentByBlock(nBlockHeight, payment)) {
                if (ret == "unknown-budget") {
                    ret = payment.nProposalHash.ToString();
                } else {
                    ret += ",";
                    ret += payment.nProposalHash.ToString();
                }
            } else {
                LogPrint("masternode", "CBudgetManager::GetRequiredPaymentsString - Couldn't find budget payment for block %d\n", nBlockHeight);
            }
        }

        ++it;
    }

    return ret;
}

CAmount CBudgetManager::GetTotalBudget(int nHeight)
{
    if (NetworkIdFromCommandLine() == CBaseChainParams::TESTNET) {
        CAmount nSubsidy = 500 * COIN;
        return ((nSubsidy / 100) * 10) * 1440;
    }

    CAmount nSubsidy = 500 * COIN;
    return ((nSubsidy / 100) * 10) * 1440;
}

void CBudgetManager::AddSeenProposal(const CBudgetProposalBroadcast& prop)
{
    mapSeenMasternodeBudgetProposals.insert(std::make_pair(prop.GetHash(), prop));
}

void CBudgetManager::AddSeenProposalVote(const CBudgetVote& vote)
{
    mapSeenMasternodeBudgetVotes.insert(std::make_pair(vote.GetHash(), vote));
}

void CBudgetManager::AddSeenFinalizedBudget(const CFinalizedBudgetBroadcast& bud)
{
    mapSeenFinalizedBudgets.insert(std::make_pair(bud.GetHash(), bud));
}

void CBudgetManager::AddSeenFinalizedBudgetVote(const CFinalizedBudgetVote& vote)
{
    mapSeenFinalizedBudgetVotes.insert(std::make_pair(vote.GetHash(), vote));
}


CDataStream CBudgetManager::GetProposalVoteSerialized(const uint256& voteHash) const
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(1000);
    ss << mapSeenMasternodeBudgetVotes.at(voteHash);
    return ss;
}

CDataStream CBudgetManager::GetProposalSerialized(const uint256& propHash) const
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(1000);
    ss << mapSeenMasternodeBudgetProposals.at(propHash);
    return ss;
}

CDataStream CBudgetManager::GetFinalizedBudgetVoteSerialized(const uint256& voteHash) const
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(1000);
    ss << mapSeenFinalizedBudgetVotes.at(voteHash);
    return ss;
}

CDataStream CBudgetManager::GetFinalizedBudgetSerialized(const uint256& budgetHash) const
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(1000);
    ss << mapSeenFinalizedBudgets.at(budgetHash);
    return ss;
}

bool CBudgetManager::AddAndRelayProposalVote(const CBudgetVote& vote, std::string& strError)
{
    if (UpdateProposal(vote, nullptr, strError)) {
        AddSeenProposalVote(vote);
        vote.Relay();
        return true;
    }
    return false;
}


void CBudgetManager::NewBlock(int height)
{
    TRY_LOCK(cs, fBudgetNewBlock);
    if (!fBudgetNewBlock)
        return;
    SetBestHeight(height);

    if (masternodeSync.RequestedMasternodeAssets <= MASTERNODE_SYNC_BUDGET)
        return;

    if (strBudgetMode == "suggest") { // suggest the budget we see
        SubmitFinalBudget();
    }

    int nCurrentHeight = GetBestHeight();
    // this function should be called 1/14 blocks, allowing up to 100 votes per day on all proposals
    if (nCurrentHeight % 14 != 0)
        return;

    // incremental sync with our peers
    if (masternodeSync.IsSynced()) {
        LogPrint("masternode", "CBudgetManager::NewBlock - incremental sync started\n");
        if (0 == rand() % 1440) {
            ClearSeen();
            ResetSync();
        }

        // LOCK(cs_vNodes);
        for (CNode* pnode : vNodes)
            if (pnode->nVersion >= ActiveProtocol())
                Sync(pnode, uint256(), true);

        MarkSynced();
    }


    CheckAndRemove();

    // remove invalid votes once in a while (we have to check the signatures and validity of every vote, somewhat CPU intensive)

    LogPrint("masternode", "CBudgetManager::NewBlock - askedForSourceProposalOrBudget cleanup - size: %d\n", askedForSourceProposalOrBudget.size());
    std::map<uint256, int64_t>::iterator it = askedForSourceProposalOrBudget.begin();
    while (it != askedForSourceProposalOrBudget.end()) {
        if ((*it).second > GetTime() - (60 * 60 * 24)) {
            ++it;
        } else {
            askedForSourceProposalOrBudget.erase(it++);
        }
    }

    LogPrint("masternode", "CBudgetManager::NewBlock - mapProposals cleanup - size: %d\n", mapProposals.size());
    std::map<uint256, CBudgetProposal>::iterator it2 = mapProposals.begin();
    while (it2 != mapProposals.end()) {
        (*it2).second.CleanAndRemove();
        ++it2;
    }

    LogPrint("masternode", "CBudgetManager::NewBlock - mapFinalizedBudgets cleanup - size: %d\n", mapFinalizedBudgets.size());
    std::map<uint256, CFinalizedBudget>::iterator it3 = mapFinalizedBudgets.begin();
    while (it3 != mapFinalizedBudgets.end()) {
        (*it3).second.CleanAndRemove();
        ++it3;
    }

    LogPrint("masternode", "CBudgetManager::NewBlock - vecImmatureBudgetProposals cleanup - size: %d\n", vecImmatureBudgetProposals.size());
    std::vector<CBudgetProposalBroadcast>::iterator it4 = vecImmatureBudgetProposals.begin();
    while (it4 != vecImmatureBudgetProposals.end()) {
        std::string strError = "";
        int nConf = 0;
        const uint256& nHash = it4->GetHash();
        if (!IsBudgetCollateralValid(it4->GetFeeTXHash(), nHash, strError, (*it4).nTime, nConf)) {
            ++it4;
            continue;
        }

        if (!(*it4).UpdateValid(nCurrentHeight)) {
            LogPrint("masternode", "mprop (immature) - invalid budget proposal - %s\n", strError);
            it4 = vecImmatureBudgetProposals.erase(it4);
            continue;
        }

        CBudgetProposal budgetProposal((*it4));
        if (AddProposal(budgetProposal)) {
            (*it4).Relay();
        }

        LogPrint("masternode", "mprop (immature) - new budget - %s\n", (*it4).GetHash().ToString());
        it4 = vecImmatureBudgetProposals.erase(it4);
    }

    LogPrint("masternode", "CBudgetManager::NewBlock - vecImmatureFinalizedBudgets cleanup - size: %d\n", vecImmatureFinalizedBudgets.size());
    std::vector<CFinalizedBudgetBroadcast>::iterator it5 = vecImmatureFinalizedBudgets.begin();
    while (it5 != vecImmatureFinalizedBudgets.end()) {
        std::string strError = "";
        int nConf = 0;
        const uint256& nHash = it5->GetHash();
        if (!IsBudgetCollateralValid(it5->GetFeeTXHash(), nHash, strError, (*it5).nTime, nConf, true)) {
            ++it5;
            continue;
        }

        if (!(*it5).UpdateValid(nCurrentHeight)) {
            LogPrint("masternode", "fbs (immature) - invalid finalized budget - %s\n", it5->IsInvalidReason());
            it5 = vecImmatureFinalizedBudgets.erase(it5);
            continue;
        }

        LogPrint("masternode", "fbs (immature) - new finalized budget - %s\n", (*it5).GetHash().ToString());

        CFinalizedBudget finalizedBudget((*it5));
        if (AddFinalizedBudget(finalizedBudget)) {
            (*it5).Relay();
        }

        it5 = vecImmatureFinalizedBudgets.erase(it5);
    }
    LogPrint("masternode", "CBudgetManager::NewBlock - PASSED\n");
}

void CBudgetManager::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    // lite mode is not supported
    if (fLiteMode)
        return;
    if (!masternodeSync.IsBlockchainSynced())
        return;

    int nCurrentHeight = GetBestHeight();
    LOCK(cs_budget);

    if (strCommand == "mnvs") { // Masternode vote sync
        uint256 nProp;
        vRecv >> nProp;

        if (NetworkIdFromCommandLine() == CBaseChainParams::MAIN) {
            if (nProp == uint256()) {
                if (pfrom->HasFulfilledRequest("mnvs")) {
                    LogPrint("masternode", "mnvs - peer already asked me for the list\n");
                    Misbehaving(pfrom->GetId(), 20);
                    return;
                }
                pfrom->FulfilledRequest("mnvs");
            }
        }

        Sync(pfrom, nProp);
        LogPrint("mnbudget", "mnvs - Sent Masternode votes to peer %i\n", pfrom->GetId());
    }

    if (strCommand == "mprop") { // Masternode Proposal
        CBudgetProposalBroadcast budgetProposalBroadcast;
        vRecv >> budgetProposalBroadcast;

        if (HaveSeenProposal(budgetProposalBroadcast.GetHash())) {
            masternodeSync.AddedBudgetItem(budgetProposalBroadcast.GetHash());
            return;
        }

        std::string strError = "";
        int nConf = 0;
        const uint256& nHash = budgetProposalBroadcast.GetHash();
        const uint256& nFeeTXHash = budgetProposalBroadcast.GetFeeTXHash();
        if (!IsBudgetCollateralValid(nFeeTXHash, nHash, strError, budgetProposalBroadcast.nTime, nConf)) {
            LogPrint("mnbudget", "Proposal FeeTX is not valid - %s - %s\n", nFeeTXHash.ToString(), strError);
            if (nConf >= 1)
                vecImmatureBudgetProposals.push_back(budgetProposalBroadcast);
            return;
        }

        AddSeenProposal(budgetProposalBroadcast);

        if (!budgetProposalBroadcast.UpdateValid(nCurrentHeight)) {
            LogPrint("masternode", "mprop - invalid budget proposal - %s\n", budgetProposalBroadcast.IsInvalidReason());
            return;
        }

        CBudgetProposal budgetProposal(budgetProposalBroadcast);
        if (AddProposal(budgetProposal)) {
            budgetProposalBroadcast.Relay();
        }
        masternodeSync.AddedBudgetItem(nHash);

        LogPrint("masternode", "mprop - new budget - %s\n", nHash.ToString());

        // We might have active votes for this proposal that are valid now
        CheckOrphanVotes();
    }

    if (strCommand == "mvote") { // Masternode Vote
        CBudgetVote vote;
        vRecv >> vote;
        vote.SetValid(true);

        if (HaveSeenProposalVote(vote.GetHash())) {
            masternodeSync.AddedBudgetItem(vote.GetHash());
            return;
        }

        const CTxIn& voteVin = vote.GetVin();
        CMasternode* pmn = mnodeman.Find(voteVin);
        if (pmn == NULL) {
            LogPrint("masternode", "mvote - unknown masternode - vin: %s\n", voteVin.ToString());
            mnodeman.AskForMN(pfrom, voteVin);
            return;
        }


        AddSeenProposalVote(vote);
        string strError = "";
        if (!vote.CheckSignature(strError)) {
            LogPrint("masternode", "mvote - signature invalid\n");
            if (masternodeSync.IsSynced()) {
                LogPrintf("mvote - signature invalid\n");
                LOCK(cs_main);
                Misbehaving(pfrom->GetId(), 20);
            }
            // it could just be a non-synced masternode
            mnodeman.AskForMN(pfrom, voteVin);
            return;
        }

        if (UpdateProposal(vote, pfrom, strError)) {
            vote.Relay();
            masternodeSync.AddedBudgetItem(vote.GetHash());
        }

        LogPrint("masternode", "mvote - new budget vote for budget %s - %s\n", vote.GetProposalHash().ToString(), vote.GetHash().ToString());
    }

    if (strCommand == "fbs") { // Finalized Budget Suggestion
        CFinalizedBudgetBroadcast finalizedBudgetBroadcast;
        vRecv >> finalizedBudgetBroadcast;

        if (HaveSeenFinalizedBudget(finalizedBudgetBroadcast.GetHash())) {
            masternodeSync.AddedBudgetItem(finalizedBudgetBroadcast.GetHash());
            return;
        }

        std::string strError = "";
        int nConf = 0;
        const uint256& nHash = finalizedBudgetBroadcast.GetHash();
        const uint256& nFeeTXHash = finalizedBudgetBroadcast.GetFeeTXHash();
        if (!IsBudgetCollateralValid(nFeeTXHash, nHash, strError, finalizedBudgetBroadcast.nTime, nConf, true)) {
            LogPrint("masternode", "Finalized Budget FeeTX is not valid - %s - %s\n", finalizedBudgetBroadcast.GetFeeTXHash().ToString(), strError);

            if (nConf >= 1)
                vecImmatureFinalizedBudgets.push_back(finalizedBudgetBroadcast);
            return;
        }

        AddSeenFinalizedBudget(finalizedBudgetBroadcast);

        if (!finalizedBudgetBroadcast.UpdateValid(nCurrentHeight)) {
            LogPrint("mnbudget", "fbs - invalid finalized budget - %s\n", finalizedBudgetBroadcast.IsInvalidReason());
            return;
        }

        LogPrint("masternode", "fbs - new finalized budget - %s\n", finalizedBudgetBroadcast.GetHash().ToString());

        CFinalizedBudget finalizedBudget(finalizedBudgetBroadcast);
        if (AddFinalizedBudget(finalizedBudget)) {
            finalizedBudgetBroadcast.Relay();
        }
        masternodeSync.AddedBudgetItem(nHash);

        // we might have active votes for this budget that are now valid
        CheckOrphanVotes();
    }

    if (strCommand == "fbvote") { // Finalized Budget Vote
        CFinalizedBudgetVote vote;
        vRecv >> vote;
        vote.SetValid(true);

        if (HaveSeenFinalizedBudgetVote(vote.GetHash())) {
            masternodeSync.AddedBudgetItem(vote.GetHash());
            return;
        }

        const CTxIn& voteVin = vote.GetVin();
        CMasternode* pmn = mnodeman.Find(voteVin);
        if (pmn == NULL) {
            LogPrint("mnbudget", "fbvote - unknown masternode - vin: %s\n", voteVin.prevout.hash.ToString());
            mnodeman.AskForMN(pfrom, voteVin);
            return;
        }

        std::string strError = "";
        mapSeenFinalizedBudgetVotes.insert(make_pair(vote.GetHash(), vote));
        if (!vote.CheckSignature(strError)) {
            if (masternodeSync.IsSynced()) {
                LogPrintf("CBudgetManager::ProcessMessage() : fbvote - signature invalid\n");
                LOCK(cs_main);
                Misbehaving(pfrom->GetId(), 20);
            }
            // it could just be a non-synced masternode
            mnodeman.AskForMN(pfrom, voteVin);
            return;
        }

        if (UpdateFinalizedBudget(vote, pfrom, strError)) {
            vote.Relay();
            masternodeSync.AddedBudgetItem(vote.GetHash());

            LogPrint("masternode", "fbvote - new finalized budget vote - %s\n", vote.GetHash().ToString());
        } else {
            LogPrint("masternode", "fbvote - rejected finalized budget vote - %s - %s\n", vote.GetHash().ToString(), strError);
        }
    }
}

// mark that a full sync is needed
void CBudgetManager::SetSynced(bool synced)
{
    LOCK(cs);

    for (const auto& it : mapSeenMasternodeBudgetProposals) {
        CBudgetProposal* pbudgetProposal = FindProposal(it.first);
        if (pbudgetProposal && pbudgetProposal->IsValid()) {
            // mark votes
            pbudgetProposal->SetSynced(synced);
        }
    }

    for (const auto& it : mapSeenFinalizedBudgets) {
        CFinalizedBudget* pfinalizedBudget = FindFinalizedBudget(it.first);
        if (pfinalizedBudget && pfinalizedBudget->IsValid()) {
            // mark votes
            pfinalizedBudget->SetSynced(synced);
        }
    }
}

void CBudgetManager::Sync(CNode* pfrom, const uint256& nProp, bool fPartial)
{
    LOCK(cs);

    /*
        Sync with a client on the network

        --

        This code checks each of the hash maps for all known budget proposals and finalized budget proposals, then checks them against the
        budget object to see if they're OK. If all checks pass, we'll send it to the peer.

    */
    int nInvCount = 0;

    std::map<uint256, CBudgetProposalBroadcast>::iterator it1 = mapSeenMasternodeBudgetProposals.begin();
    for (auto& it : mapSeenMasternodeBudgetProposals) {
        CBudgetProposal* pbudgetProposal = FindProposal(it.first);
        if (pbudgetProposal && pbudgetProposal->IsValid() && (nProp.IsNull() || it.first == nProp)) {
            pfrom->PushInventory(CInv(MSG_BUDGET_PROPOSAL, it.second.GetHash()));
            nInvCount++;
            pbudgetProposal->SyncVotes(pfrom, fPartial, nInvCount);
        }
    }

    pfrom->PushMessage("ssc", MASTERNODE_SYNC_BUDGET_PROP, nInvCount);

    LogPrint("mnbudget", "CBudgetManager::Sync - sent %d items\n", nInvCount);

    nInvCount = 0;
    for (auto& it : mapSeenFinalizedBudgets) {
        CFinalizedBudget* pfinalizedBudget = FindFinalizedBudget(it.first);
        if (pfinalizedBudget && pfinalizedBudget->IsValid() && (nProp.IsNull() || it.first == nProp)) {
            pfrom->PushInventory(CInv(MSG_BUDGET_FINALIZED, it.second.GetHash()));
            nInvCount++;
            pfinalizedBudget->SyncVotes(pfrom, fPartial, nInvCount);
        }
    }

    pfrom->PushMessage("ssc", MASTERNODE_SYNC_BUDGET_FIN, nInvCount);
    LogPrint("mnbudget", "CBudgetManager::Sync - sent %d items\n", nInvCount);
}

bool CBudgetManager::UpdateProposal(const CBudgetVote& vote, CNode* pfrom, std::string& strError)
{
    LOCK(cs);

    const uint256& nProposalHash = vote.GetProposalHash();
    if (!mapProposals.count(nProposalHash)) {
        if (pfrom) {
            // only ask for missing items after our syncing process is complete --
            //   otherwise we'll think a full sync succeeded when they return a result
            if (!masternodeSync.IsSynced())
                return false;

            LogPrint("masternode", "CBudgetManager::UpdateProposal - Unknown proposal %d, asking for source proposal\n", nProposalHash.ToString());
            mapOrphanMasternodeBudgetVotes[nProposalHash] = vote;

            if (!askedForSourceProposalOrBudget.count(nProposalHash)) {
                pfrom->PushMessage("mnvs", nProposalHash);
                askedForSourceProposalOrBudget[nProposalHash] = GetTime();
            }
        }

        strError = "Proposal not found!";
        return false;
    }


    return mapProposals[nProposalHash].AddOrUpdateVote(vote, strError);
}

bool CBudgetManager::UpdateFinalizedBudget(CFinalizedBudgetVote& vote, CNode* pfrom, std::string& strError)
{
    LOCK(cs);

    const uint256& nBudgetHash = vote.GetBudgetHash();
    if (!mapFinalizedBudgets.count(nBudgetHash)) {
        if (pfrom) {
            // only ask for missing items after our syncing process is complete --
            //   otherwise we'll think a full sync succeeded when they return a result
            if (!masternodeSync.IsSynced())
                return false;

            LogPrint("masternode", "CBudgetManager::UpdateFinalizedBudget - Unknown Finalized Proposal %s, asking for source budget\n", nBudgetHash.ToString());
            mapOrphanFinalizedBudgetVotes[nBudgetHash] = vote;

            if (!askedForSourceProposalOrBudget.count(nBudgetHash)) {
                pfrom->PushMessage("mnvs", nBudgetHash);
                askedForSourceProposalOrBudget[nBudgetHash] = GetTime();
            }
        }

        strError = "Finalized Budget " + nBudgetHash.ToString() + " not found!";
        return false;
    }
    LogPrint("masternode", "CBudgetManager::UpdateFinalizedBudget - Finalized Proposal %s added\n", nBudgetHash.ToString());
    return mapFinalizedBudgets[nBudgetHash].AddOrUpdateVote(vote, strError);
}

CBudgetProposal::CBudgetProposal()
{
    strProposalName = "unknown";
    nBlockStart = 0;
    nBlockEnd = 0;
    nAmount = 0;
    nTime = 0;
    fValid = true;
    strInvalid = "";
}

CBudgetProposal::CBudgetProposal(std::string strProposalNameIn, std::string strURLIn, int nBlockStartIn, int nBlockEndIn, CScript addressIn, CAmount nAmountIn, uint256 nFeeTXHashIn)
{
    strProposalName = strProposalNameIn;
    strURL = strURLIn;
    nBlockStart = nBlockStartIn;
    nBlockEnd = nBlockEndIn;
    address = addressIn;
    nAmount = nAmountIn;
    nFeeTXHash = nFeeTXHashIn;
    fValid = true;
    strInvalid = "";
}

CBudgetProposal::CBudgetProposal(const CBudgetProposal& other)
{
    strProposalName = other.strProposalName;
    strURL = other.strURL;
    nBlockStart = other.nBlockStart;
    nBlockEnd = other.nBlockEnd;
    address = other.address;
    nAmount = other.nAmount;
    nTime = other.nTime;
    nFeeTXHash = other.nFeeTXHash;
    mapVotes = other.mapVotes;
    fValid = true;
}

void CBudgetProposal::SyncVotes(CNode* pfrom, bool fPartial, int& nInvCount) const
{
    LOCK(cs);
    for (const auto& it : mapVotes) {
        const CBudgetVote& vote = it.second;
        if (vote.IsValid() && (!fPartial || !vote.IsSynced())) {
            pfrom->PushInventory(CInv(MSG_BUDGET_VOTE, vote.GetHash()));
            nInvCount++;
        }
    }
}

bool CBudgetProposal::UpdateValid(int nCurrentHeight, bool fCheckCollateral)
{
    fValid = false;
    if (GetNays() - GetYeas() > mnodeman.CountEnabled(ActiveProtocol()) / 10) {
        strInvalid = "Proposal " + strProposalName + ": Active removal";
        return false;
    }

    if (nBlockStart < 0) {
        strInvalid = "Invalid Proposal";
        return false;
    }

    if (nBlockEnd < nBlockStart) {
        strInvalid = "Proposal " + strProposalName + ": Invalid nBlockEnd (end before start)";
        return false;
    }

    if (nAmount < 10 * COIN) {
        strInvalid = "Proposal " + strProposalName + ": Invalid nAmount";
        return false;
    }

    if (address == CScript()) {
        strInvalid = "Proposal " + strProposalName + ": Invalid Payment Address";
        return false;
    }

    if (fCheckCollateral) {
        int nConf = 0;
        if (!IsBudgetCollateralValid(nFeeTXHash, GetHash(), strInvalid, nTime, nConf)) {
            strInvalid = "Proposal " + strProposalName + ": Invalid collateral";
            return false;
        }
    }

    /*
        TODO: There might be an issue with multisig in the coinbase on mainnet, we will add support for it in a future release.
    */
    if (address.IsPayToScriptHash()) {
        strInvalid = "Proposal " + strProposalName + ": Multisig is not currently supported.";
        return false;
    }

    // if proposal doesn't gain traction within 2 weeks, remove it
    //  nTime not being saved correctly
    //  -- TODO: We should keep track of the last time the proposal was valid, if it's invalid for 2 weeks, erase it
    //  if(nTime + (60*60*24*2) < GetAdjustedTime()) {
    //      if(GetYeas()-GetNays() < (mnodeman.CountEnabled(ActiveProtocol())/10)) {
    //          strError = "Not enough support";
    //          return false;
    //      }
    //  }

    // can only pay out 10% of the possible coins (min value of coins)
    if (nAmount > budget.GetTotalBudget(nBlockStart)) {
        strInvalid = "Proposal " + strProposalName + ": Payment more than max";
        return false;
    }

    // Calculate maximum block this proposal will be valid, which is start of proposal + (number of payments * cycle)
    int nProposalEnd = GetBlockStart() + (GetBudgetPaymentCycleBlocks() * GetTotalPaymentCount());

    // if (GetBlockEnd() < pindexPrev->nHeight - GetBudgetPaymentCycleBlocks() / 2) {
    if (nCurrentHeight <= 0) {
        strInvalid = "Proposal " + strProposalName + ": Tip is NULL";
        return true;
    }

    if (nProposalEnd < nCurrentHeight) {
        strInvalid = "Proposal " + strProposalName + ": Invalid nBlockEnd (" + std::to_string(nProposalEnd) + ") < current height (" + std::to_string(nCurrentHeight) + ")";
        return false;
    }

    fValid = true;
    strInvalid.clear();
    return true;
}

bool CBudgetProposal::IsEstablished() const
{
    return nTime < GetAdjustedTime() - Params().GetConsensus().nProposalEstablishmentTime;
}

bool CBudgetProposal::IsPassing(int nBlockStartBudget, int nBlockEndBudget, int mnCount) const
{
    if (!fValid)
        return false;

    if (this->nBlockStart > nBlockStartBudget)
        return false;

    if (this->nBlockEnd < nBlockEndBudget)
        return false;

    if (GetYeas() - GetNays() <= mnCount / 10)
        return false;

    if (!IsEstablished())
        return false;

    return true;
}

bool CBudgetProposal::AddOrUpdateVote(const CBudgetVote& vote, std::string& strError)
{
    std::string strAction = "New vote inserted:";
    LOCK(cs);

    const uint256& hash = vote.GetVin().prevout.GetHash();
    const int64_t voteTime = vote.GetTime();

    if (mapVotes.count(hash)) {
        const int64_t& oldTime = mapVotes[hash].GetTime();
        if (oldTime > voteTime) {
            strError = strprintf("new vote older than existing vote - %s\n", vote.GetHash().ToString());
            LogPrint("mnbudget", "CBudgetProposal::AddOrUpdateVote - %s\n", strError);
            return false;
        }
        if (voteTime - oldTime < BUDGET_VOTE_UPDATE_MIN) {
            strError = strprintf("time between votes is too soon - %s - %lli sec < %lli sec\n", vote.GetHash().ToString(), voteTime - oldTime, BUDGET_VOTE_UPDATE_MIN);
            LogPrint("mnbudget", "CBudgetProposal::AddOrUpdateVote - %s\n", strError);
            return false;
        }
        strAction = "Existing vote updated:";
    }

    if (voteTime > GetTime() + (60 * 60)) {
        strError = strprintf("new vote is too far ahead of current time - %s - nTime %lli - Max Time %lli\n", vote.GetHash().ToString(), voteTime, GetTime() + (60 * 60));
        LogPrint("mnbudget", "CBudgetProposal::AddOrUpdateVote - %s\n", strError);
        return false;
    }

    mapVotes[hash] = vote;
    LogPrint("mnbudget", "CBudgetProposal::AddOrUpdateVote - %s %s\n", strAction.c_str(), vote.GetHash().ToString().c_str());

    return true;
}

UniValue CBudgetProposal::GetVotesArray() const
{
    LOCK(cs);
    UniValue ret(UniValue::VARR);
    for (const auto& it : mapVotes) {
        ret.push_back(it.second.ToJSON());
    }
    return ret;
}

void CBudgetProposal::SetSynced(bool synced)
{
    LOCK(cs);
    for (auto& it : mapVotes) {
        CBudgetVote& vote = it.second;
        if (synced) {
            if (vote.IsValid())
                vote.SetSynced(true);
        } else {
            vote.SetSynced(false);
        }
    }
}

// If masternode voted for a proposal, but is now invalid -- remove the vote
void CBudgetProposal::CleanAndRemove()
{
    std::map<uint256, CBudgetVote>::iterator it = mapVotes.begin();

    while (it != mapVotes.end()) {
        CMasternode* pmn = mnodeman.Find((*it).second.GetVin());
        (*it).second.SetValid(pmn != nullptr);
        ++it;
    }
}

double CBudgetProposal::GetRatio() const
{
    int yeas = GetYeas();
    int nays = GetNays();

    if (yeas + nays == 0)
        return 0.0f;

    return ((double)(yeas) / (double)(yeas + nays));
}

int CBudgetProposal::GetVoteCount(CBudgetVote::VoteDirection vd) const
{
    LOCK(cs);
    int ret = 0;
    for (const auto& it : mapVotes) {
        const CBudgetVote& vote = it.second;
        if (vote.GetDirection() == vd && vote.IsValid())
            ret++;
    }
    return ret;
}

int CBudgetProposal::GetBlockStartCycle() const
{
    // end block is half way through the next cycle (so the proposal will be removed much after the payment is sent)

    return nBlockStart - nBlockStart % GetBudgetPaymentCycleBlocks();
}

int CBudgetProposal::GetBlockCycle(int nHeight)
{
    return nHeight - nHeight % GetBudgetPaymentCycleBlocks();
}

int CBudgetProposal::GetBlockEndCycle() const
{
    // XX42: right now single payment proposals have nBlockEnd have a cycle too early!
    // switch back if it break something else
    // end block is half way through the next cycle (so the proposal will be removed much after the payment is sent)
    // return nBlockEnd - GetBudgetPaymentCycleBlocks() / 2;

    // End block is half way through the next cycle (so the proposal will be removed much after the payment is sent)
    return nBlockEnd;
}

int CBudgetProposal::GetTotalPaymentCount() const
{
    return (GetBlockEndCycle() - GetBlockStartCycle()) / GetBudgetPaymentCycleBlocks();
}

int CBudgetProposal::GetRemainingPaymentCount(int nCurrentHeight) const
{
    // If this budget starts in the future, this value will be wrong

    int nPayments = (GetBlockEndCycle() - GetBlockCycle(nCurrentHeight)) / GetBudgetPaymentCycleBlocks() - 1;
    // Take the lowest value
    return std::min(nPayments, GetTotalPaymentCount());
}

CBudgetProposalBroadcast::CBudgetProposalBroadcast(std::string strProposalNameIn, std::string strURLIn, int nPaymentCount, CScript addressIn, CAmount nAmountIn, int nBlockStartIn, uint256 nFeeTXHashIn)
{
    strProposalName = strProposalNameIn;
    strURL = strURLIn;

    nBlockStart = nBlockStartIn;

    int nCycleStart = nBlockStart - nBlockStart % GetBudgetPaymentCycleBlocks();

    // XX42: right now single payment proposals have nBlockEnd have a cycle too early!
    // switch back if it break something else
    // calculate the end of the cycle for this vote, add half a cycle (vote will be deleted after that block)
    // nBlockEnd = nCycleStart + GetBudgetPaymentCycleBlocks() * nPaymentCount + GetBudgetPaymentCycleBlocks() / 2;

    // Calculate the end of the cycle for this vote, vote will be deleted after next cycle
    nBlockEnd = nCycleStart + GetBudgetPaymentCycleBlocks() * nPaymentCount + 1;

    address = addressIn;
    nAmount = nAmountIn;

    nFeeTXHash = nFeeTXHashIn;
}

void CBudgetProposalBroadcast::Relay()
{
    CInv inv(MSG_BUDGET_PROPOSAL, GetHash());
    RelayInv(inv);
}

CBudgetVote::CBudgetVote() : CSignedMessage(),
                             fValid(true),
                             fSynced(false),
                             vin(),
                             nProposalHash(uint256()),
                             nVote(VOTE_ABSTAIN),
                             nTime(0)
{
    const bool fNewSigs = NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_MORAG);
    if (fNewSigs) {
        nMessVersion = MessageVersion::MESS_VER_HASH;
    }
}

CBudgetVote::CBudgetVote(CTxIn vinIn, uint256 nProposalHashIn, VoteDirection nVoteIn) : CSignedMessage(),
                                                                                        fValid(true),
                                                                                        fSynced(false),
                                                                                        vin(vinIn),
                                                                                        nProposalHash(nProposalHashIn),
                                                                                        nVote(nVoteIn),
                                                                                        nTime(GetAdjustedTime())
{
    const bool fNewSigs = NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_MORAG);
    if (fNewSigs) {
        nMessVersion = MessageVersion::MESS_VER_HASH;
    }
}

void CBudgetVote::Relay() const
{
    CInv inv(MSG_BUDGET_VOTE, GetHash());
    RelayInv(inv);
}

uint256 CBudgetVote::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << vin;
    ss << nProposalHash;
    ss << (int)nVote;
    ss << nTime;
    return ss.GetHash();
}

std::string CBudgetVote::GetStrMessage() const
{
    return vin.prevout.ToStringShort() + nProposalHash.ToString() +
           std::to_string(nVote) + std::to_string(nTime);
}

UniValue CBudgetVote::ToJSON() const
{
    UniValue bObj(UniValue::VOBJ);
    bObj.push_back(Pair("mnId", vin.prevout.hash.ToString()));
    bObj.push_back(Pair("nHash", vin.prevout.GetHash().ToString()));
    bObj.push_back(Pair("Vote", GetVoteString()));
    bObj.push_back(Pair("nTime", nTime));
    bObj.push_back(Pair("fValid", fValid));
    return bObj;
}

CFinalizedBudget::CFinalizedBudget() : fAutoChecked(false),
                                       fValid(true),
                                       strBudgetName(""),
                                       strInvalid(),
                                       nBlockStart(0),
                                       vecBudgetPayments(),
                                       mapVotes(),
                                       nTime(0),
                                       nFeeTXHash(uint256())
{
}
CFinalizedBudget::CFinalizedBudget(const CFinalizedBudget& other) : fAutoChecked(false),
                                                                    fValid(true),
                                                                    strBudgetName(other.strBudgetName),
                                                                    nBlockStart(other.nBlockStart),
                                                                    vecBudgetPayments(other.vecBudgetPayments),
                                                                    strInvalid(),
                                                                    mapVotes(other.mapVotes),
                                                                    nFeeTXHash(other.nFeeTXHash),
                                                                    nTime(other.nTime)
{
}

bool CFinalizedBudget::AddOrUpdateVote(const CFinalizedBudgetVote& vote, std::string& strError)
{
    LOCK(cs);

    const uint256& hash = vote.GetVin().prevout.GetHash();
    const int64_t voteTime = vote.GetTime();
    std::string strAction = "New vote inserted:";

    if (mapVotes.count(hash)) {
        const int64_t oldTime = mapVotes[hash].GetTime();
        if (oldTime > voteTime) {
            strError = strprintf("new vote older than existing vote - %s\n", vote.GetHash().ToString());
            LogPrint("mnbudget", "CFinalizedBudget::AddOrUpdateVote - %s\n", strError);
            return false;
        }
        if (voteTime - oldTime < BUDGET_VOTE_UPDATE_MIN) {
            strError = strprintf("time between votes is too soon - %s - %lli sec < %lli sec\n", vote.GetHash().ToString(), voteTime - oldTime, BUDGET_VOTE_UPDATE_MIN);
            LogPrint("mnbudget", "CFinalizedBudget::AddOrUpdateVote - %s\n", strError);
            return false;
        }
        strAction = "Existing vote updated:";
    }

    if (voteTime > GetTime() + (60 * 60)) {
        strError = strprintf("new vote is too far ahead of current time - %s - nTime %lli - Max Time %lli\n", vote.GetHash().ToString(), voteTime, GetTime() + (60 * 60));
        LogPrint("mnbudget", "CFinalizedBudget::AddOrUpdateVote - %s\n", strError);
        return false;
    }

    mapVotes[hash] = vote;
    LogPrint("mnbudget", "CFinalizedBudget::AddOrUpdateVote - %s %s\n", strAction.c_str(), vote.GetHash().ToString().c_str());
    return true;
}

UniValue CFinalizedBudget::GetVotesObject() const
{
    LOCK(cs);
    UniValue ret(UniValue::VOBJ);
    for (const auto& it : mapVotes) {
        const CFinalizedBudgetVote& vote = it.second;
        ret.push_back(std::make_pair(vote.GetVin().prevout.ToStringShort(), vote.ToJSON()));
    }
    return ret;
}

void CFinalizedBudget::SetSynced(bool synced)
{
    LOCK(cs);
    for (auto& it : mapVotes) {
        CFinalizedBudgetVote& vote = it.second;
        if (synced) {
            if (vote.IsValid())
                vote.SetSynced(true);
        } else {
            vote.SetSynced(false);
        }
    }
}


// Sort budget proposals by hash
struct sortProposalsByHash {
    bool operator()(const CBudgetProposal* left, const CBudgetProposal* right)
    {
        return (left->GetHash() < right->GetHash());
    }
};

// evaluate if we should vote for this. Masternode only
void CFinalizedBudget::CheckAndVote()
{
    if (!fMasterNode || fAutoChecked)
        return;

    // do this 1 in 4 blocks -- spread out the voting activity on mainnet
    //  -- this function is only called every fourteenth block, so this is really 1 in 56 blocks
    if (NetworkIdFromCommandLine() == CBaseChainParams::MAIN && rand() % 4 != 0) {
        LogPrint("masternode", "CFinalizedBudget::AutoCheck - waiting\n");
        return;
    }

    fAutoChecked = true; // we only need to check this once


    if (strBudgetMode == "auto") // only vote for exact matches
    {
        LOCK(cs);
        std::vector<CBudgetProposal*> vBudgetProposals = budget.GetBudget();


        for (unsigned int i = 0; i < vecBudgetPayments.size(); i++) {
            LogPrint("masternode", "CFinalizedBudget::AutoCheck - nProp %d %s\n", i, vecBudgetPayments[i].nProposalHash.ToString());
            LogPrint("masternode", "CFinalizedBudget::AutoCheck - Payee %d %s\n", i, vecBudgetPayments[i].payee.ToString());
            LogPrint("masternode", "CFinalizedBudget::AutoCheck - nAmount %d %lli\n", i, vecBudgetPayments[i].nAmount);
        }

        for (unsigned int i = 0; i < vBudgetProposals.size(); i++) {
            LogPrint("masternode", "CFinalizedBudget::AutoCheck - nProp %d %s\n", i, vBudgetProposals[i]->GetHash().ToString());
            LogPrint("masternode", "CFinalizedBudget::AutoCheck - Payee %d %s\n", i, vBudgetProposals[i]->GetPayee().ToString());
            LogPrint("masternode", "CFinalizedBudget::AutoCheck - nAmount %d %lli\n", i, vBudgetProposals[i]->GetAmount());
        }

        if (vBudgetProposals.size() == 0) {
            LogPrint("masternode", "CFinalizedBudget::AutoCheck - Can't get Budget, aborting\n");
            return;
        }

        if (vBudgetProposals.size() != vecBudgetPayments.size()) {
            LogPrint("masternode", "CFinalizedBudget::AutoCheck - Budget length doesn't match. vBudgetProposals.size()=%ld != vecBudgetPayments.size()=%ld\n",
                     vBudgetProposals.size(), vecBudgetPayments.size());
            return;
        }


        for (unsigned int i = 0; i < vecBudgetPayments.size(); i++) {
            if (i > vBudgetProposals.size() - 1) {
                LogPrint("masternode", "CFinalizedBudget::AutoCheck - Proposal size mismatch, i=%d > (vBudgetProposals.size() - 1)=%d\n", i, vBudgetProposals.size() - 1);
                return;
            }

            if (vecBudgetPayments[i].nProposalHash != vBudgetProposals[i]->GetHash()) {
                LogPrint("masternode", "CFinalizedBudget::AutoCheck - item #%d doesn't match %s %s\n", i, vecBudgetPayments[i].nProposalHash.ToString(), vBudgetProposals[i]->GetHash().ToString());
                return;
            }

            // if(vecBudgetPayments[i].payee != vBudgetProposals[i]->GetPayee()){ -- triggered with false positive
            if (vecBudgetPayments[i].payee.ToString() != vBudgetProposals[i]->GetPayee().ToString()) {
                LogPrint("masternode", "CFinalizedBudget::AutoCheck - item #%d payee doesn't match %s %s\n", i, vecBudgetPayments[i].payee.ToString(), vBudgetProposals[i]->GetPayee().ToString());
                return;
            }

            if (vecBudgetPayments[i].nAmount != vBudgetProposals[i]->GetAmount()) {
                LogPrint("masternode", "CFinalizedBudget::AutoCheck - item #%d payee doesn't match %lli %lli\n", i, vecBudgetPayments[i].nAmount, vBudgetProposals[i]->GetAmount());
                return;
            }
        }

        LogPrint("masternode", "CFinalizedBudget::AutoCheck - Finalized Budget Matches! Submitting Vote.\n");
        SubmitVote();
    }
}
// If masternode voted for a proposal, but is now invalid -- remove the vote
void CFinalizedBudget::CleanAndRemove()
{
    std::map<uint256, CFinalizedBudgetVote>::iterator it = mapVotes.begin();

    while (it != mapVotes.end()) {
        CMasternode* pmn = mnodeman.Find((*it).second.GetVin());
        (*it).second.SetValid(pmn != nullptr);
        ++it;
    }
}


CAmount CFinalizedBudget::GetTotalPayout() const
{
    CAmount ret = 0;

    for (auto& vecBudgetPayment : vecBudgetPayments) {
        ret += vecBudgetPayment.nAmount;
    }

    return ret;
}

std::string CFinalizedBudget::GetProposals()
{
    LOCK(cs);
    std::string ret = "";

    for (CTxBudgetPayment& budgetPayment : vecBudgetPayments) {
        CBudgetProposal* pbudgetProposal = budget.FindProposal(budgetPayment.nProposalHash);

        std::string token = budgetPayment.nProposalHash.ToString();

        if (pbudgetProposal)
            token = pbudgetProposal->GetName();
        if (ret == "") {
            ret = token;
        } else {
            ret += "," + token;
        }
    }
    return ret;
}

std::string CFinalizedBudget::GetStatus() const
{
    std::string retBadHashes = "";
    std::string retBadPayeeOrAmount = "";

    for (int nBlockHeight = GetBlockStart(); nBlockHeight <= GetBlockEnd(); nBlockHeight++) {
        CTxBudgetPayment budgetPayment;
        if (!GetBudgetPaymentByBlock(nBlockHeight, budgetPayment)) {
            LogPrint("masternode", "CFinalizedBudget::GetStatus - Couldn't find budget payment for block %lld\n", nBlockHeight);
            continue;
        }

        const CBudgetProposal* pbudgetProposal = budget.FindProposal(budgetPayment.nProposalHash);
        if (!pbudgetProposal) {
            if (retBadHashes == "") {
                retBadHashes = "Unknown proposal hash! Check this proposal before voting" + budgetPayment.nProposalHash.ToString();
            } else {
                retBadHashes += "," + budgetPayment.nProposalHash.ToString();
            }
        } else {
            if (pbudgetProposal->GetPayee() != budgetPayment.payee || pbudgetProposal->GetAmount() != budgetPayment.nAmount) {
                if (retBadPayeeOrAmount == "") {
                    retBadPayeeOrAmount = "Budget payee/nAmount doesn't match our proposal! " + budgetPayment.nProposalHash.ToString();
                } else {
                    retBadPayeeOrAmount += "," + budgetPayment.nProposalHash.ToString();
                }
            }
        }
    }

    if (retBadHashes == "" && retBadPayeeOrAmount == "")
        return "OK";

    return retBadHashes + retBadPayeeOrAmount;
}

void CFinalizedBudget::SyncVotes(CNode* pfrom, bool fPartial, int& nInvCount) const
{
    LOCK(cs);
    for (const auto& it : mapVotes) {
        const CFinalizedBudgetVote& vote = it.second;
        if (vote.IsValid() && (!fPartial || !vote.IsSynced())) {
            pfrom->PushInventory(CInv(MSG_BUDGET_FINALIZED_VOTE, vote.GetHash()));
            nInvCount++;
        }
    }
}

bool CFinalizedBudget::UpdateValid(int nCurrentHeight, bool fCheckCollateral)
{
    fValid = false;
    // Must be the correct block for payment to happen (once a month)
    if (nBlockStart % GetBudgetPaymentCycleBlocks() != 0) {
        strInvalid = "Invalid BlockStart";
        return false;
    }

    // The following 2 checks check the same (basically if vecBudgetPayments.size() > 100)
    if (GetBlockEnd() - nBlockStart > 100) {
        strInvalid = "Invalid BlockEnd";
        return false;
    }
    if ((int)vecBudgetPayments.size() > 100) {
        strInvalid = "Invalid budget payments count (too many)";
        return false;
    }
    if (strBudgetName == "") {
        strInvalid = "Invalid Budget Name";
        return false;
    }
    if (nBlockStart == 0) {
        strInvalid = "Budget " + strBudgetName + " Invalid BlockStart == 0";
        return false;
    }
    if (nFeeTXHash == uint256()) {
        strInvalid = "Budget " + strBudgetName + " Invalid FeeTx == 0";
        return false;
    }

    // Can only pay out 10% of the possible coins (min value of coins)
    if (GetTotalPayout() > budget.GetTotalBudget(nBlockStart)) {
        strInvalid = "Budget " + strBudgetName + " Invalid Payout (more than max)";
        return false;
    }

    std::string strError2 = "";
    if (fCheckCollateral) {
        int nConf = 0;
        if (!IsBudgetCollateralValid(nFeeTXHash, GetHash(), strError2, nTime, nConf)) {
            {
                strInvalid = "Budget " + strBudgetName + " Invalid Collateral : " + strError2;
                return false;
            }
        }
    }

    // TODO: if N cycles old, invalid, invalid

    CBlockIndex* pindexPrev = chainActive.Tip();
    if (pindexPrev == NULL)
        return true;

    // TODO: verify if we can safely remove this
    //
    //    if (nBlockStart < pindexPrev->nHeight - 100) {
    //        strError = "Budget " + strBudgetName + " Older than current blockHeight" ;
    //        return false;
    //    }
    fValid = true;
    strInvalid.clear();
    return true;
}

bool CFinalizedBudget::IsPaidAlready(uint256 nProposalHash, int nBlockHeight) const
{
    // Remove budget-payments from former/future payment cycles
    std::map<uint256, int>::iterator it = mapPayment_History.begin();
    int nPaidBlockHeight = 0;
    uint256 nOldProposalHash;

    for (it = mapPayment_History.begin(); it != mapPayment_History.end(); /* No incrementation needed */) {
        nPaidBlockHeight = (*it).second;
        if ((nPaidBlockHeight < GetBlockStart()) || (nPaidBlockHeight > GetBlockEnd())) {
            nOldProposalHash = (*it).first;
            LogPrint("mnbudget", "%s: Budget Proposal %s, Block %d from old cycle deleted\n",
                     __func__, nOldProposalHash.ToString().c_str(), nPaidBlockHeight);
            mapPayment_History.erase(it++);
        } else {
            ++it;
        }
    }

    // Now that we only have payments from the current payment cycle check if this budget was paid already
    if (mapPayment_History.count(nProposalHash) == 0) {
        // New proposal payment, insert into map for checks with later blocks from this cycle
        mapPayment_History.insert(std::pair<uint256, int>(nProposalHash, nBlockHeight));
        LogPrint("mnbudget", "%s: Budget Proposal %s, Block %d added to payment history\n",
                 __func__, nProposalHash.ToString().c_str(), nBlockHeight);
        return false;
    }
    // This budget was paid already -> reject transaction so it gets paid to a masternode instead
    return true;
}


TrxValidationStatus CFinalizedBudget::IsTransactionValid(const CTransaction& txNew, int nBlockHeight) const
{
    TrxValidationStatus transactionStatus = TrxValidationStatus::InValid;
    int nCurrentBudgetPayment = nBlockHeight - GetBlockStart();
    if (nCurrentBudgetPayment < 0) {
        LogPrint("masternode", "CFinalizedBudget::IsTransactionValid - Invalid block - height: %d start: %d\n", nBlockHeight, GetBlockStart());
        return TrxValidationStatus::InValid;
    }

    if (nCurrentBudgetPayment > (int)vecBudgetPayments.size() - 1) {
        LogPrint("masternode", "CFinalizedBudget::IsTransactionValid - Invalid block - current budget payment: %d of %d\n", nCurrentBudgetPayment + 1, (int)vecBudgetPayments.size());
        return TrxValidationStatus::InValid;
    }

    bool paid = false;
    for (const CTxOut& out : txNew.vout) {
        if (vecBudgetPayments[nCurrentBudgetPayment].payee == out.scriptPubKey && vecBudgetPayments[nCurrentBudgetPayment].nAmount == out.nValue) {
            // Check if this proposal was paid already. If so, pay a masternode instead
            paid = IsPaidAlready(vecBudgetPayments[nCurrentBudgetPayment].nProposalHash, nBlockHeight);
            if (paid) {
                LogPrint("mnbudget", "%s: Double Budget Payment of %d for proposal %d detected. Paying a masternode instead.\n",
                         __func__, vecBudgetPayments[nCurrentBudgetPayment].nAmount, vecBudgetPayments[nCurrentBudgetPayment].nProposalHash.GetHex());
                // No matter what we've found before, stop all checks here. In future releases there might be more than one budget payment
                // per block, so even if the first one was not paid yet this one disables all budget payments for this block.
                transactionStatus = TrxValidationStatus::DoublePayment;
                break;
            } else {
                transactionStatus = TrxValidationStatus::Valid;
                LogPrint("mnbudget", "%s: Found valid Budget Payment of %d for proposal %d\n", __func__,
                         vecBudgetPayments[nCurrentBudgetPayment].nAmount, vecBudgetPayments[nCurrentBudgetPayment].nProposalHash.GetHex());
            }
        }
    }

    if (transactionStatus == TrxValidationStatus::InValid) {
        CTxDestination address1;
        ExtractDestination(vecBudgetPayments[nCurrentBudgetPayment].payee, address1);
        KeyIO keyIO(Params());
        LogPrint("masternode", "CFinalizedBudget::IsTransactionValid - Missing required payment - %s: %d c: %d\n",
                 keyIO.EncodeDestination(address1), vecBudgetPayments[nCurrentBudgetPayment].nAmount, nCurrentBudgetPayment);
    }

    return transactionStatus;
}

bool CFinalizedBudget::GetBudgetPaymentByBlock(int64_t nBlockHeight, CTxBudgetPayment& payment) const
{
    LOCK(cs);

    int i = nBlockHeight - GetBlockStart();
    if (i < 0)
        return false;
    if (i > (int)vecBudgetPayments.size() - 1)
        return false;
    payment = vecBudgetPayments[i];
    return true;
}

bool CFinalizedBudget::GetPayeeAndAmount(int64_t nBlockHeight, CScript& payee, CAmount& nAmount) const
{
    LOCK(cs);

    int i = nBlockHeight - GetBlockStart();
    if (i < 0)
        return false;
    if (i > (int)vecBudgetPayments.size() - 1)
        return false;
    payee = vecBudgetPayments[i].payee;
    nAmount = vecBudgetPayments[i].nAmount;
    return true;
}

void CFinalizedBudget::SubmitVote()
{
    CPubKey pubKeyMasternode;
    CKey keyMasternode;
    bool fNewSigs = false;
    {
        fNewSigs = NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_MORAG);
    }

    if (!CMessageSigner::GetKeysFromSecret(strMasterNodePrivKey, keyMasternode, pubKeyMasternode, fNewSigs)) {
        LogPrint("mnbudget", "CFinalizedBudget::SubmitVote - Error upon calling GetKeysFromSecret\n");
        return;
    }

    CFinalizedBudgetVote vote(activeMasternode.vin, GetHash());
    if (!vote.SignMessage(keyMasternode, pubKeyMasternode, fNewSigs)) {
        LogPrint("mnbudget", "CFinalizedBudget::SubmitVote - Failure to sign.");
        return;
    }

    std::string strError = "";
    if (budget.UpdateFinalizedBudget(vote, NULL, strError)) {
        LogPrint("masternode", "CFinalizedBudget::SubmitVote  - new finalized budget vote - %s\n", vote.GetHash().ToString());

        budget.AddSeenFinalizedBudgetVote(vote);
        vote.Relay();
    } else {
        LogPrint("masternode", "CFinalizedBudget::SubmitVote : Error submitting vote - %s\n", strError);
    }
}

CFinalizedBudgetBroadcast::CFinalizedBudgetBroadcast() : CFinalizedBudget()
{
}

CFinalizedBudgetBroadcast::CFinalizedBudgetBroadcast(const CFinalizedBudget& other) : CFinalizedBudget(other)
{
}

CFinalizedBudgetBroadcast::CFinalizedBudgetBroadcast(std::string strBudgetNameIn,
                                                     int nBlockStartIn,
                                                     const std::vector<CTxBudgetPayment>& vecBudgetPaymentsIn,
                                                     uint256 nFeeTXHashIn)
{
    strBudgetName = strBudgetNameIn;
    nBlockStart = nBlockStartIn;
    for (const CTxBudgetPayment& out : vecBudgetPaymentsIn)
        vecBudgetPayments.push_back(out);
    nFeeTXHash = nFeeTXHashIn;
}

void CFinalizedBudgetBroadcast::Relay()
{
    CInv inv(MSG_BUDGET_FINALIZED, GetHash());
    RelayInv(inv);
}

CFinalizedBudgetVote::CFinalizedBudgetVote() : CSignedMessage(),
                                               fValid(true),
                                               fSynced(false),
                                               vin(),
                                               nTime(0)
{
    nBudgetHash = uint256();
    const bool fNewSigs = NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_MORAG);
    if (fNewSigs) {
        nMessVersion = MessageVersion::MESS_VER_HASH;
    }
}

CFinalizedBudgetVote::CFinalizedBudgetVote(CTxIn vinIn, uint256 nBudgetHashIn) : CSignedMessage(),
                                                                                 fValid(true),
                                                                                 fSynced(false),
                                                                                 vin(vinIn),
                                                                                 nBudgetHash(nBudgetHashIn)
{
    nTime = GetAdjustedTime();
    const bool fNewSigs = NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_MORAG);
    if (fNewSigs) {
        nMessVersion = MessageVersion::MESS_VER_HASH;
    }
}

UniValue CFinalizedBudgetVote::ToJSON() const
{
    UniValue bObj(UniValue::VOBJ);
    bObj.push_back(Pair("nHash", vin.prevout.GetHash().ToString()));
    bObj.push_back(Pair("nTime", (int64_t)nTime));
    bObj.push_back(Pair("fValid", fValid));
    return bObj;
}

void CFinalizedBudgetVote::Relay() const
{
    CInv inv(MSG_BUDGET_FINALIZED_VOTE, GetHash());
    RelayInv(inv);
}

uint256 CFinalizedBudgetVote::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << vin;
    ss << nBudgetHash;
    ss << nTime;
    return ss.GetHash();
}

std::string CFinalizedBudgetVote::GetStrMessage() const
{
    return vin.prevout.ToStringShort() + nBudgetHash.ToString() + std::to_string(nTime);
}

std::string CBudgetManager::ToString() const
{
    std::ostringstream info;

    info << "Proposals: " << (int)mapProposals.size() << ", Budgets: " << (int)mapFinalizedBudgets.size() << ", Seen Budgets: " << (int)mapSeenMasternodeBudgetProposals.size() << ", Seen Budget Votes: " << (int)mapSeenMasternodeBudgetVotes.size() << ", Seen Final Budgets: " << (int)mapSeenFinalizedBudgets.size() << ", Seen Final Budget Votes: " << (int)mapSeenFinalizedBudgetVotes.size();

    return info.str();
}
