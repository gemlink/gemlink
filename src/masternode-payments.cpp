// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2017-2018 The SnowGem developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternode-payments.h"
#include "addrman.h"
#include "masternode-budget.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "spork.h"
#include "sync.h"
#include "util.h"
#include "utilmoneystr.h"
#include <boost/filesystem.hpp>

#include "key_io.h"

/** Object for who's going to get paid on which blocks */
CMasternodePayments masternodePayments;

CCriticalSection cs_vecPayments;
CCriticalSection cs_mapMasternodeBlocks;
CCriticalSection cs_mapMasternodePayeeVotes;

//
// CMasternodePaymentDB
//

CMasternodePaymentDB::CMasternodePaymentDB()
{
    pathDB = GetDataDir() / "mnpayments.dat";
    strMagicMessage = "MasternodePayments";
}

bool CMasternodePaymentDB::Write(const CMasternodePayments& objToSave)
{
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

    LogPrint("masternode", "Written info to mnpayments.dat  %dms\n", GetTimeMillis() - nStart);

    return true;
}

CMasternodePaymentDB::ReadResult CMasternodePaymentDB::Read(CMasternodePayments& objToLoad, bool fDryRun)
{
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
            error("%s : Invalid masternode payement cache magic message", __func__);
            return IncorrectMagicMessage;
        }


        // de-serialize file header (network specific magic number) and ..
        ssObj >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp))) {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }

        // de-serialize data into CMasternodePayments object
        ssObj >> objToLoad;
    } catch (std::exception& e) {
        objToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    LogPrint("masternode", "Loaded info from mnpayments.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrint("masternode", "  %s\n", objToLoad.ToString());
    if (!fDryRun) {
        LogPrint("masternode", "Masternode payments manager - cleaning....\n");
        objToLoad.CleanPaymentList();
        LogPrint("masternode", "Masternode payments manager - result:\n");
        LogPrint("masternode", "  %s\n", objToLoad.ToString());
    }

    return Ok;
}

CMasternodePaymentWinner::CMasternodePaymentWinner() : CSignedMessage(),
                                                       vinMasternode(CTxIn()),
                                                       nBlockHeight(0),
                                                       payee(CScript())
{
    const bool fNewSigs = NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_MORAG);
    if (fNewSigs) {
        nMessVersion = MessageVersion::MESS_VER_HASH;
    }
}

CMasternodePaymentWinner::CMasternodePaymentWinner(CTxIn vinIn) : CSignedMessage(),
                                                                  vinMasternode(vinIn),
                                                                  nBlockHeight(0),
                                                                  payee(CScript())
{
    const bool fNewSigs = NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_MORAG);
    if (fNewSigs) {
        nMessVersion = MessageVersion::MESS_VER_HASH;
    }
}

uint256 CMasternodePaymentWinner::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << std::vector<unsigned char>(payee.begin(), payee.end());
    ss << nBlockHeight;
    ss << vinMasternode.prevout;

    return ss.GetHash();
}

std::string CMasternodePaymentWinner::GetStrMessage() const
{
    return vinMasternode.prevout.ToStringShort() + std::to_string(nBlockHeight) + payee.ToString();
}

bool CMasternodePaymentWinner::CheckSignature() const
{
    std::string strError = "";
    if (!CSignedMessage::CheckSignature(strError)) {
        LogPrintf("CMasternodePaymentWinner::CheckSignature Error - %s\n", strError);
        return false;
    }
    return true;
}

// TODO gemlink can remove after morag fork
bool CMasternodePaymentWinner::Sign(CKey& key, CPubKey& pubKey, bool fNewSigs)
{
    if (!CSignedMessage::SignMessage(key, pubKey, fNewSigs)) {
        LogPrint("masternode", "CMasternodePaymentWinner::Sign() - Error\n");
        return false;
    }
    return true;
}

bool CMasternodePaymentWinner::IsValid(CNode* pnode, std::string& strError)
{
    CMasternode* pmn = mnodeman.Find(vinMasternode);

    if (!pmn) {
        strError = strprintf("Unknown Masternode %s", vinMasternode.prevout.hash.ToString());
        LogPrint("masternode", "CMasternodePaymentWinner::IsValid - %s\n", strError);
        mnodeman.AskForMN(pnode, vinMasternode);
        return false;
    }

    if (pmn->protocolVersion < ActiveProtocol()) {
        strError = strprintf("Masternode protocol too old %d - req %d", pmn->protocolVersion, ActiveProtocol());
        LogPrint("masternode", "CMasternodePaymentWinner::IsValid - %s\n", strError);
        return false;
    }

    int n = mnodeman.GetMasternodeRank(vinMasternode, nBlockHeight - 100, ActiveProtocol());

    if (n > MNPAYMENTS_SIGNATURES_TOTAL) {
        // It's common to have masternodes mistakenly think they are in the top 10
        //  We don't want to print all of these messages, or punish them unless they're way off
        if (n > MNPAYMENTS_SIGNATURES_TOTAL * 2) {
            strError = strprintf("Masternode not in the top %d (%d)", MNPAYMENTS_SIGNATURES_TOTAL * 2, n);
            LogPrint("masternode", "CMasternodePaymentWinner::IsValid - %s\n", strError);
            // if (masternodeSync.IsSynced()) Misbehaving(pnode->GetId(), 20);
        }
        return false;
    }

    return true;
}

void CMasternodePaymentWinner::Relay()
{
    CInv inv(MSG_MASTERNODE_WINNER, GetHash());
    RelayInv(inv);
}

void DumpMasternodePayments()
{
    int64_t nStart = GetTimeMillis();

    CMasternodePaymentDB paymentdb;
    CMasternodePayments tempPayments;

    LogPrint("masternode", "Verifying mnpayments.dat format...\n");
    CMasternodePaymentDB::ReadResult readResult = paymentdb.Read(tempPayments, true);
    // there was an error and it was not an error on file opening => do not proceed
    if (readResult == CMasternodePaymentDB::FileError)
        LogPrint("masternode", "Missing budgets file - mnpayments.dat, will try to recreate\n");
    else if (readResult != CMasternodePaymentDB::Ok) {
        LogPrint("masternode", "Error reading mnpayments.dat: ");
        if (readResult == CMasternodePaymentDB::IncorrectFormat)
            LogPrint("masternode", "magic is ok but data has invalid format, will try to recreate\n");
        else {
            LogPrint("masternode", "file format is unknown or invalid, please fix it manually\n");
            return;
        }
    }
    LogPrint("masternode", "Writting info to mnpayments.dat...\n");
    paymentdb.Write(masternodePayments);

    LogPrint("masternode", "Budget dump finished  %dms\n", GetTimeMillis() - nStart);
}

bool IsBlockValueValid(int nHeight, const CBlock& block, CAmount nExpectedValue)
{
    if (!masternodeSync.IsSynced()) {
        // there is no budget data to use to check anything
        // super blocks will always be on these blocks, max 100 per budgeting
        if (sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS) && nHeight % GetBudgetPaymentCycleBlocks() < 100) {
            return true;
        }
    } else {
        // we're synced and have data so check the budget schedule
        // if the superblock spork is enabled
        if (sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS) &&
            budget.IsBudgetPaymentBlock(nHeight)) {
            // the value of the block is evaluated in CheckBlock
            return true;
        }
    }
    return block.vtx[0].GetValueOut() <= nExpectedValue;
}

bool IsBlockPayeeValid(const CChainParams& chainparams, const CBlock& block, int nBlockHeight)
{
    TrxValidationStatus transactionStatus = TrxValidationStatus::InValid;
    if (!masternodeSync.IsSynced()) { // there is no budget data to use to check anything -- find the longest chain
        LogPrint("masternodepayments", "Client not synced, skipping block payee checks\n");
        return true;
    }

    const CTransaction& txNew = block.vtx[0];

    // check if it's a budget block
    if (sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS)) {
        if (budget.IsBudgetPaymentBlock(nBlockHeight)) {
            transactionStatus = budget.IsTransactionValid(txNew, nBlockHeight);
            if (transactionStatus == TrxValidationStatus::Valid) {
                return true;
            }

            if (sporkManager.IsSporkActive(SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT)) {
                LogPrintf("Invalid budget payment detected %s\n", txNew.ToString().c_str());
                return false;
            }

            LogPrint("masternodepayments", "Budget enforcement is disabled, accepting block\n");
            return true;
        }
    }

    // check for masternode payee
    if (masternodePayments.IsTransactionValid(chainparams, txNew, nBlockHeight))
        return true;

    LogPrintf("Invalid mn payment detected %s\n", txNew.ToString().c_str());

    if (!Params().GetConsensus().NetworkUpgradeActive(chainActive.Height() + 1, Consensus::UPGRADE_MORAG) && sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT)) {
        return false;
    } else if (sporkManager.IsSporkActive(SPORK_19_MASTERNODE_PAYMENT_ENFORCEMENT_MORAG)) {
        return false;
    }
    LogPrint("masternodepayments", "Masternode payment enforcement is disabled, accepting block\n");

    return true;
}


void FillBlockPayee(CMutableTransaction& txNew, CAmount nFees, CScript& payee)
{
    CBlockIndex* pindexPrev = chainActive.Tip();
    if (!pindexPrev)
        return;

    if (sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS) && budget.IsBudgetPaymentBlock(pindexPrev->nHeight + 1)) {
        budget.FillBlockPayee(txNew, payee);
    } else {
        masternodePayments.FillBlockPayee(txNew, nFees, payee);
    }
}

std::string GetRequiredPaymentsString(int nBlockHeight)
{
    if (sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS) && budget.IsBudgetPaymentBlock(nBlockHeight)) {
        return budget.GetRequiredPaymentsString(nBlockHeight);
    } else {
        return masternodePayments.GetRequiredPaymentsString(nBlockHeight);
    }
}

void CMasternodePayments::FillBlockPayee(CMutableTransaction& txNew, int64_t nFees, CScript& payee)
{
    CBlockIndex* pindexPrev = chainActive.Tip();
    if (!pindexPrev)
        return;

    bool hasPayment = true;
    int nHeight = pindexPrev->nHeight + 1;

    // spork
    if (!masternodePayments.GetBlockPayee(nHeight, payee)) {
        // no masternode detected
        CMasternode* winningNode = mnodeman.GetCurrentMasterNode(1);
        if (winningNode) {
            payee = GetScriptForDestination(winningNode->pubKeyCollateralAddress.GetID());
        } else {
            LogPrint("masternode", "CreateNewBlock: Failed to detect masternode to pay\n");
            payee = CScript();
            hasPayment = false;
        }
    }

    CAmount blockValue = GetBlockSubsidy(nHeight, Params().GetConsensus());

    CAmount masternodePayment = GetMasternodePayment(nHeight, blockValue);

    txNew.vout[0].nValue = blockValue + nFees;

    if ((nHeight > 0) && (nHeight <= Params().GetConsensus().GetLastFoundersRewardBlockHeight()) && !Params().GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_MORAG)) {
        // Founders reward
        CAmount vFoundersReward = 0;
        if (nHeight < Params().GetConsensus().vUpgrades[Consensus::UPGRADE_OVERWINTER].nActivationHeight) {
            vFoundersReward = blockValue / 20;
        } else if (nHeight < Params().GetConsensus().vUpgrades[Consensus::UPGRADE_KNOWHERE].nActivationHeight) {
            vFoundersReward = blockValue * 7.5 / 100;
        } else if (nHeight < Params().GetConsensus().vUpgrades[Consensus::UPGRADE_MORAG].nActivationHeight) {
            vFoundersReward = blockValue * 15 / 100;
        }

        // Take some reward away from us
        txNew.vout[0].nValue -= vFoundersReward;

        // And give it to the founders
        txNew.vout.push_back(CTxOut(vFoundersReward, Params().GetFoundersRewardScriptAtHeight(nHeight)));
    }

    if ((nHeight > 0) && (nHeight <= Params().GetConsensus().GetLastTreasuryRewardBlockHeight())) {
        // Treasury reward
        CAmount vTreasuryReward = 0;

        if (nHeight >= Params().GetConsensus().vUpgrades[Consensus::UPGRADE_KNOWHERE].nActivationHeight &&
            !NetworkUpgradeActive(nHeight, Params().GetConsensus(), Consensus::UPGRADE_ATLANTIS)) {
            vTreasuryReward = blockValue * 5 / 100;
        } else if (!NetworkUpgradeActive(nHeight, Params().GetConsensus(), Consensus::UPGRADE_MORAG)) {
            vTreasuryReward = blockValue * 10 / 100;
        }

        if (nHeight >= Params().GetConsensus().vUpgrades[Consensus::UPGRADE_KNOWHERE].nActivationHeight) {
            txNew.vout[0].nValue -= vTreasuryReward;
            txNew.vout.push_back(CTxOut(vTreasuryReward, Params().GetTreasuryRewardScriptAtHeight(nHeight)));
        }
    }

    if (Params().GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_MORAG) &&
        nHeight <= Params().GetConsensus().GetLastDevelopersRewardBlockHeight()) {
        const CAmount vDevelopersReward = GetDevelopersPayment(nHeight, blockValue);

        // And give it to the developers
        txNew.vout.push_back(CTxOut(vDevelopersReward, Params().GetDevelopersRewardScriptAtHeight(nHeight)));

        // Take some reward away from us
        txNew.vout[0].nValue -= vDevelopersReward;
    }

    if (nHeight == Params().GetConsensus().vUpgrades[Consensus::UPGRADE_MORAG].nActivationHeight) {
        txNew.vout.push_back(CTxOut(GetPremineAmountAtHeight(nHeight), Params().GetDevelopersRewardScriptAtHeight(nHeight)));

        txNew.vout[0].nValue -= GetPremineAmountAtHeight(nHeight);
    }

    //@TODO masternode
    if (hasPayment) {
        txNew.vout.push_back(CTxOut(masternodePayment, payee));
        txNew.vout[0].nValue -= masternodePayment;
    }
}

int CMasternodePayments::GetMinMasternodePaymentsProto()
{
    int minPeer = MIN_PEER_PROTO_VERSION_ENFORCEMENT;
    int nHeight = 0;
    {
        TRY_LOCK(cs_main, locked);
        if (!locked || chainActive.Tip() == NULL)
            return MIN_PEER_PROTO_VERSION_ENFORCEMENT;
        nHeight = chainActive.Tip()->nHeight;
    }
    if (NetworkUpgradeActive(nHeight, Params().GetConsensus(), Consensus::UPGRADE_MORAG)) {
        minPeer = MIN_PEER_PROTO_VERSION_ENFORCEMENT_MORAG;
    } else if (NetworkUpgradeActive(nHeight, Params().GetConsensus(), Consensus::UPGRADE_XANDAR)) {
        minPeer = MIN_PEER_PROTO_VERSION_ENFORCEMENT_XANDAR;
    }
    return minPeer;
}

void CMasternodePayments::ProcessMessageMasternodePayments(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (!masternodeSync.IsBlockchainSynced())
        return;

    if (fLiteMode)
        return; // disable all Obfuscation/Masternode related functionality


    if (strCommand == "mnget") { // Masternode Payments Request Sync
        if (fLiteMode)
            return; // disable all Obfuscation/Masternode related functionality

        int nCountNeeded;
        vRecv >> nCountNeeded;

        if (NetworkIdFromCommandLine() == CBaseChainParams::MAIN) {
            if (pfrom->HasFulfilledRequest("mnget")) {
                LogPrint("masternode", "mnget - peer already asked me for the list\n");
                Misbehaving(pfrom->GetId(), 20);
                return;
            }
        }

        pfrom->FulfilledRequest("mnget");
        masternodePayments.Sync(pfrom, nCountNeeded);
        LogPrint("mnpayments", "mnget - Sent Masternode winners to peer %i\n", pfrom->GetId());
    } else if (strCommand == "mnw") { // Masternode Payments Declare Winner
        // this is required in litemodef
        CMasternodePaymentWinner winner;
        vRecv >> winner;

        if (pfrom->nVersion < ActiveProtocol())
            return;

        int nHeight;
        {
            TRY_LOCK(cs_main, locked);
            if (!locked || chainActive.Tip() == NULL)
                return;
            nHeight = chainActive.Tip()->nHeight;
        }

        if (masternodePayments.mapMasternodePayeeVotes.count(winner.GetHash())) {
            LogPrint("mnpayments", "mnw - Already seen - %s bestHeight %d\n", winner.GetHash().ToString().c_str(), nHeight);
            masternodeSync.AddedMasternodeWinner(winner.GetHash());
            return;
        }

        int nCount = (mnodeman.CountEnabled() * 1.25);

        int nFirstBlock = nHeight - nCount;

        if (winner.nBlockHeight < nFirstBlock || winner.nBlockHeight > nHeight + 20) {
            LogPrint("mnpayments", "mnw - winner out of range - FirstBlock %d Height %d bestHeight %d\n", nFirstBlock, winner.nBlockHeight, nHeight);
            return;
        }

        std::string strError = "";
        if (!winner.IsValid(pfrom, strError)) {
            if (strError != "")
                LogPrint("masternode", "mnw - invalid message - %s\n", strError);
            return;
        }

        if (!masternodePayments.CanVote(winner.vinMasternode.prevout, winner.nBlockHeight)) {
            LogPrint("masternode", "mnw - masternode already voted - %s\n", winner.vinMasternode.prevout.ToStringShort());
            return;
        }

        if (!winner.CheckSignature()) {
            LogPrint("masternode", "mnw - invalid signature\n");
            if (masternodeSync.IsSynced()) {
                Misbehaving(pfrom->GetId(), 20);
            }
            // it could just be a non-synced masternode
            mnodeman.AskForMN(pfrom, winner.vinMasternode);
            return;
        }

        CTxDestination address1;
        ExtractDestination(winner.payee, address1);

        //   LogPrint("mnpayments", "mnw - winning vote - Addr %s Height %d bestHeight %d - %s\n", EncodeDestination(address1), winner.nBlockHeight, nHeight, winner.vinMasternode.prevout.ToStringShort());

        if (masternodePayments.AddWinningMasternode(winner)) {
            winner.Relay();
            masternodeSync.AddedMasternodeWinner(winner.GetHash());
        }
    }
}


bool CMasternodePayments::GetBlockPayee(int nBlockHeight, CScript& payee)
{
    if (mapMasternodeBlocks.count(nBlockHeight)) {
        return mapMasternodeBlocks[nBlockHeight].GetPayee(payee);
    }

    return false;
}

// Is this masternode scheduled to get paid soon?
// -- Only look ahead up to 8 blocks to allow for propagation of the latest 2 winners
bool CMasternodePayments::IsScheduled(CMasternode& mn, int nNotBlockHeight)
{
    LOCK(cs_mapMasternodeBlocks);

    int nHeight;
    {
        TRY_LOCK(cs_main, locked);
        if (!locked || chainActive.Tip() == NULL)
            return false;
        nHeight = chainActive.Tip()->nHeight;
    }

    CScript mnpayee;
    mnpayee = GetScriptForDestination(mn.pubKeyCollateralAddress.GetID());

    CScript payee;
    for (int64_t h = nHeight; h <= nHeight + 8; h++) {
        if (h == nNotBlockHeight)
            continue;
        if (mapMasternodeBlocks.count(h)) {
            if (mapMasternodeBlocks[h].GetPayee(payee)) {
                if (mnpayee == payee) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool CMasternodePayments::AddWinningMasternode(CMasternodePaymentWinner& winnerIn)
{
    uint256 blockHash = uint256();

    if (!GetBlockHash(blockHash, winnerIn.nBlockHeight - 100)) {
        return false;
    }

    {
        LOCK2(cs_mapMasternodeBlocks, cs_mapMasternodePayeeVotes);

        if (mapMasternodePayeeVotes.count(winnerIn.GetHash())) {
            return false;
        }

        mapMasternodePayeeVotes[winnerIn.GetHash()] = winnerIn;

        if (!mapMasternodeBlocks.count(winnerIn.nBlockHeight)) {
            CMasternodeBlockPayees blockPayees(winnerIn.nBlockHeight);
            mapMasternodeBlocks[winnerIn.nBlockHeight] = blockPayees;
        }
    }

    mapMasternodeBlocks[winnerIn.nBlockHeight].AddPayee(winnerIn.payee, 1);

    return true;
}

bool CMasternodeBlockPayees::IsTransactionValid(const CChainParams& chainparams, const CTransaction& txNew)
{
    LOCK(cs_vecPayments);

    int nMaxSignatures = 0;

    for (CMasternodePayee& payee : vecPayments)
        if (payee.nVotes >= nMaxSignatures && payee.nVotes >= MNPAYMENTS_SIGNATURES_REQUIRED)
            nMaxSignatures = payee.nVotes;

    // if we don't have at least 6 signatures on a payee, approve whichever is the longest chain
    if (nMaxSignatures < MNPAYMENTS_SIGNATURES_REQUIRED)
        return true;

    std::string strPayeesPossible = "";
    CAmount nReward = GetBlockSubsidy(nBlockHeight, chainparams.GetConsensus());
    CAmount requiredMasternodePayment = GetMasternodePayment(nBlockHeight, nReward);

    for (CMasternodePayee& payee : vecPayments) {
        bool found = false;
        for (CTxOut out : txNew.vout) {
            if (payee.scriptPubKey == out.scriptPubKey) {
                LogPrint("masternode", "Masternode payment Paid=%s Min=%s\n", FormatMoney(out.nValue).c_str(), FormatMoney(requiredMasternodePayment).c_str());
                if (out.nValue >= requiredMasternodePayment)
                    found = true;
                else
                    LogPrint("masternode", "Masternode payment is out of drift range");
            }
        }

        if (payee.nVotes >= MNPAYMENTS_SIGNATURES_REQUIRED) {
            if (found)
                return true;

            CTxDestination address1;
            ExtractDestination(payee.scriptPubKey, address1);
            CBitcoinAddress address2(address1);

            if (strPayeesPossible == "") {
                strPayeesPossible += address2.ToString();
            } else {
                strPayeesPossible += "," + address2.ToString();
            }
        }
    }

    LogPrint("mnpaymentpayee", "Transaction output: ");
    for (CTxOut out : txNew.vout) {
        LogPrint("mnpaymentpayee", "%ld,", out.nValue);
    }
    LogPrint("mnpaymentpayee", "\nCMasternodePayments::IsTransactionValid - Missing required payment of %s to %s\n", FormatMoney(requiredMasternodePayment).c_str(), strPayeesPossible.c_str());
    return false;
}

std::string CMasternodeBlockPayees::GetRequiredPaymentsString()
{
    LOCK(cs_vecPayments);

    std::string ret = "Unknown";
    KeyIO keyIO(Params());
    for (CMasternodePayee& payee : vecPayments) {
        CTxDestination address1;
        ExtractDestination(payee.scriptPubKey, address1);

        if (ret != "Unknown") {
            ret += ", " + keyIO.EncodeDestination(address1) + ":" + std::to_string(payee.nVotes);
        } else {
            ret = keyIO.EncodeDestination(address1) + ":" + std::to_string(payee.nVotes);
        }
    }

    return ret;
}

std::string CMasternodePayments::GetRequiredPaymentsString(int nBlockHeight)
{
    LOCK(cs_mapMasternodeBlocks);

    if (mapMasternodeBlocks.count(nBlockHeight)) {
        return mapMasternodeBlocks[nBlockHeight].GetRequiredPaymentsString();
    }

    return "Unknown";
}

bool CMasternodePayments::IsTransactionValid(const CChainParams& chainparams, const CTransaction& txNew, int nBlockHeight)
{
    LOCK(cs_mapMasternodeBlocks);

    LogPrint("masternode", "mapMasternodeBlocks size = %d, nBlockHeight = %d", mapMasternodeBlocks.size(), nBlockHeight);
    if (mapMasternodeBlocks.count(nBlockHeight)) {
        LogPrint("masternode", "mapMasternodeBlocks check transaction");
        return mapMasternodeBlocks[nBlockHeight].IsTransactionValid(chainparams, txNew);
    }

    return true;
}

void CMasternodePayments::CleanPaymentList()
{
    LOCK2(cs_mapMasternodeBlocks, cs_mapMasternodePayeeVotes);

    int nHeight;
    {
        TRY_LOCK(cs_main, locked);
        if (!locked || chainActive.Tip() == NULL)
            return;
        nHeight = chainActive.Tip()->nHeight;
    }

    // keep up to five cycles for historical sake
    int nLimit = std::max(int(mnodeman.size() * 1.25), 1000);

    std::map<uint256, CMasternodePaymentWinner>::iterator it = mapMasternodePayeeVotes.begin();
    while (it != mapMasternodePayeeVotes.end()) {
        CMasternodePaymentWinner winner = (*it).second;

        if (nHeight - winner.nBlockHeight > nLimit) {
            LogPrint("mnpayments", "CMasternodePayments::CleanPaymentList - Removing old Masternode payment - block %d\n", winner.nBlockHeight);
            masternodeSync.mapSeenSyncMNW.erase((*it).first);
            mapMasternodePayeeVotes.erase(it++);
            mapMasternodeBlocks.erase(winner.nBlockHeight);
        } else {
            ++it;
        }
    }
}

bool CMasternodePayments::ProcessBlock(int nBlockHeight)
{
    if (!fMasterNode)
        return false;

    // reference node - hybrid mode

    int n = mnodeman.GetMasternodeRank(activeMasternode.vin, nBlockHeight - 100, ActiveProtocol());

    if (n == -1) {
        LogPrint("masternode", "CMasternodePayments::ProcessBlock - Unknown Masternode\n");
        return false;
    }

    if (n > MNPAYMENTS_SIGNATURES_TOTAL) {
        LogPrint("masternode", "CMasternodePayments::ProcessBlock - Masternode not in the top %d (%d)\n", MNPAYMENTS_SIGNATURES_TOTAL, n);
        return false;
    }

    if (nBlockHeight <= nLastBlockHeight)
        return false;

    CMasternodePaymentWinner newWinner(activeMasternode.vin);

    if (budget.IsBudgetPaymentBlock(nBlockHeight)) {
        // is budget payment block -- handled by the budgeting software
    } else {
        LogPrint("masternode", "CMasternodePayments::ProcessBlock() Start nHeight %d - vin %s. \n", nBlockHeight, activeMasternode.vin.prevout.hash.ToString());

        // pay to the oldest MN that still had no payment but its input is old enough and it was active long enough
        int nCount = 0;
        CMasternode* pmn = mnodeman.GetNextMasternodeInQueueForPayment(nBlockHeight, true, nCount);
        KeyIO keyIO(Params());
        if (pmn != NULL) {
            LogPrint("masternode", "CMasternodePayments::ProcessBlock() Found by FindOldestNotInVec \n");

            newWinner.nBlockHeight = nBlockHeight;

            CScript payee = GetScriptForDestination(pmn->pubKeyCollateralAddress.GetID());
            newWinner.AddPayee(payee);

            CTxDestination address1;
            ExtractDestination(payee, address1);

            LogPrint("masternode", "CMasternodePayments::ProcessBlock() Winner payee %s nHeight %d. \n", keyIO.EncodeDestination(address1), newWinner.nBlockHeight);
        } else {
            LogPrint("masternode", "CMasternodePayments::ProcessBlock() Failed to find masternode to pay\n");
        }
    }

    bool fNewSigs = false;
    {
        fNewSigs = NetworkUpgradeActive(chainActive.Height() - 20, Params().GetConsensus(), Consensus::UPGRADE_MORAG);
    }
    std::string errorMessage;
    CPubKey pubKeyMasternode;
    CKey keyMasternode;

    if (!CMessageSigner::GetKeysFromSecret(strMasterNodePrivKey, keyMasternode, pubKeyMasternode, fNewSigs)) {
        LogPrint("masternode", "CMasternodePayments::ProcessBlock() - Error upon calling GetKeysFromSecret.\n");
        return false;
    }

    LogPrint("masternode", "CMasternodePayments::ProcessBlock() - Signing Winner\n");
    if (newWinner.Sign(keyMasternode, pubKeyMasternode, fNewSigs)) {
        LogPrint("masternode", "CMasternodePayments::ProcessBlock() - AddWinningMasternode\n");

        if (AddWinningMasternode(newWinner)) {
            newWinner.Relay();
            nLastBlockHeight = nBlockHeight;
            return true;
        }
    }

    return false;
}

void CMasternodePayments::Sync(CNode* node, int nCountNeeded)
{
    LOCK(cs_mapMasternodePayeeVotes);

    int nHeight;
    {
        TRY_LOCK(cs_main, locked);
        if (!locked || chainActive.Tip() == NULL)
            return;
        nHeight = chainActive.Tip()->nHeight;
    }

    int nCount = (mnodeman.CountEnabled() * 1.25);

    if (nCountNeeded > nCount)
        nCountNeeded = nCount;

    int nInvCount = 0;
    std::map<uint256, CMasternodePaymentWinner>::iterator it = mapMasternodePayeeVotes.begin();
    while (it != mapMasternodePayeeVotes.end()) {
        CMasternodePaymentWinner winner = (*it).second;
        if (winner.nBlockHeight >= nHeight - nCountNeeded && winner.nBlockHeight <= nHeight + 20) {
            node->PushInventory(CInv(MSG_MASTERNODE_WINNER, winner.GetHash()));
            nInvCount++;
        }
        ++it;
    }
    node->PushMessage("ssc", MASTERNODE_SYNC_MNW, nInvCount);
}

std::string CMasternodePayments::ToString() const
{
    std::ostringstream info;

    info << "Votes: " << (int)mapMasternodePayeeVotes.size() << ", Blocks: " << (int)mapMasternodeBlocks.size();

    return info.str();
}
