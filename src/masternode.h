// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2017-2018 The SnowGem developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODE_H
#define MASTERNODE_H

#include "base58.h"
#include "key.h"
#include "main.h"
#include "messagesigner.h"
#include "net.h"
#include "sync.h"
#include "timedata.h"
#include "util.h"

#define MASTERNODE_MIN_CONFIRMATIONS 15
#define MASTERNODE_MIN_MNP_SECONDS (10 * 60)
#define MASTERNODE_MIN_MNB_SECONDS (5 * 60)
#define MASTERNODE_PING_SECONDS (5 * 60)
#define MASTERNODE_EXPIRATION_SECONDS (120 * 60)
#define MASTERNODE_REMOVAL_SECONDS (130 * 60)
#define MASTERNODE_CHECK_SECONDS 5
#define MASTERNODE_MIN_MNP_SECONDS (10 * 60)

using namespace std;

class CMasternode;
class CMasternodeBroadcast;
class CMasternodePing;
extern map<int64_t, uint256> mapCacheBlockHashes;

bool GetBlockHash(uint256& hash, int nBlockHeight);


//
// The Masternode Ping Class : Contains a different serialize method for sending pings from masternodes throughout the network
//

class CMasternodePing : public CSignedMessage
{
public:
    CTxIn vin;
    uint256 blockHash;
    int64_t sigTime; // mnb message times
    // removed stop

    CMasternodePing();
    CMasternodePing(CTxIn& newVin);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(vin);
        READWRITE(blockHash);
        READWRITE(sigTime);
        READWRITE(vchSig);
        try {
            READWRITE(nMessVersion);
        } catch (...) {
            nMessVersion = MessageVersion::MESS_VER_STRMESS;
        }
    }

    uint256 GetHash() const;

    // override CSignedMessage functions
    uint256 GetSignatureHash() const override { return GetHash(); }
    std::string GetStrMessage() const override;
    const CTxIn GetVin() const override { return vin; };

    bool IsNull() const { return blockHash.IsNull() || vin.prevout.IsNull(); }
    bool CheckAndUpdate(int& nDos, bool fRequireEnabled = true, bool fCheckSigTimeOnly = false);
    void Relay();
    // TODO gemlink can remove after morag fork
    // bool VerifyMessage(CPubKey pubkey, const vector<unsigned char>& vchSig, std::string strMessage, std::string& errorMessage) const;
    bool CheckSignature() const;
    bool Sign(CKey& keyMasternode, CPubKey& pubKeyMasternode, bool fNewSigs);

    void swap(CMasternodePing& first, CMasternodePing& second) // nothrow
    {
        CSignedMessage::swap(first, second);
        // enable ADL (not necessary in our case, but good practice)
        using std::swap;

        // by swapping the members of two classes,
        // the two classes are effectively swapped
        swap(first.vin, second.vin);
        swap(first.blockHash, second.blockHash);
        swap(first.sigTime, second.sigTime);
    }

    CMasternodePing& operator=(CMasternodePing from)
    {
        swap(*this, from);
        return *this;
    }
    friend bool operator==(const CMasternodePing& a, const CMasternodePing& b)
    {
        return a.vin == b.vin && a.blockHash == b.blockHash;
    }
    friend bool operator!=(const CMasternodePing& a, const CMasternodePing& b)
    {
        return !(a == b);
    }
};

//
// The Masternode Class. For managing the Obfuscation process. It contains the input of the 1000 GLINK, signature to prove
// it's the one who own that ip address and code for calculating the payment election.
//
class CMasternode : public CSignedMessage
{
private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;
    int64_t lastTimeChecked;
    bool fCollateralSpent{false};

public:
    enum state {
        MASTERNODE_PRE_ENABLED,
        MASTERNODE_ENABLED,
        MASTERNODE_EXPIRED,
        MASTERNODE_REMOVE,
        MASTERNODE_WATCHDOG_EXPIRED,
        MASTERNODE_POSE_BAN,
        MASTERNODE_VIN_SPENT,
        MASTERNODE_POS_ERROR,
        MASTERNODE_MISSING
    };

    CTxIn vin;
    CService addr;
    CPubKey pubKeyCollateralAddress;
    CPubKey pubKeyMasternode;
    int activeState;
    int64_t sigTime; // mnb message time
    bool unitTest;
    bool allowFreeTx;
    int protocolVersion;
    int64_t nLastDsq; // the dsq count from the last dsq broadcast of this node
    int nScanningErrorCount;
    int nLastScanningErrorBlockHeight;
    CMasternodePing lastPing;

    int64_t nLastDsee;  // temporary, do not save. Remove after migration to v12
    int64_t nLastDseep; // temporary, do not save. Remove after migration to v12

    CMasternode();
    CMasternode(const CMasternode& other);

    // override CSignedMessage functions
    uint256 GetSignatureHash() const override;
    std::string GetStrMessage() const override;
    const CTxIn GetVin() const override { return vin; };
    const CPubKey GetPublicKey(std::string& strErrorRet) const override { return pubKeyCollateralAddress; }

    void swap(CMasternode& first, CMasternode& second) // nothrow
    {
        CSignedMessage::swap(first, second);
        // enable ADL (not necessary in our case, but good practice)
        using std::swap;

        // by swapping the members of two classes,
        // the two classes are effectively swapped
        swap(first.vin, second.vin);
        swap(first.addr, second.addr);
        swap(first.pubKeyCollateralAddress, second.pubKeyCollateralAddress);
        swap(first.pubKeyMasternode, second.pubKeyMasternode);
        swap(first.activeState, second.activeState);
        swap(first.sigTime, second.sigTime);
        swap(first.lastPing, second.lastPing);
        swap(first.unitTest, second.unitTest);
        swap(first.allowFreeTx, second.allowFreeTx);
        swap(first.protocolVersion, second.protocolVersion);
        swap(first.nLastDsq, second.nLastDsq);
        swap(first.nScanningErrorCount, second.nScanningErrorCount);
        swap(first.nLastScanningErrorBlockHeight, second.nLastScanningErrorBlockHeight);
    }

    CMasternode& operator=(CMasternode from)
    {
        swap(*this, from);
        return *this;
    }
    friend bool operator==(const CMasternode& a, const CMasternode& b)
    {
        return a.vin == b.vin;
    }
    friend bool operator!=(const CMasternode& a, const CMasternode& b)
    {
        return !(a.vin == b.vin);
    }

    // CALCULATE A RANK AGAINST OF GIVEN BLOCK
    arith_uint256 CalculateScore(int64_t nBlockHeight = 0) const;

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        LOCK(cs);

        READWRITE(vin);
        READWRITE(addr);
        READWRITE(pubKeyCollateralAddress);
        READWRITE(pubKeyMasternode);
        READWRITE(vchSig);
        READWRITE(sigTime);
        READWRITE(protocolVersion);
        READWRITE(activeState);
        READWRITE(lastPing);
        READWRITE(unitTest);
        READWRITE(allowFreeTx);
        READWRITE(nLastDsq);
        READWRITE(nScanningErrorCount);
        READWRITE(nLastScanningErrorBlockHeight);
    }

    int64_t SecondsSincePayment();

    bool UpdateFromNewBroadcast(CMasternodeBroadcast& mnb);

    void Check(bool forceCheck = false);

    bool IsBroadcastedWithin(int seconds)
    {
        return (GetAdjustedTime() - sigTime) < seconds;
    }

    bool IsPingedWithin(int seconds, int64_t now = -1) const
    {
        now == -1 ? now = GetAdjustedTime() : now;

        return lastPing.IsNull() ? false : now - lastPing.sigTime < seconds;
    }

    int GetExpirationTime();
    int GetRemovalTime();

    void SetSpent() { fCollateralSpent = true; }

    void Disable()
    {
        LOCK(cs);
        sigTime = 0;
        lastPing = CMasternodePing();
    }

    bool IsEnabled()
    {
        return activeState == MASTERNODE_ENABLED;
    }

    bool IsPreEnabled() const
    {
        return activeState == MASTERNODE_PRE_ENABLED;
    }

    bool IsAvailableState() const
    {
        return activeState == MASTERNODE_ENABLED || activeState == MASTERNODE_PRE_ENABLED;
    }

    bool IsExpiring() const
    {
        return activeState == MASTERNODE_EXPIRED;
    }

    std::string Status()
    {
        std::string strStatus = "ACTIVE";

        LOCK(cs);
        if (activeState == CMasternode::MASTERNODE_PRE_ENABLED)
            strStatus = "PRE_ENABLED";
        if (activeState == CMasternode::MASTERNODE_ENABLED)
            strStatus = "ENABLED";
        if (activeState == CMasternode::MASTERNODE_EXPIRED)
            strStatus = "EXPIRED";
        if (activeState == CMasternode::MASTERNODE_VIN_SPENT)
            strStatus = "VIN_SPENT";
        if (activeState == CMasternode::MASTERNODE_REMOVE)
            strStatus = "REMOVE";
        if (activeState == CMasternode::MASTERNODE_POS_ERROR)
            strStatus = "POS_ERROR";
        if (activeState == CMasternode::MASTERNODE_MISSING)
            strStatus = "MISSING";

        return strStatus;
    }

    int64_t GetLastPaid();
    bool IsValidNetAddr();
    /// Is the input associated with collateral public key? (and there is 10000 PIV - checking if valid masternode)
    bool IsInputAssociatedWithPubkey() const;
};


//
// The Masternode Broadcast Class : Contains a different serialize method for sending masternodes through the network
//

class CMasternodeBroadcast : public CMasternode
{
public:
    CMasternodeBroadcast();
    CMasternodeBroadcast(CService newAddr, CTxIn newVin, CPubKey newPubkey, CPubKey newPubkey2, int protocolVersionIn);
    CMasternodeBroadcast(const CMasternode& mn);

    bool CheckAndUpdate(int& nDoS);
    bool CheckInputsAndAdd(int& nDos);
    uint256 GetHash() const;
    void Relay();
    bool Sign(const CKey& key, const CPubKey& pubKey, const bool fNewSigs);
    bool Sign(const std::string strSignKey, const bool fNewSigs);
    bool CheckSignature() const;

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(vin);
        READWRITE(addr);
        READWRITE(pubKeyCollateralAddress);
        READWRITE(pubKeyMasternode);
        READWRITE(vchSig);
        READWRITE(sigTime);
        READWRITE(protocolVersion);
        READWRITE(lastPing);

        if (protocolVersion >= Params().GetConsensus().vUpgrades[Consensus::UPGRADE_MORAG].nProtocolVersion) {
            READWRITE(nMessVersion); // abuse nLastDsq (which will be removed) for old serialization
            if (ser_action.ForRead())
                nLastDsq = 0;
        } else {
            try {
                READWRITE(nLastDsq);
            } catch (...) {
                nLastDsq = 0;
            }
            if (ser_action.ForRead())
                nMessVersion = MessageVersion::MESS_VER_STRMESS;
        }
    }

    uint256 GetHash()
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << sigTime;
        ss << pubKeyCollateralAddress;
        return ss.GetHash();
    }

    /// Create Masternode broadcast, needs to be relayed manually after that
    static bool Create(CTxIn vin, CService service, CKey keyCollateralAddressNew, CPubKey pubKeyCollateralAddressNew, CKey keyMasternodeNew, CPubKey pubKeyMasternodeNew, std::string& strErrorRet, CMasternodeBroadcast& mnbRet);
    static bool Create(std::string strService, std::string strKey, std::string strTxHash, std::string strOutputIndex, std::string& strErrorRet, CMasternodeBroadcast& mnbRet, bool fOffline = false);
    static bool CheckDefaultPort(CService service, std::string& strErrorRet, std::string strContext);
};

#endif
