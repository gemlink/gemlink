// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "primitives/transaction.h"

#include "hash.h"
#include "tinyformat.h"
#include "utilstrencodings.h"

#include "librustzcash.h"

#include "key_io.h"

std::string COutPoint::ToString() const
{
    return strprintf("COutPoint(%s, %u)", hash.ToString().substr(0, 10), n);
}

std::string COutPoint::ToStringShort() const
{
    return strprintf("%s-%u", hash.ToString().substr(0, 64), n);
}

uint256 COutPoint::GetHash() const
{
    return Hash(BEGIN(hash), END(hash), BEGIN(n), END(n));
}

std::string SaplingOutPoint::ToString() const
{
    return strprintf("SaplingOutPoint(%s, %u)", hash.ToString().substr(0, 10), n);
}

CTxIn::CTxIn(COutPoint prevoutIn, CScript scriptSigIn, uint32_t nSequenceIn)
{
    prevout = prevoutIn;
    scriptSig = scriptSigIn;
    nSequence = nSequenceIn;
}

CTxIn::CTxIn(uint256 hashPrevTx, uint32_t nOut, CScript scriptSigIn, uint32_t nSequenceIn)
{
    prevout = COutPoint(hashPrevTx, nOut);
    scriptSig = scriptSigIn;
    nSequence = nSequenceIn;
}

std::string CTxIn::ToString() const
{
    std::string str;
    str += "CTxIn(";
    str += prevout.ToString();
    if (prevout.IsNull())
        str += strprintf(", coinbase %s", HexStr(scriptSig));
    else
        str += strprintf(", scriptSig=%s", HexStr(scriptSig).substr(0, 24));
    if (nSequence != std::numeric_limits<unsigned int>::max())
        str += strprintf(", nSequence=%u", nSequence);
    str += ")";
    return str;
}

CTxOut::CTxOut(const CAmount& nValueIn, CScript scriptPubKeyIn)
{
    nValue = nValueIn;
    scriptPubKey = scriptPubKeyIn;
    nRounds = -10;
}

uint256 CTxOut::GetHash() const
{
    return SerializeHash(*this);
}

std::string CTxOut::ToString() const
{
    CTxDestination address1;
    ExtractDestination(scriptPubKey, address1);
    KeyIO keyIO(Params());
    return strprintf("CTxOut(nValue=%d.%08d, scriptPubKey=%s, address=%s)", nValue / COIN, nValue % COIN, HexStr(scriptPubKey).substr(0, 30), keyIO.EncodeDestination(address1));
}

CMutableTransaction::CMutableTransaction() : fOverwintered(false), nVersion(CTransaction::SPROUT_MIN_CURRENT_VERSION), nVersionGroupId(0), nLockTime(0), nExpiryHeight(0), valueBalance(0) {}
CMutableTransaction::CMutableTransaction(const CTransaction& tx) : fOverwintered(tx.fOverwintered), nVersion(tx.nVersion), nVersionGroupId(tx.nVersionGroupId),
                                                                   vin(tx.vin), vout(tx.vout),
                                                                   nLockTime(tx.nLockTime), nExpiryHeight(tx.nExpiryHeight),
                                                                   valueBalance(tx.valueBalance), vShieldedSpend(tx.vShieldedSpend), vShieldedOutput(tx.vShieldedOutput),
                                                                   vjoinsplit(tx.vjoinsplit), joinSplitPubKey(tx.joinSplitPubKey), joinSplitSig(tx.joinSplitSig),
                                                                   bindingSig(tx.bindingSig)
{
}

uint256 CMutableTransaction::GetHash() const
{
    return SerializeHash(*this);
}

void CTransaction::UpdateHash() const
{
    *const_cast<uint256*>(&hash) = SerializeHash(*this);
}

CTransaction::CTransaction() : fOverwintered(false), nVersion(CTransaction::SPROUT_MIN_CURRENT_VERSION), nVersionGroupId(0), vin(), vout(), nLockTime(0), nExpiryHeight(0), valueBalance(0), vShieldedSpend(), vShieldedOutput(), vjoinsplit(), joinSplitPubKey(), joinSplitSig(), bindingSig() {}

CTransaction::CTransaction(const CMutableTransaction& tx) : fOverwintered(tx.fOverwintered), nVersion(tx.nVersion), nVersionGroupId(tx.nVersionGroupId),
                                                            vin(tx.vin), vout(tx.vout), nLockTime(tx.nLockTime), nExpiryHeight(tx.nExpiryHeight),
                                                            valueBalance(tx.valueBalance), vShieldedSpend(tx.vShieldedSpend), vShieldedOutput(tx.vShieldedOutput),
                                                            vjoinsplit(tx.vjoinsplit), joinSplitPubKey(tx.joinSplitPubKey), joinSplitSig(tx.joinSplitSig),
                                                            bindingSig(tx.bindingSig)
{
    UpdateHash();
}

// Protected constructor which only derived classes can call.
// For developer testing only.
CTransaction::CTransaction(
    const CMutableTransaction& tx,
    bool evilDeveloperFlag) : fOverwintered(tx.fOverwintered), nVersion(tx.nVersion), nVersionGroupId(tx.nVersionGroupId),
                              vin(tx.vin), vout(tx.vout), nLockTime(tx.nLockTime), nExpiryHeight(tx.nExpiryHeight),
                              valueBalance(tx.valueBalance), vShieldedSpend(tx.vShieldedSpend), vShieldedOutput(tx.vShieldedOutput),
                              vjoinsplit(tx.vjoinsplit), joinSplitPubKey(tx.joinSplitPubKey), joinSplitSig(tx.joinSplitSig),
                              bindingSig(tx.bindingSig)
{
    assert(evilDeveloperFlag);
}

CTransaction::CTransaction(CMutableTransaction&& tx) : fOverwintered(tx.fOverwintered), nVersion(tx.nVersion), nVersionGroupId(tx.nVersionGroupId),
                                                       vin(std::move(tx.vin)), vout(std::move(tx.vout)), nLockTime(tx.nLockTime), nExpiryHeight(tx.nExpiryHeight),
                                                       valueBalance(tx.valueBalance),
                                                       vShieldedSpend(std::move(tx.vShieldedSpend)), vShieldedOutput(std::move(tx.vShieldedOutput)),
                                                       vjoinsplit(std::move(tx.vjoinsplit)),
                                                       joinSplitPubKey(std::move(tx.joinSplitPubKey)), joinSplitSig(std::move(tx.joinSplitSig))
{
    UpdateHash();
}

CTransaction& CTransaction::operator=(const CTransaction& tx)
{
    *const_cast<bool*>(&fOverwintered) = tx.fOverwintered;
    *const_cast<int*>(&nVersion) = tx.nVersion;
    *const_cast<uint32_t*>(&nVersionGroupId) = tx.nVersionGroupId;
    *const_cast<std::vector<CTxIn>*>(&vin) = tx.vin;
    *const_cast<std::vector<CTxOut>*>(&vout) = tx.vout;
    *const_cast<unsigned int*>(&nLockTime) = tx.nLockTime;
    *const_cast<uint32_t*>(&nExpiryHeight) = tx.nExpiryHeight;
    *const_cast<CAmount*>(&valueBalance) = tx.valueBalance;
    *const_cast<std::vector<SpendDescription>*>(&vShieldedSpend) = tx.vShieldedSpend;
    *const_cast<std::vector<OutputDescription>*>(&vShieldedOutput) = tx.vShieldedOutput;
    *const_cast<std::vector<JSDescription>*>(&vjoinsplit) = tx.vjoinsplit;
    *const_cast<Ed25519VerificationKey*>(&joinSplitPubKey) = tx.joinSplitPubKey;
    *const_cast<Ed25519Signature*>(&joinSplitSig) = tx.joinSplitSig;
    *const_cast<binding_sig_t*>(&bindingSig) = tx.bindingSig;
    *const_cast<uint256*>(&hash) = tx.hash;
    return *this;
}

CAmount CTransaction::GetValueOut() const
{
    CAmount nValueOut = 0;
    for (std::vector<CTxOut>::const_iterator it(vout.begin()); it != vout.end(); ++it) {
        nValueOut += it->nValue;
        if (!MoneyRange(it->nValue) || !MoneyRange(nValueOut))
            throw std::runtime_error("CTransaction::GetValueOut(): value out of range");
    }

    if (valueBalance <= 0) {
        // NB: negative valueBalance "takes" money from the transparent value pool just as outputs do
        nValueOut += -valueBalance;

        if (!MoneyRange(-valueBalance) || !MoneyRange(nValueOut)) {
            throw std::runtime_error("CTransaction::GetValueOut(): value out of range");
        }
    }

    for (std::vector<JSDescription>::const_iterator it(vjoinsplit.begin()); it != vjoinsplit.end(); ++it) {
        // NB: vpub_old "takes" money from the transparent value pool just as outputs do
        nValueOut += it->vpub_old;

        if (!MoneyRange(it->vpub_old) || !MoneyRange(nValueOut))
            throw std::runtime_error("CTransaction::GetValueOut(): value out of range");
    }
    return nValueOut;
}

CAmount CTransaction::GetShieldedValueIn() const
{
    CAmount nValue = 0;

    if (valueBalance >= 0) {
        // NB: positive valueBalance "gives" money to the transparent value pool just as inputs do
        nValue += valueBalance;

        if (!MoneyRange(valueBalance) || !MoneyRange(nValue)) {
            throw std::runtime_error("CTransaction::GetShieldedValueIn(): value out of range");
        }
    }

    for (std::vector<JSDescription>::const_iterator it(vjoinsplit.begin()); it != vjoinsplit.end(); ++it) {
        // NB: vpub_new "gives" money to the transparent value pool just as inputs do
        nValue += it->vpub_new;

        if (!MoneyRange(it->vpub_new) || !MoneyRange(nValue))
            throw std::runtime_error("CTransaction::GetShieldedValueIn(): value out of range");
    }

    return nValue;
}

double CTransaction::ComputePriority(double dPriorityInputs, unsigned int nTxSize) const
{
    nTxSize = CalculateModifiedSize(nTxSize);
    if (nTxSize == 0)
        return 0.0;

    return dPriorityInputs / nTxSize;
}

unsigned int CTransaction::CalculateModifiedSize(unsigned int nTxSize) const
{
    // In order to avoid disincentivizing cleaning up the UTXO set we don't count
    // the constant overhead for each txin and up to 110 bytes of scriptSig (which
    // is enough to cover a compressed pubkey p2sh redemption) for priority.
    // Providing any more cleanup incentive than making additional inputs free would
    // risk encouraging people to create junk outputs to redeem later.
    if (nTxSize == 0)
        nTxSize = ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION);
    for (std::vector<CTxIn>::const_iterator it(vin.begin()); it != vin.end(); ++it) {
        unsigned int offset = 41U + std::min(110U, (unsigned int)it->scriptSig.size());
        if (nTxSize > offset)
            nTxSize -= offset;
    }
    return nTxSize;
}

std::string CTransaction::ToString() const
{
    std::string str;
    if (!fOverwintered) {
        str += strprintf("CTransaction(hash=%s, ver=%d, vin.size=%u, vout.size=%u, nLockTime=%u)\n",
                         GetHash().ToString().substr(0, 10),
                         nVersion,
                         vin.size(),
                         vout.size(),
                         nLockTime);
    } else if (nVersion >= SAPLING_MIN_TX_VERSION) {
        str += strprintf("CTransaction(hash=%s, ver=%d, fOverwintered=%d, nVersionGroupId=%08x, vin.size=%u, vout.size=%u, nLockTime=%u, nExpiryHeight=%u, valueBalance=%u, vShieldedSpend.size=%u, vShieldedOutput.size=%u)\n",
                         GetHash().ToString().substr(0, 10),
                         nVersion,
                         fOverwintered,
                         nVersionGroupId,
                         vin.size(),
                         vout.size(),
                         nLockTime,
                         nExpiryHeight,
                         valueBalance,
                         vShieldedSpend.size(),
                         vShieldedOutput.size());
    } else if (nVersion >= 3) {
        str += strprintf("CTransaction(hash=%s, ver=%d, fOverwintered=%d, nVersionGroupId=%08x, vin.size=%u, vout.size=%u, nLockTime=%u, nExpiryHeight=%u)\n",
                         GetHash().ToString().substr(0, 10),
                         nVersion,
                         fOverwintered,
                         nVersionGroupId,
                         vin.size(),
                         vout.size(),
                         nLockTime,
                         nExpiryHeight);
    }
    for (unsigned int i = 0; i < vin.size(); i++)
        str += "    " + vin[i].ToString() + "\n";
    for (unsigned int i = 0; i < vout.size(); i++)
        str += "    " + vout[i].ToString() + "\n";
    return str;
}
