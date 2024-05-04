// Copyright (c) 2014-2016 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2017-2018 The SnowGem developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemasternode.h"
#include "addrman.h"
#include "masternode-sync.h"
#include "masternode.h"
#include "masternodeconfig.h"
#include "masternodeman.h"
#include "messagesigner.h"
#include "protocol.h"
#include "spork.h"

//
// Bootup the Masternode, look for a 1000 SnowGem input and register on the network
//
void CActiveMasternode::ManageStatus()
{
    std::string errorMessage;

    if (!fMasterNode)
        return;

    if (fDebug)
        LogPrintf("CActiveMasternode::ManageStatus() - Begin\n");

    // need correct blocks to send ping
    if (NetworkIdFromCommandLine() != CBaseChainParams::REGTEST && !masternodeSync.IsBlockchainSynced()) {
        status = ACTIVE_MASTERNODE_SYNC_IN_PROCESS;
        LogPrintf("CActiveMasternode::ManageStatus() - %d\n", GetStatus());
        return;
    }

    if (status == ACTIVE_MASTERNODE_SYNC_IN_PROCESS)
        status = ACTIVE_MASTERNODE_INITIAL;

    if (status == ACTIVE_MASTERNODE_INITIAL) {
        CMasternode* pmn;
        pmn = mnodeman.Find(pubKeyMasternode);
        if (pmn != NULL) {
            pmn->Check();
            if (pmn->IsEnabled() && pmn->protocolVersion == PROTOCOL_VERSION)
                EnableHotColdMasterNode(pmn->vin, pmn->addr);
        }
    }

    if (status != ACTIVE_MASTERNODE_STARTED) {
        // Set defaults
        status = ACTIVE_MASTERNODE_NOT_CAPABLE;
        notCapableReason = "";

        if (pwalletMain->IsLocked()) {
            notCapableReason = "Wallet is locked.";
            LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason);
            return;
        }

        // if (pwalletMain->GetBalance() == 0) {
        //     notCapableReason = "Hot node, waiting for remote activation.";
        //     LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason);
        //     return;
        // }

        if (strMasterNodeAddr.empty()) {
            if (!GetLocal(service)) {
                notCapableReason = "Can't detect external address. Please use the masternodeaddr configuration option.";
                LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason);
                return;
            }
        } else {
            int nPort;
            std::string strHost;
            SplitHostPort(strMasterNodeAddr, nPort, strHost);
            service = LookupNumeric(strHost.c_str(), nPort);
        }

        LogPrintf("CActiveMasternode::ManageStatus() - Checking inbound connection to '%s'\n", service.ToString());

        // The service needs the correct default port to work properly
        if (!CMasternodeBroadcast::CheckDefaultPort(service, errorMessage, "CActiveMasternode::ManageStatus()"))
            return;
        CAddress addr(service, NODE_NETWORK);
        if (!OpenNetworkConnection(addr, true, nullptr, service.ToString().c_str())) {
            notCapableReason = "Could not connect to " + service.ToString();
            LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason);
            return;
        }

        notCapableReason = "Waiting for start message from controller.";
        return;
    }

    // send to all peers
    if (!SendMasternodePing(errorMessage)) {
        LogPrintf("CActiveMasternode::ManageStatus() - Error on Ping: %s\n", errorMessage);
    }
}

void CActiveMasternode::ResetStatus()
{
    status = ACTIVE_MASTERNODE_INITIAL;
    ManageStatus();
}

std::string CActiveMasternode::GetStatusMessage() const
{
    switch (status) {
    case ACTIVE_MASTERNODE_INITIAL:
        return "Node just started, not yet activated";
    case ACTIVE_MASTERNODE_SYNC_IN_PROCESS:
        return "Sync in progress. Must wait until sync is complete to start Masternode";
    case ACTIVE_MASTERNODE_NOT_CAPABLE:
        return "Not capable masternode: " + notCapableReason;
    case ACTIVE_MASTERNODE_STARTED:
        return "Masternode successfully started";
    default:
        return "unknown";
    }
}

bool CActiveMasternode::SendMasternodePing(std::string& errorMessage)
{
    if (status != ACTIVE_MASTERNODE_STARTED) {
        errorMessage = "Masternode is not in a running status";
        return false;
    }

    CPubKey pubKeyMasternode;
    CKey keyMasternode;

    bool fNewSigs = false;
    {
        fNewSigs = Params().GetConsensus().NetworkUpgradeActive(chainActive.Height() + 1, Consensus::UPGRADE_MORAG);
    }
    if (!CMessageSigner::GetKeysFromSecret(strMasterNodePrivKey, keyMasternode, pubKeyMasternode, fNewSigs)) {
        errorMessage = "Error upon calling GetKeysFromSecret.\n";
        return false;
    }

    LogPrint("masternode", "CActiveMasternode::SendMasternodePing() - Relay Masternode Ping vin = %s\n", vin.ToString());


    CMasternodePing mnp(vin);
    if (!mnp.Sign(keyMasternode, pubKeyMasternode, fNewSigs)) {
        errorMessage = "Couldn't sign Masternode Ping";
        return false;
    }

    // Update lastPing for our masternode in Masternode list
    CMasternode* pmn = mnodeman.Find(vin);
    if (pmn != NULL) {
        if (pmn->IsPingedWithin(MASTERNODE_PING_SECONDS, mnp.sigTime)) {
            errorMessage = "Too early to send Masternode Ping";
            return false;
        }

        pmn->lastPing = mnp;
        mnodeman.mapSeenMasternodePing.insert(make_pair(mnp.GetHash(), mnp));

        // mnodeman.mapSeenMasternodeBroadcast.lastPing is probably outdated, so we'll update it
        CMasternodeBroadcast mnb(*pmn);
        uint256 hash = mnb.GetHash();
        if (mnodeman.mapSeenMasternodeBroadcast.count(hash))
            mnodeman.mapSeenMasternodeBroadcast[hash].lastPing = mnp;

        mnp.Relay();

        /*
         * IT'S SAFE TO REMOVE THIS IN FURTHER VERSIONS
         * AFTER MIGRATION TO V12 IS DONE
         */

        if (sporkManager.IsSporkActive(SPORK_10_MASTERNODE_PAY_UPDATED_NODES))
            return true;
        // for migration purposes ping our node on old masternodes network too
        std::string retErrorMessage;
        std::vector<unsigned char> vchMasterNodeSignature;
        int64_t masterNodeSignatureTime = GetAdjustedTime();

        std::string strMessage = service.ToString() + std::to_string(masterNodeSignatureTime) + std::to_string(false);

        if (!CMessageSigner::SignMessage(strMessage, vchMasterNodeSignature, keyMasternode)) {
            errorMessage = "dseep sign message failed.";
            return false;
        }

        if (!CMessageSigner::VerifyMessage(pubKeyMasternode, vchMasterNodeSignature, strMessage, retErrorMessage)) {
            errorMessage = "dseep verify message failed: " + retErrorMessage;
            return false;
        }

        LogPrint("masternode", "dseep - relaying from active mn, %s \n", vin.ToString().c_str());
        LOCK(cs_vNodes);
        for (CNode* pnode : vNodes)
            pnode->PushMessage("dseep", vin, vchMasterNodeSignature, masterNodeSignatureTime, false);

        /*
         * END OF "REMOVE"
         */
        return true;
    } else {
        // Seems like we are trying to send a ping while the Masternode is not registered in the network
        errorMessage = "Obfuscation Masternode List doesn't include our Masternode, shutting down Masternode pinging service! " + vin.ToString();
        status = ACTIVE_MASTERNODE_NOT_CAPABLE;
        notCapableReason = errorMessage;
        return false;
    }
}

// when starting a Masternode, this can enable to run as a hot wallet with no funds
bool CActiveMasternode::EnableHotColdMasterNode(CTxIn& newVin, CService& newService)
{
    if (!fMasterNode)
        return false;

    status = ACTIVE_MASTERNODE_STARTED;

    // The values below are needed for signing mnping messages going forward
    vin = newVin;
    service = newService;

    LogPrintf("CActiveMasternode::EnableHotColdMasterNode() - Enabled! You may shut down the cold daemon.\n");

    return true;
}