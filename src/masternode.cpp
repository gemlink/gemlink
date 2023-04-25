// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2017-2018 The SnowGem developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternode.h"
#include "addrman.h"
#include "consensus/validation.h"
#include "init.h"
#include "masternode-payments.h"
#include "masternode-sync.h"
#include "masternodeconfig.h"
#include "masternodeman.h"
#include "sync.h"
#include "util.h"
#include "wallet/wallet.h"

#include "key_io.h"

// keep track of the scanning errors I've seen
map<uint256, int> mapSeenMasternodeScanningErrors;
// cache block hashes as we calculate them
std::map<int64_t, uint256> mapCacheBlockHashes;

// Get the last hash that matches the modulus given. Processed in reverse order
bool GetBlockHash(uint256& hash, int nBlockHeight)
{
    const CBlockIndex* tipIndex = GetChainTip();
    if (!tipIndex || !tipIndex->nHeight)
        return false;

    if (nBlockHeight == 0)
        nBlockHeight = tipIndex->nHeight;

    if (mapCacheBlockHashes.count(nBlockHeight)) {
        hash = mapCacheBlockHashes[nBlockHeight];
        return true;
    }

    int nBlocksAgo = 0;
    if (nBlockHeight > 0)
        nBlocksAgo = (tipIndex->nHeight + 1) - nBlockHeight;
    if (nBlocksAgo < 0)
        return false;

    const CBlockIndex* BlockReading = tipIndex;

    int n = 0;
    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if (n >= nBlocksAgo) {
            hash = BlockReading->GetBlockHash();
            mapCacheBlockHashes[nBlockHeight] = hash;
            return true;
        }
        n++;

        if (BlockReading->pprev == NULL) {
            assert(BlockReading);
            break;
        }
        BlockReading = BlockReading->pprev;
    }

    return false;
}

CMasternode::CMasternode() : CSignedMessage()
{
    LOCK(cs);
    vin = CTxIn();
    addr = CService();
    pubKeyCollateralAddress = CPubKey();
    pubKeyMasternode = CPubKey();
    sigTime = GetAdjustedTime();
    lastPing = CMasternodePing();
    unitTest = false;
    allowFreeTx = true;
    activeState = MASTERNODE_ENABLED,
    protocolVersion = PROTOCOL_VERSION;
    nLastDsq = 0;
    nScanningErrorCount = 0;
    nLastScanningErrorBlockHeight = 0;
    lastTimeChecked = 0;
    bool fNewSigs = false;
    {
        fNewSigs = NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_MORAG);
    }
    if (fNewSigs) {
        nMessVersion = MessageVersion::MESS_VER_HASH;
    }
}

CMasternode::CMasternode(const CMasternode& other) : CSignedMessage(other)
{
    LOCK(cs);
    vin = other.vin;
    addr = other.addr;
    pubKeyCollateralAddress = other.pubKeyCollateralAddress;
    pubKeyMasternode = other.pubKeyMasternode;
    activeState = other.activeState;
    sigTime = other.sigTime;
    lastPing = other.lastPing;
    unitTest = other.unitTest;
    allowFreeTx = other.allowFreeTx;
    protocolVersion = other.protocolVersion;
    nLastDsq = other.nLastDsq;
    nScanningErrorCount = other.nScanningErrorCount;
    nLastScanningErrorBlockHeight = other.nLastScanningErrorBlockHeight;
    lastTimeChecked = 0;
    nMessVersion = other.nMessVersion;
}

uint256 CMasternode::GetSignatureHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << nMessVersion;
    ss << addr;
    ss << sigTime;
    ss << pubKeyCollateralAddress;
    ss << pubKeyMasternode;
    ss << protocolVersion;
    return ss.GetHash();
}

std::string CMasternode::GetStrMessage() const
{
    std::string strMessage;

    std::string vchPubKey(pubKeyCollateralAddress.begin(), pubKeyCollateralAddress.end());
    std::string vchPubKey2(pubKeyMasternode.begin(), pubKeyMasternode.end());
    strMessage = addr.ToString() + std::to_string(sigTime) + vchPubKey + vchPubKey2 + std::to_string(protocolVersion);
    return strMessage;
}

//
// When a new masternode broadcast is sent, update our information
//
bool CMasternode::UpdateFromNewBroadcast(CMasternodeBroadcast& mnb)
{
    if (mnb.sigTime > sigTime) {
        pubKeyMasternode = mnb.pubKeyMasternode;
        pubKeyCollateralAddress = mnb.pubKeyCollateralAddress;
        sigTime = mnb.sigTime;
        vchSig = mnb.vchSig;
        protocolVersion = mnb.protocolVersion;
        addr = mnb.addr;
        lastTimeChecked = 0;
        int nDoS = 0;
        if (NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_XANDAR)) {
            if (IsEnabled() || IsUnlocking() || IsReEnabled()) {
                activeState = MASTERNODE_RE_ENABLED;
            }
        }
        if (mnb.lastPing.IsNull() || (!mnb.lastPing.IsNull() && mnb.lastPing.CheckAndUpdate(nDoS, false))) {
            lastPing = mnb.lastPing;
            mnodeman.mapSeenMasternodePing.insert(make_pair(lastPing.GetHash(), lastPing));
        }
        return true;
    }
    return false;
}

//
// Deterministically calculate a given "score" for a Masternode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
arith_uint256 CMasternode::CalculateScore(int64_t nBlockHeight) const
{
    {
        LOCK(cs_main);
        if (chainActive.Tip() == NULL)
            return arith_uint256();
    }

    uint256 hash;
    if (!GetBlockHash(hash, nBlockHeight)) {
        LogPrint("masternode", "CalculateScore ERROR - nHeight %d - Returned 0\n", nBlockHeight);
        return arith_uint256();
    }

    uint256 aux = ArithToUint256(UintToArith256(vin.prevout.hash) + vin.prevout.n);

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << hash;
    arith_uint256 hash2 = UintToArith256(ss.GetHash());

    CHashWriter ss2(SER_GETHASH, PROTOCOL_VERSION);
    ss2 << hash;
    ss2 << aux;
    arith_uint256 hash3 = UintToArith256(ss2.GetHash());

    return (hash3 > hash2 ? hash3 - hash2 : hash2 - hash3);
}

void CMasternode::Check(bool forceCheck)
{
    if (ShutdownRequested())
        return;

    if (!forceCheck && (GetTime() - lastTimeChecked < MASTERNODE_CHECK_SECONDS))
        return;
    lastTimeChecked = GetTime();


    // once spent, stop doing the checks
    if (activeState == MASTERNODE_VIN_SPENT)
        return;

    if (!IsPingedWithin(GetRemovalTime())) {
        activeState = MASTERNODE_REMOVE;
        return;
    }

    if (!IsPingedWithin(GetExpirationTime())) {
        activeState = MASTERNODE_EXPIRED;
        return;
    }

    bool isXandarActive = NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_XANDAR);

    if (lastPing.sigTime - sigTime < MASTERNODE_MIN_MNP_SECONDS) {
        if (isXandarActive == false || (true == isXandarActive && activeState != MASTERNODE_RE_ENABLED)) {
            activeState = MASTERNODE_PRE_ENABLED;
        }
        return;
    }

    if (!unitTest) {
        CValidationState state;
        CMutableTransaction tx = CMutableTransaction();
        CScript dummyScript;
        dummyScript << ToByteVector(pubKeyCollateralAddress) << OP_CHECKSIG;

        CTxOut vout = CTxOut(Params().GetMasternodeCollateral(chainActive.Height() + 1) * COIN - (CAmount)(0.01 * COIN), dummyScript);
        if (NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_MORAG)) {
            CAmount testingAmount = Params().GetMasternodeCollateral(chainActive.Height() + 1) * COIN - (CAmount)(0.01 * COIN);
            vout = CTxOut(testingAmount, dummyScript);
        }
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);
        {
            TRY_LOCK(cs_main, lockMain);
            if (!lockMain)
                return;

            if (!AcceptableInputs(mempool, state, CTransaction(tx), false, NULL)) {
                activeState = MASTERNODE_VIN_SPENT;
                return;
            }
        }
    }

    // ifit's active, move it to unlocking state
    if (true == isXandarActive && !IsPingedWithin(Params().GetMnStartUnlockTime()) && activeState != MASTERNODE_UNLOCKING) {
        activeState = MASTERNODE_UNLOCKING;
        return;
    }

    if (activeState == MASTERNODE_UNLOCKING)
        return;

    activeState = MASTERNODE_ENABLED; // OK
}

int64_t CMasternode::SecondsSincePayment()
{
    CScript pubkeyScript;
    pubkeyScript = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    int64_t sec = (GetAdjustedTime() - GetLastPaid());
    int64_t month = 60 * 60 * 24 * 30;
    if (sec < month)
        return sec; // if it's less than 30 days, give seconds

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << vin;
    ss << sigTime;
    uint256 hash = ss.GetHash();

    // return some deterministic value for unknown/unpaid but force it to be more than 30 days old
    return month + (UintToArith256(hash)).GetCompact(false);
}

int64_t CMasternode::GetLastPaid()
{
    const CBlockIndex* BlockReading = GetChainTip();
    if (BlockReading == nullptr)
        return false;

    CScript mnpayee;
    mnpayee = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << vin;
    ss << sigTime;
    uint256 hash = ss.GetHash();

    // use a deterministic offset to break a tie -- 2.5 minutes
    int64_t nOffset = (UintToArith256(hash)).GetCompact(false) % 150;

    int nMnCount = mnodeman.CountEnabled() * 1.25;
    int n = 0;
    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if (n >= nMnCount) {
            return 0;
        }
        n++;

        if (masternodePayments.mapMasternodeBlocks.count(BlockReading->nHeight)) {
            /*
                Search for this payee, with at least 2 votes. This will aid in consensus allowing the network
                to converge on the same payees quickly, then keep the same schedule.
            */
            if (masternodePayments.mapMasternodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(mnpayee, 2)) {
                return BlockReading->nTime + nOffset;
            }
        }

        if (BlockReading->pprev == NULL) {
            assert(BlockReading);
            break;
        }
        BlockReading = BlockReading->pprev;
    }

    return 0;
}

bool CMasternode::IsValidNetAddr()
{
    // TODO: regtest is fine with any addresses for now,
    // should probably be a bit smarter if one day we start to implement tests for this
    return NetworkIdFromCommandLine() == CBaseChainParams::REGTEST ||
           (IsReachable(addr) && addr.IsRoutable());
}
bool CMasternode::IsInputAssociatedWithPubkey() const
{
    CScript payee;
    payee = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    CTransaction txVin;
    uint256 hash;
    if (GetTransaction(vin.prevout.hash, txVin, Params().GetConsensus(), hash, true)) {
        for (CTxOut out : txVin.vout) {
            if (out.nValue == Params().GetMasternodeCollateral(chainActive.Height()) * COIN && out.scriptPubKey == payee)
                return true;
        }
    }

    return false;
}

int CMasternode::GetExpirationTime()
{
    if (NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_XANDAR)) {
        return MASTERNODE_EXPIRATION_SECONDS + Params().GetMnLockTime();
    } else {
        return MASTERNODE_EXPIRATION_SECONDS;
    }
}

int CMasternode::GetRemovalTime()
{
    if (NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_XANDAR)) {
        return MASTERNODE_REMOVAL_SECONDS + Params().GetMnLockTime();
    } else {
        return MASTERNODE_REMOVAL_SECONDS;
    }
}

CMasternodeBroadcast::CMasternodeBroadcast() : CMasternode()
{
}

CMasternodeBroadcast::CMasternodeBroadcast(CService newAddr, CTxIn newVin, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyMasternodeNew, int protocolVersionIn) : CMasternode()
{
    vin = newVin;
    addr = newAddr;
    pubKeyCollateralAddress = pubKeyCollateralAddressNew;
    pubKeyMasternode = pubKeyMasternodeNew;
    protocolVersion = protocolVersionIn;
}

CMasternodeBroadcast::CMasternodeBroadcast(const CMasternode& mn) : CMasternode(mn)
{
}

bool CMasternodeBroadcast::Create(std::string strService, std::string strKeyMasternode, std::string strTxHash, std::string strOutputIndex, std::string& strErrorRet, CMasternodeBroadcast& mnbRet, bool fOffline)
{
    CTxIn txin;
    CPubKey pubKeyCollateralAddressNew;
    CKey keyCollateralAddressNew;
    CPubKey pubKeyMasternodeNew;
    CKey keyMasternodeNew;
    bool fNewSigs = false;
    {
        fNewSigs = NetworkUpgradeActive(chainActive.Height() - 20, Params().GetConsensus(), Consensus::UPGRADE_MORAG);
    }
    // need correct blocks to send ping
    if (!fOffline && !masternodeSync.IsBlockchainSynced()) {
        strErrorRet = "Sync in progress. Must wait until sync is complete to start Masternode";
        LogPrint("masternode", "CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if (!CMessageSigner::GetKeysFromSecret(strKeyMasternode, keyMasternodeNew, pubKeyMasternodeNew, fNewSigs)) {
        strErrorRet = strprintf("Invalid masternode key %s", strKeyMasternode);
        LogPrint("masternode", "CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if (!pwalletMain->GetMasternodeVinAndKeys(txin, pubKeyCollateralAddressNew, keyCollateralAddressNew, strTxHash, strOutputIndex)) {
        strErrorRet = strprintf("Could not allocate txin %s:%s for masternode %s", strTxHash, strOutputIndex, strService);
        LogPrint("masternode", "CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    int nPort;
    int nDefaultPort = Params().GetDefaultPort();
    std::string strHost;
    SplitHostPort(strService, nPort, strHost);
    if (nPort == 0)
        nPort = nDefaultPort;
    CService _service(LookupNumeric(strHost.c_str(), nPort));

    // The service needs the correct default port to work properly
    if (!CheckDefaultPort(_service, strErrorRet, "CMasternodeBroadcast::Create"))
        return false;

    return Create(txin, _service, keyCollateralAddressNew, pubKeyCollateralAddressNew, keyMasternodeNew, pubKeyMasternodeNew, strErrorRet, mnbRet);
}

bool CMasternodeBroadcast::CheckDefaultPort(CService service, std::string& strErrorRet, std::string strContext)
{
    int nDefaultPort = Params().GetDefaultPort();

    if (service.GetPort() != nDefaultPort) {
        strErrorRet = strprintf("Invalid port %u for masternode %s, only %d is supported on %s-net.",
                                service.GetPort(), service.ToString(), nDefaultPort, Params().NetworkIDString());
        LogPrint("masternode", "%s - %s\n", strContext, strErrorRet);
        return false;
    }

    return true;
}

bool CMasternodeBroadcast::Create(CTxIn txin, CService service, CKey keyCollateralAddressNew, CPubKey pubKeyCollateralAddressNew, CKey keyMasternodeNew, CPubKey pubKeyMasternodeNew, std::string& strErrorRet, CMasternodeBroadcast& mnbRet)
{
    // wait for reindex and/or import to finish
    if (fImporting || fReindex)
        return false;

    bool fNewSigs = false;
    {
        fNewSigs = NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_MORAG);
    }
    KeyIO keyIO(Params());
    LogPrint("masternode", "CMasternodeBroadcast::Create -- pubKeyCollateralAddressNew = %s, pubKeyMasternodeNew.GetID() = %s\n",
             keyIO.EncodeDestination(pubKeyCollateralAddressNew.GetID()),
             pubKeyMasternodeNew.GetID().ToString());

    CMasternodePing mnp(txin);
    if (!mnp.Sign(keyMasternodeNew, pubKeyMasternodeNew, fNewSigs)) {
        strErrorRet = strprintf("Failed to sign ping, masternode=%s", txin.prevout.hash.ToString());
        LogPrint("masternode", "CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CMasternodeBroadcast();
        return false;
    }

    mnbRet = CMasternodeBroadcast(service, txin, pubKeyCollateralAddressNew, pubKeyMasternodeNew, PROTOCOL_VERSION);

    if (!mnbRet.Sign(keyCollateralAddressNew, pubKeyCollateralAddressNew, fNewSigs)) {
        strErrorRet = strprintf("Invalid IP address %s, masternode=%s", mnbRet.addr.ToStringIP(), txin.prevout.hash.ToString());
        LogPrint("masternode", "CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CMasternodeBroadcast();
        return false;
    }

    mnbRet.lastPing = mnp;
    if (!mnbRet.Sign(keyCollateralAddressNew, pubKeyCollateralAddressNew, fNewSigs)) {
        strErrorRet = strprintf("Failed to sign broadcast, masternode=%s", txin.prevout.hash.ToString());
        LogPrint("masternode", "CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CMasternodeBroadcast();
        return false;
    }

    return true;
}


bool CMasternodeBroadcast::Sign(const CKey& key, const CPubKey& pubKey, const bool fNewSigs)
{
    std::string strError = "";
    std::string strMessage;

    if (fNewSigs) {
        nMessVersion = MessageVersion::MESS_VER_HASH;
        strMessage = GetSignatureHash().GetHex();
    } else {
        nMessVersion = MessageVersion::MESS_VER_STRMESS;
        strMessage = GetStrMessage();
    }

    if (!CMessageSigner::SignMessage(strMessage, vchSig, key)) {
        return error("%s : SignMessage() (nMessVersion=%d) failed", __func__, nMessVersion);
    }

    if (!CMessageSigner::VerifyMessage(pubKey, vchSig, strMessage, strError)) {
        return error("%s : VerifyMessage() (nMessVersion=%d) failed, error: %s\n",
                     __func__, nMessVersion, strError);
    }

    return true;
}

bool CMasternodeBroadcast::Sign(const std::string strSignKey, const bool fNewSigs)
{
    CKey key;
    CPubKey pubkey;

    if (!CMessageSigner::GetKeysFromSecret(strSignKey, key, pubkey, fNewSigs)) {
        return error("%s : Invalid strSignKey", __func__);
    }

    return Sign(key, pubkey, fNewSigs);
}

bool CMasternodeBroadcast::CheckSignature() const
{
    std::string strError = "";
    std::string strMessage = (nMessVersion == MessageVersion::MESS_VER_HASH ?
                                  GetSignatureHash().GetHex() :
                                  GetStrMessage());

    if (!CMessageSigner::VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, strError))
        return error("%s : VerifyMessage (nMessVersion=%d) failed: %s", __func__, nMessVersion, strError);

    return true;
}
bool CMasternodeBroadcast::CheckAndUpdate(int& nDos)
{
    // make sure signature isn't in the future (past is OK)
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrint("masternode", "mnb - Signature rejected, too far into the future %s\n", vin.prevout.hash.ToString());
        nDos = 30;
        return false;
    }

    if (protocolVersion < masternodePayments.GetMinMasternodePaymentsProto()) {
        LogPrint("masternode", "mnb - ignoring outdated Masternode %s protocol version %d\n", vin.prevout.hash.ToString(), protocolVersion);
        return false;
    }


    std::string strError = "";
    if (!CheckSignature()) {
        // don't ban for old masternodes, their sigs could be broken because of the bug
        nDos = 100;
        return error("CMasternodeBroadcast::CheckAndUpdate - Got bad Masternode address signature : %s", __func__);
    }

    if (NetworkIdFromCommandLine() == CBaseChainParams::MAIN) {
        if (addr.GetPort() != 16113)
            return false;
    } else if (addr.GetPort() == 16113)
        return false;

    // search existing Masternode list, this is where we update existing Masternodes with new mnb broadcasts
    CMasternode* pmn = mnodeman.Find(vin);

    // no such masternode, nothing to update
    if (pmn == NULL)
        return true;

    // incorrect ping or its sigTime
    if (Params().GetConsensus().NetworkUpgradeActive(chainActive.Height() + 1, Consensus::UPGRADE_MORAG)) {
        if ((pmn->lastPing.IsNull() || !pmn->lastPing.CheckAndUpdate(nDos, false, true)) && pmn->IsEnabled()) {
            return false;
        }
    }

    // this broadcast is older or equal than the one that we already have - it's bad and should never happen
    // unless someone is doing something fishy
    // (mapSeenMasternodeBroadcast in CMasternodeMan::ProcessMessage should filter legit duplicates)
    if (pmn->sigTime >= sigTime) {
        return error("CMasternodeBroadcast::CheckAndUpdate - Bad sigTime %d for Masternode %20s %105s (existing broadcast is at %d)",
                     sigTime, addr.ToString(), vin.ToString(), pmn->sigTime);
    }

    // masternode is not enabled yet/already, nothing to update
    if (!pmn->IsEnabled() && !pmn->IsUnlocking() && !pmn->IsReEnabled())
        return true;

    // mn.pubkey = pubkey, IsVinAssociatedWithPubkey is validated once below,
    //   after that they just need to match
    if (pmn->pubKeyCollateralAddress == pubKeyCollateralAddress && !pmn->IsBroadcastedWithin(MASTERNODE_MIN_MNB_SECONDS)) {
        // take the newest entry
        LogPrint("masternode", "mnb - Got updated entry for %s\n", vin.prevout.hash.ToString());
        if (pmn->UpdateFromNewBroadcast((*this))) {
            pmn->Check();
            if (pmn->IsEnabled() || pmn->IsUnlocking() || pmn->IsReEnabled())
                Relay();
        }
        masternodeSync.AddedMasternodeList(GetHash());
    }

    return true;
}

bool CMasternodeBroadcast::CheckInputsAndAdd(int& nDoS)
{
    // we are a masternode with the same vin (i.e. already activated) and this mnb is ours (matches our Masternode privkey)
    // so nothing to do here for us
    if (fMasterNode && vin.prevout == activeMasternode.vin.prevout && pubKeyMasternode == activeMasternode.pubKeyMasternode)
        return true;

    // search existing Masternode list
    CMasternode* pmn = mnodeman.Find(vin);

    if (pmn != NULL) {
        // incorrect ping or its sigTime
        if ((pmn->lastPing.IsNull() || !pmn->lastPing.CheckAndUpdate(nDoS, false, true)) && pmn->IsEnabled())
            return false;

        // nothing to do here if we already know about this masternode and it's enabled
        if (pmn->IsEnabled() || pmn->IsUnlocking() || pmn->IsReEnabled())
            return true;
        // if it's not enabled, remove old MN first and continue
        else
            mnodeman.Remove(pmn->vin);
    }

    CValidationState state;
    CMutableTransaction tx = CMutableTransaction();
    CScript dummyScript;
    dummyScript << ToByteVector(pubKeyCollateralAddress) << OP_CHECKSIG;
    CTxOut vout = CTxOut(Params().GetMasternodeCollateral(chainActive.Height() + 1) * COIN - (CAmount)(0.01 * COIN), dummyScript);
    if (NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_MORAG)) {
        CAmount testingAmount = Params().GetMasternodeCollateral(chainActive.Height() + 1) * COIN - (CAmount)(0.01 * COIN);
        vout = CTxOut(testingAmount, dummyScript);
    }
    tx.vin.push_back(vin);
    tx.vout.push_back(vout);

    int nChainHeight = 0;
    {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain) {
            LogPrint("masternode", "lockMain\n");
            // not mnb fault, let it to be checked again later
            mnodeman.mapSeenMasternodeBroadcast.erase(GetHash());
            masternodeSync.mapSeenSyncMNB.erase(GetHash());
            return false;
        }

        CCoins coins;
        if (!pcoinsTip->GetCoins(vin.prevout.hash, coins) ||
            (unsigned int)vin.prevout.n >= coins.vout.size() ||
            coins.vout[vin.prevout.n].IsNull()) {
            LogPrint("masternode", "CMasternodeBroadcast::CheckInputsAndAdd -- Failed to find Masternode UTXO, masternode=%s\n", vin.prevout.ToStringShort());
            return false;
        }

        nChainHeight = chainActive.Height();
    }

    LogPrint("masternode", "mnb - Accepted Masternode entry\n");

    if (GetInputAge(vin) < MASTERNODE_MIN_CONFIRMATIONS) {
        LogPrint("masternode", "mnb - Input must have at least %d confirmations\n", MASTERNODE_MIN_CONFIRMATIONS);
        // maybe we miss few blocks, let this mnb to be checked again later
        mnodeman.mapSeenMasternodeBroadcast.erase(GetHash());
        masternodeSync.mapSeenSyncMNB.erase(GetHash());
        return false;
    }

    // verify that sig time is legit in past
    // should be at least not earlier than block when 10000 GLINK tx got MASTERNODE_MIN_CONFIRMATIONS
    uint256 hashBlock = uint256();
    CTransaction tx2;
    GetTransaction(vin.prevout.hash, tx2, Params().GetConsensus(), hashBlock, true);
    BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi != mapBlockIndex.end() && (*mi).second) {
        CBlockIndex* pMNIndex = (*mi).second;                                                        // block for 1000 SnowGem tx -> 1 confirmation
        CBlockIndex* pConfIndex = chainActive[pMNIndex->nHeight + MASTERNODE_MIN_CONFIRMATIONS - 1]; // block where tx got MASTERNODE_MIN_CONFIRMATIONS
        if (pConfIndex->GetBlockTime() > sigTime) {
            LogPrint("masternode", "mnb - Bad sigTime %d for Masternode %s (%i conf block is at %d)\n",
                     sigTime, vin.prevout.hash.ToString(), MASTERNODE_MIN_CONFIRMATIONS, pConfIndex->GetBlockTime());
            return false;
        }
    }

    LogPrint("masternode", "mnb - Got NEW Masternode entry - %s - %lli \n", vin.prevout.hash.ToString(), sigTime);
    CMasternode mn(*this);
    mnodeman.Add(mn);

    // if it matches our Masternode privkey, then we've been remotely activated
    if (pubKeyMasternode == activeMasternode.pubKeyMasternode && protocolVersion == PROTOCOL_VERSION) {
        activeMasternode.EnableHotColdMasterNode(vin, addr);
    }

    bool isLocal = addr.IsRFC1918() || addr.IsLocal();
    if (NetworkIdFromCommandLine() == CBaseChainParams::REGTEST)
        isLocal = false;

    if (!isLocal)
        Relay();

    return true;
}

void CMasternodeBroadcast::Relay()
{
    CInv inv(MSG_MASTERNODE_ANNOUNCE, GetHash());
    RelayInv(inv);
}

uint256 CMasternodeBroadcast::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << sigTime;
    ss << pubKeyCollateralAddress;
    return ss.GetHash();
}

CMasternodePing::CMasternodePing() : CSignedMessage(),
                                     vin(),
                                     blockHash(),
                                     sigTime(GetAdjustedTime())
{
    bool fNewSigs = false;
    {
        // LOCK(cs_main);
        fNewSigs = NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_MORAG);
    }
    if (fNewSigs) {
        nMessVersion = MessageVersion::MESS_VER_HASH;
    }
}

CMasternodePing::CMasternodePing(CTxIn& newVin) : CSignedMessage(),
                                                  vin(newVin),
                                                  sigTime(GetAdjustedTime())
{
    int nHeight;
    {
        LOCK(cs_main);
        nHeight = chainActive.Height();
        if (nHeight > 12)
            blockHash = chainActive[nHeight - 12]->GetBlockHash();
    }
    bool fNewSigs = false;
    {
        fNewSigs = NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_MORAG);
    }
    if (fNewSigs) {
        nMessVersion = MessageVersion::MESS_VER_HASH;
    }
}

uint256 CMasternodePing::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << vin;
    if (nMessVersion == MessageVersion::MESS_VER_HASH)
        ss << blockHash;
    ss << sigTime;
    return ss.GetHash();
}

std::string CMasternodePing::GetStrMessage() const
{
    return vin.ToString() + blockHash.ToString() + std::to_string(sigTime);
}

bool CMasternodePing::Sign(CKey& key, CPubKey& pubKey, bool fNewSigs)
{
    if (!CSignedMessage::SignMessage(key, pubKey, fNewSigs)) {
        LogPrint("masternode", "CMasternodePing::Sign() - Error\n");
        return false;
    }
    return true;
}

bool CMasternodePing::CheckSignature() const
{
    std::string strError = "";
    if (!CSignedMessage::CheckSignature(strError)) {
        LogPrintf("CMasternodePaymentWinner::CheckSignature Error - %s\n", strError);
        return false;
    }
    return true;
}

bool CMasternodePing::CheckAndUpdate(int& nDos, bool fRequireEnabled, bool fCheckSigTimeOnly)
{
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrint("masternode", "CMasternodePing::CheckAndUpdate - Signature rejected, too far into the future %s\n", vin.prevout.hash.ToString());
        nDos = 30;
        return false;
    }

    if (sigTime <= GetAdjustedTime() - 60 * 60) {
        LogPrint("masternode", "CMasternodePing::CheckAndUpdate - Signature rejected, too far into the past %s - %d %d \n", vin.prevout.hash.ToString(), sigTime, GetAdjustedTime());
        nDos = 30;
        return false;
    }

    // see if we have this Masternode
    CMasternode* pmn = mnodeman.Find(vin);
    const bool isMasternodeFound = (pmn != nullptr);
    const bool isSignatureValid = (isMasternodeFound && CheckSignature());

    if (fCheckSigTimeOnly) {
        if (isMasternodeFound && !isSignatureValid) {
            nDos = 33;
            return false;
        }
        return true;
    }

    LogPrint("masternode", "CMasternodePing::CheckAndUpdate - New Ping - %s - %lli\n", blockHash.ToString(), sigTime);

    // see if we have this Masternode
    if (isMasternodeFound && pmn->protocolVersion >= masternodePayments.GetMinMasternodePaymentsProto()) {
        if (fRequireEnabled && !pmn->IsAvailableState()) {
            // nDos = 20;
            return false;
        }

        // LogPrint("masternode","mnping - Found corresponding mn for vin: %s\n", vin.ToString());
        // update only if there is no known ping for this masternode or
        // last ping was more then MASTERNODE_MIN_MNP_SECONDS-60 ago comparing to this one
        if (!pmn->IsPingedWithin(MASTERNODE_MIN_MNP_SECONDS - 60, sigTime)) {
            if (!isSignatureValid) {
                LogPrint("masternode", "CMasternodePing::CheckAndUpdate - Got bad Masternode address signature %s\n", vin.prevout.hash.ToString());
                nDos = 33;
                return false;
            }

            BlockMap::iterator mi = mapBlockIndex.find(blockHash);
            if (mi == mapBlockIndex.end() || !(*mi).second) {
                LogPrint("masternode", "CMasternodePing::CheckAndUpdate - ping block not in disk. Masternode %s block hash %s\n", vin.prevout.hash.ToString(), blockHash.ToString());
                return false;
            }

            // Verify ping block hash in main chain and in the [ tip > x > tip - 24 ] range.
            {
                LOCK(cs_main);
                if (!chainActive.Contains((*mi).second) || (chainActive.Height() - (*mi).second->nHeight > 24)) {
                    LogPrint("masternode", "CMasternodePing::CheckAndUpdate - Masternode %s block hash %s is too old or has an invalid block hash\n", vin.prevout.hash.ToString(), blockHash.ToString());
                    // Do nothing here (no Masternode update, no mnping relay)
                    // Let this node to be visible but fail to accept mnping
                    return false;
                }
            }

            pmn->lastPing = *this;

            // mnodeman.mapSeenMasternodeBroadcast.lastPing is probably outdated, so we'll update it
            CMasternodeBroadcast mnb(*pmn);
            uint256 hash = mnb.GetHash();
            if (mnodeman.mapSeenMasternodeBroadcast.count(hash)) {
                mnodeman.mapSeenMasternodeBroadcast[hash].lastPing = *this;
            }

            pmn->Check(true);
            if (!pmn->IsEnabled())
                return false;

            LogPrint("masternode", "CMasternodePing::CheckAndUpdate - Masternode ping accepted, vin: %s\n", vin.prevout.hash.ToString());

            Relay();
            return true;
        }
        LogPrint("masternode", "CMasternodePing::CheckAndUpdate - Masternode ping arrived too early, vin: %s\n", vin.prevout.hash.ToString());
        // nDos = 1; //disable, this is happening frequently and causing banned peers
        return false;
    }
    LogPrint("masternode", "CMasternodePing::CheckAndUpdate - Couldn't find compatible Masternode entry, vin: %s\n", vin.prevout.hash.ToString());

    return false;
}

void CMasternodePing::Relay()
{
    CInv inv(MSG_MASTERNODE_PING, GetHash());
    RelayInv(inv);
}
