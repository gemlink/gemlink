// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "clientversion.h"
#include "experimental_features.h"
#include "init.h"
#include "key_io.h"
#include "main.h"
#include "masternode-sync.h"
#include "metrics.h"
#include "net.h"
#include "netbase.h"
#include "rpc/server.h"
#include "spork.h"
#include "timedata.h"
#include "txdb.h"
#include "txmempool.h"
#include "util.h"
#include "utilmoneystr.h"
// #ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#include "wallet/walletdb.h"
// #endif

#include <stdint.h>

#include <boost/assign/list_of.hpp>

#include <univalue.h>

#include "zcash/Address.hpp"

using namespace std;

/**
 *Return current blockchain status, wallet balance, address balance and the last 200 transactions
 **/
UniValue getalldata(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "getalldata \"datatype transactiontype \"\n"
            "\nArguments:\n"
            "1. \"datatype\"     (integer, required) \n"
            "                    Value of 0: Return address, balance, transactions and blockchain info\n"
            "                    Value of 1: Return address, balance, blockchain info\n"
            "                    Value of 2: Return transactions and blockchain info\n"
            "2. \"transactiontype\"     (integer, optional) \n"
            "                    Value of 1: Return all transactions in the last 24 hours\n"
            "                    Value of 2: Return all transactions in the last 7 days\n"
            "                    Value of 3: Return all transactions in the last 30 days\n"
            "                    Other number: Return all transactions in the last 24 hours\n"
            "\nResult:\n"
            "\nExamples:\n" +
            HelpExampleCli("getalldata", "0") + HelpExampleRpc("getalldata", "0"));

    LOCK(cs_main);

    UniValue returnObj(UniValue::VOBJ);
    int connectionCount = 0;
    {
        LOCK2(cs_main, cs_vNodes);
        connectionCount = (int)vNodes.size();
    }

#ifdef ENABLE_WALLET
    vector<COutput> vecOutputs;
    CAmount remainingValue = 0;
    bool fProtectCoinbase = Params().GetCoinbaseProtected(chainActive.Height() + 1);
    pwalletMain->AvailableCoins(vecOutputs, true, NULL, false, !fProtectCoinbase, true);

    // Find unspent coinbase utxos and update estimated size
    BOOST_FOREACH (const COutput& out, vecOutputs) {
        if (!out.fSpendable) {
            continue;
        }

        CTxDestination address;
        if (!ExtractDestination(out.tx->vout[out.i].scriptPubKey, address)) {
            continue;
        }

        if (!out.tx->IsCoinBase()) {
            continue;
        }

        CAmount nValue = out.tx->vout[out.i].nValue;

        remainingValue += nValue;
    }
#endif

    int nMinDepth = 1;
#ifdef ENABLE_WALLET
    CAmount nBalance = getBalanceTaddr("", nMinDepth, true);
    CAmount nPrivateBalance = getBalanceZaddr("", nMinDepth, INT_MAX, true);
    CAmount nLockedCoin = pwalletMain->GetLockedCoins();
    CAmount nTotalBalance = nBalance + nPrivateBalance + nLockedCoin;
#endif

    returnObj.push_back(Pair("connectionCount", connectionCount));
    returnObj.push_back(Pair("besttime", chainActive.Tip()->GetBlockTime()));
    returnObj.push_back(Pair("blocks", (int)chainActive.Height()));
    returnObj.push_back(Pair("bestblockhash", chainActive.Tip()->GetBlockHash().GetHex()));
#ifdef ENABLE_WALLET
    returnObj.push_back(Pair("transparentbalance", ValueFromAmount(nBalance)));
    returnObj.push_back(Pair("privatebalance", ValueFromAmount(nPrivateBalance)));
    returnObj.push_back(Pair("lockedbalance", ValueFromAmount(nLockedCoin)));
    returnObj.push_back(Pair("totalbalance", ValueFromAmount(nTotalBalance)));
    returnObj.push_back(Pair("remainingValue", ValueFromAmount(remainingValue)));
    returnObj.push_back(Pair("unconfirmedbalance", ValueFromAmount(pwalletMain->GetUnconfirmedBalance())));
    returnObj.push_back(Pair("immaturebalance", ValueFromAmount(pwalletMain->GetImmatureBalance())));


    // get address balance
    nBalance = 0;
    // get all t address
    UniValue addressbalance(UniValue::VARR);
    UniValue addrlist(UniValue::VOBJ);
    KeyIO keyIO(Params());
    if (params.size() > 0 && (params[0].get_int() == 1 || params[0].get_int() == 0)) {
        vector<COutput> vecOutputs;

        {
            LOCK2(cs_main, pwalletMain->cs_wallet);

            pwalletMain->AvailableCoins(vecOutputs, false, NULL, true);
        }

        BOOST_FOREACH (const PAIRTYPE(CTxDestination, CAddressBookData) & item, pwalletMain->mapAddressBook) {
            UniValue addr(UniValue::VOBJ);
            const CTxDestination& dest = item.first;

            isminetype mine = pwalletMain ? IsMine(*pwalletMain, dest) : ISMINE_NO;

            // const string& strName = item.second.name;
            nBalance = 0;
            for (const COutput& out : vecOutputs) {
                if (out.nDepth < nMinDepth) {
                    continue;
                }

                CScript scriptPubKey = GetScriptForDestination(dest);

                if (out.tx->vout[out.i].scriptPubKey != scriptPubKey) {
                    continue;
                }

                CAmount nValue = out.tx->vout[out.i].nValue;
                nBalance += nValue;
            }

            addr.push_back(Pair("amount", ValueFromAmount(nBalance)));
            addr.push_back(Pair("ismine", (mine & ISMINE_SPENDABLE) ? true : false));
            addrlist.push_back(Pair(keyIO.EncodeDestination(dest), addr));
        }

        // address grouping
        {
            LOCK2(cs_main, pwalletMain->cs_wallet);

            map<CTxDestination, CAmount> balances = pwalletMain->GetAddressBalances();
            BOOST_FOREACH (set<CTxDestination> grouping, pwalletMain->GetAddressGroupings()) {
                BOOST_FOREACH (CTxDestination address, grouping) {
                    UniValue addr(UniValue::VOBJ);
                    const string& strName = keyIO.EncodeDestination(address);
                    if (addrlist.exists(strName))
                        continue;

                    isminetype mine = pwalletMain ? IsMine(*pwalletMain, address) : ISMINE_NO;

                    nBalance = 0;
                    for (const COutput& out : vecOutputs) {
                        if (out.nDepth < nMinDepth) {
                            continue;
                        }

                        CTxDestination taddr = keyIO.DecodeDestination(strName);

                        CScript scriptPubKey = GetScriptForDestination(taddr);

                        if (out.tx->vout[out.i].scriptPubKey != scriptPubKey) {
                            continue;
                        }

                        CAmount nValue = out.tx->vout[out.i].nValue;
                        nBalance += nValue;
                    }

                    addr.push_back(Pair("amount", ValueFromAmount(nBalance)));
                    addr.push_back(Pair("ismine", (mine & ISMINE_SPENDABLE) ? true : false));
                    addrlist.push_back(Pair(strName, addr));
                }
            }
        }


        // get all z address
        // {
        //     std::set<libzcash::SproutPaymentAddress> addresses;
        //     pwalletMain->GetSproutPaymentAddresses(addresses);
        //     for (auto addr : addresses) {
        //         if (pwalletMain->HaveSproutSpendingKey(addr)) {
        //             UniValue address(UniValue::VOBJ);
        //             const string& strName = EncodePaymentAddress(addr);
        //             nBalance = getBalanceZaddr(strName, nMinDepth, false);
        //             address.push_back(Pair("amount", ValueFromAmount(nBalance)));
        //             address.push_back(Pair("ismine", true));
        //             addrlist.push_back(Pair(strName, address));
        //         } else {
        //             UniValue address(UniValue::VOBJ);
        //             const string& strName = EncodePaymentAddress(addr);
        //             nBalance = getBalanceZaddr(strName, nMinDepth, false);
        //             address.push_back(Pair("amount", ValueFromAmount(nBalance)));
        //             address.push_back(Pair("ismine", false));
        //             addrlist.push_back(Pair(strName, address));
        //         }
        //     }
        // }
        {
            std::set<libzcash::SaplingPaymentAddress> addresses;
            pwalletMain->GetSaplingPaymentAddresses(addresses);
            for (auto addr : addresses) {
                const string& strName = keyIO.EncodePaymentAddress(addr);
                UniValue address(UniValue::VOBJ);
                nBalance = getBalanceZaddr(strName, nMinDepth, INT_MAX, false);
                address.push_back(Pair("amount", ValueFromAmount(nBalance)));
                if (pwalletMain->HaveSaplingSpendingKeyForAddress(addr)) {
                    address.push_back(Pair("ismine", true));
                } else {
                    address.push_back(Pair("ismine", false));
                }
                addrlist.push_back(Pair(strName, address));
            }
        }
    }

    addressbalance.push_back(addrlist);
    // returnObj.push_back(Pair("addressbalance", addressbalance));
    returnObj.push_back(Pair("addressbalancev2", addrlist));


    // get transactions
    string strAccount = "";
    int nCount = 200;
    // int nFrom = 0;
    isminefilter filter = ISMINE_SPENDABLE;

    UniValue trans(UniValue::VARR);
    UniValue transTime(UniValue::VARR);
    if (params.size() > 0 && (params[0].get_int() == 2 || params[0].get_int() == 0)) {
        int day = 1;
        if (params.size() > 1) {
            if (params[1].get_int() == 1) {
                day = 1;
            } else if (params[1].get_int() == 2) {
                day = 7;
            } else if (params[1].get_int() == 3) {
                day = 30;
            } else if (params[1].get_int() == 4) {
                day = 90;
            } else if (params[1].get_int() == 5) {
                day = 365;
            }
        }

        std::list<CAccountingEntry> acentries;
        CWallet::TxItems txOrdered = pwalletMain->OrderedTxItems(acentries, strAccount);
        uint64_t t = GetTime();
        // iterate backwards until we have nCount items to return:
        for (CWallet::TxItems::reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it) {
            CWalletTx* const pwtx = (*it).second.first;
            if (pwtx != 0)
                ListTransactions(*pwtx, strAccount, 0, true, trans, filter);
            CAccountingEntry* const pacentry = (*it).second.second;
            if (pacentry != 0)
                AcentryToJSON(*pacentry, strAccount, trans);
            int confirms = pwtx->GetDepthInMainChain();
            if (confirms > 0) {
                if (mapBlockIndex[pwtx->hashBlock]->GetBlockTime() <= (t - (day * 60 * 60 * 24)) && (int)trans.size() >= nCount)
                    break;
            }
        }

        vector<UniValue> arrTmp = trans.getValues();

        std::reverse(arrTmp.begin(), arrTmp.end()); // Return oldest to newest

        trans.clear();
        trans.setArray();
        trans.push_backV(arrTmp);
    }

    returnObj.push_back(Pair("listtransactions", trans));

    if (params.size() > 0 && (params[0].get_int() == 1 || params[0].get_int() == 0)) {
        if (masternodeSync.IsMasternodeListSynced() && NetworkUpgradeActive(chainActive.Height() + 1, Params().GetConsensus(), Consensus::UPGRADE_XANDAR)) {
            vector<COutput> vCoins;
            pwalletMain->MasternodeCoins(vCoins);

            if (vCoins.size() > 0) {
                UniValue mnList(UniValue::VARR);

                auto upgradeMorag = Params().GetConsensus().vUpgrades[Consensus::UPGRADE_MORAG];

                for (COutput v : vCoins) {
                    int lastHeight = 2167201;
                    COutPoint prevout(v.tx->GetHash(), v.i);
                    CTxIn vin(prevout);
                    GetLastPaymentBlock(vin, lastHeight);

                    CTxDestination address1;
                    ExtractDestination(v.tx->vout[v.i].scriptPubKey, address1);

                    UniValue mn(UniValue::VOBJ);
                    if (lastHeight < chainActive.Height() + 1 - Params().GetmnLockBlocks(chainActive.Height())) {
                        lastHeight = 0;
                    }
                    mn.push_back(Pair("lastpayment", lastHeight));
                    mn.push_back(Pair("unlocked", lastHeight > 0 ? lastHeight + Params().GetmnLockBlocks(chainActive.Height()) : 0));
                    mn.push_back(Pair("address", keyIO.EncodeDestination(address1)));
                    mn.push_back(Pair("hash", v.tx->GetHash().ToString()));
                    mn.push_back(Pair("amount", Params().GetMasternodeCollateral(upgradeMorag.nActivationHeight)));
                    mn.push_back(Pair("idx", v.i));
                    mnList.push_back(mn);
                }
                returnObj.push_back(Pair("lockedtxs", mnList));
            }
        }
    }
    returnObj.push_back(Pair("isencrypted", pwalletMain->IsCrypted()));
    returnObj.push_back(Pair("islocked", pwalletMain->IsLocked()));

#endif

    return returnObj;
}

/**
 * @note Do not add or change anything in the information returned by this
 * method. `getinfo` exists for backwards-compatibility only. It combines
 * information from wildly different sources in the program, which is a mess,
 * and is thus planned to be deprecated eventually.
 *
 * Based on the source of the information, new information should be added to:
 * - `getblockchaininfo`,
 * - `getnetworkinfo` or
 * - `getwalletinfo`
 *
 * Or alternatively, create a specific query method for the information.
 **/
UniValue getinfo(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getinfo\n"
            "Returns an object containing various state info.\n"
            "\nResult:\n"
            "{\n"
            "  \"version\": xxxxx,           (numeric) the server version\n"
            "  \"protocolversion\": xxxxx,   (numeric) the protocol version\n"
            "  \"walletversion\": xxxxx,     (numeric) the wallet version\n"
            "  \"balance\": xxxxxxx,         (numeric) the total Gemlink balance of the wallet\n"
            "  \"blocks\": xxxxxx,           (numeric) the current number of blocks processed in the server\n"
            "  \"timeoffset\": xxxxx,        (numeric) the time offset\n"
            "  \"connections\": xxxxx,       (numeric) the number of connections\n"
            "  \"proxy\": \"host:port\",     (string, optional) the proxy used by the server\n"
            "  \"difficulty\": xxxxxx,       (numeric) the current difficulty\n"
            "  \"testnet\": true|false,      (boolean) if the server is using testnet or not\n"
            "  \"keypoololdest\": xxxxxx,    (numeric) the timestamp (seconds since GMT epoch) of the oldest pre-generated key in the key pool\n"
            "  \"keypoolsize\": xxxx,        (numeric) how many new keys are pre-generated\n"
            "  \"unlocked_until\": ttt,      (numeric) the timestamp in seconds since epoch (midnight Jan 1 1970 GMT) that the wallet is unlocked for transfers, or 0 if the wallet is locked\n"
            "  \"paytxfee\": x.xxxx,         (numeric) the transaction fee set in " +
            CURRENCY_UNIT + "/kB\n"
                            "  \"relayfee\": x.xxxx,         (numeric) minimum relay fee for non-free transactions in " +
            CURRENCY_UNIT + "/kB\n"
                            "  \"errors\": \"...\"           (string) any error messages\n"
                            "}\n"
                            "\nExamples:\n" +
            HelpExampleCli("getinfo", "") + HelpExampleRpc("getinfo", ""));

#ifdef ENABLE_WALLET
    LOCK2(cs_main, pwalletMain ? &pwalletMain->cs_wallet : NULL);
#else
    LOCK(cs_main);
#endif

    proxyType proxy;
    GetProxy(NET_IPV4, proxy);

    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("version", CLIENT_VERSION));
    obj.push_back(Pair("buildinfo", FormatFullVersion()));
    obj.push_back(Pair("protocolversion", PROTOCOL_VERSION));
#ifdef ENABLE_WALLET
    if (pwalletMain) {
        obj.push_back(Pair("walletversion", pwalletMain->GetVersion()));
        obj.push_back(Pair("balance", ValueFromAmount(pwalletMain->GetBalance())));
    }
#endif
    obj.push_back(Pair("blocks", (int)chainActive.Height()));
    obj.push_back(Pair("timeoffset", GetTimeOffset()));
    obj.push_back(Pair("connections", (int)vNodes.size()));
    obj.push_back(Pair("proxy", (proxy.IsValid() ? proxy.proxy.ToStringIPPort() : string())));
    obj.push_back(Pair("difficulty", (double)GetDifficulty()));
    obj.push_back(Pair("networksolps", GetNetworkHashPS(120, -1)));
    obj.push_back(Pair("testnet", Params().TestnetToBeDeprecatedFieldRPC()));
#ifdef ENABLE_WALLET
    if (pwalletMain) {
        obj.push_back(Pair("keypoololdest", pwalletMain->GetOldestKeyPoolTime()));
        obj.push_back(Pair("keypoolsize", (int)pwalletMain->GetKeyPoolSize()));
    }
    if (pwalletMain && pwalletMain->IsCrypted())
        obj.push_back(Pair("unlocked_until", nWalletUnlockTime));
    obj.push_back(Pair("paytxfee", ValueFromAmount(payTxFee.GetFeePerK())));
#endif
    obj.push_back(Pair("relayfee", ValueFromAmount(::minRelayTxFee.GetFeePerK())));
    auto warnings = GetWarnings("statusbar");
    obj.pushKV("errors", warnings.first);
    obj.pushKV("errorstimestamp", warnings.second);
    return obj;
}

UniValue mnsync(const UniValue& params, bool fHelp)
{
    std::string strMode;
    if (params.size() == 1)
        strMode = params[0].get_str();

    if (fHelp || params.size() != 1 || (strMode != "status" && strMode != "reset")) {
        throw runtime_error(
            "mnsync \"status|reset\"\n"
            "\nReturns the sync status or resets sync.\n"

            "\nArguments:\n"
            "1. \"mode\"    (string, required) either 'status' or 'reset'\n"

            "\nResult ('status' mode):\n"
            "{\n"
            "  \"IsBlockchainSynced\": true|false,    (boolean) 'true' if blockchain is synced\n"
            "  \"lastMasternodeList\": xxxx,        (numeric) Timestamp of last MN list message\n"
            "  \"lastMasternodeWinner\": xxxx,      (numeric) Timestamp of last MN winner message\n"
            "  \"lastBudgetItem\": xxxx,            (numeric) Timestamp of last MN budget message\n"
            "  \"lastFailure\": xxxx,           (numeric) Timestamp of last failed sync\n"
            "  \"nCountFailures\": n,           (numeric) Number of failed syncs (total)\n"
            "  \"sumMasternodeList\": n,        (numeric) Number of MN list messages (total)\n"
            "  \"sumMasternodeWinner\": n,      (numeric) Number of MN winner messages (total)\n"
            "  \"sumBudgetItemProp\": n,        (numeric) Number of MN budget messages (total)\n"
            "  \"sumBudgetItemFin\": n,         (numeric) Number of MN budget finalization messages (total)\n"
            "  \"countMasternodeList\": n,      (numeric) Number of MN list messages (local)\n"
            "  \"countMasternodeWinner\": n,    (numeric) Number of MN winner messages (local)\n"
            "  \"countBudgetItemProp\": n,      (numeric) Number of MN budget messages (local)\n"
            "  \"countBudgetItemFin\": n,       (numeric) Number of MN budget finalization messages (local)\n"
            "  \"RequestedMasternodeAssets\": n, (numeric) Status code of last sync phase\n"
            "  \"RequestedMasternodeAttempt\": n, (numeric) Status code of last sync attempt\n"
            "}\n"

            "\nResult ('reset' mode):\n"
            "\"status\"     (string) 'success'\n"
            "\nExamples:\n" +
            HelpExampleCli("mnsync", "\"status\"") + HelpExampleRpc("mnsync", "\"status\""));
    }

    if (strMode == "status") {
        UniValue obj(UniValue::VOBJ);

        obj.push_back(Pair("IsBlockchainSynced", masternodeSync.IsBlockchainSynced()));
        obj.push_back(Pair("lastMasternodeList", masternodeSync.lastMasternodeList));
        obj.push_back(Pair("lastMasternodeWinner", masternodeSync.lastMasternodeWinner));
        obj.push_back(Pair("lastBudgetItem", masternodeSync.lastBudgetItem));
        obj.push_back(Pair("lastFailure", masternodeSync.lastFailure));
        obj.push_back(Pair("nCountFailures", masternodeSync.nCountFailures));
        obj.push_back(Pair("sumMasternodeList", masternodeSync.sumMasternodeList));
        obj.push_back(Pair("sumMasternodeWinner", masternodeSync.sumMasternodeWinner));
        obj.push_back(Pair("sumBudgetItemProp", masternodeSync.sumBudgetItemProp));
        obj.push_back(Pair("sumBudgetItemFin", masternodeSync.sumBudgetItemFin));
        obj.push_back(Pair("countMasternodeList", masternodeSync.countMasternodeList));
        obj.push_back(Pair("countMasternodeWinner", masternodeSync.countMasternodeWinner));
        obj.push_back(Pair("countBudgetItemProp", masternodeSync.countBudgetItemProp));
        obj.push_back(Pair("countBudgetItemFin", masternodeSync.countBudgetItemFin));
        obj.push_back(Pair("RequestedMasternodeAssets", masternodeSync.RequestedMasternodeAssets));
        obj.push_back(Pair("RequestedMasternodeAttempt", masternodeSync.RequestedMasternodeAttempt));

        return obj;
    }

    if (strMode == "reset") {
        masternodeSync.Reset();
        return "success";
    }
    return "failure";
}

#ifdef ENABLE_WALLET
class DescribeAddressVisitor : public boost::static_visitor<UniValue>
{
public:
    UniValue operator()(const CNoDestination& dest) const { return UniValue(UniValue::VOBJ); }

    UniValue operator()(const CKeyID& keyID) const
    {
        UniValue obj(UniValue::VOBJ);
        CPubKey vchPubKey;
        obj.push_back(Pair("isscript", false));
        if (pwalletMain && pwalletMain->GetPubKey(keyID, vchPubKey)) {
            obj.push_back(Pair("pubkey", HexStr(vchPubKey)));
            obj.push_back(Pair("iscompressed", vchPubKey.IsCompressed()));
        }
        return obj;
    }

    UniValue operator()(const CScriptID& scriptID) const
    {
        UniValue obj(UniValue::VOBJ);
        CScript subscript;
        obj.push_back(Pair("isscript", true));
        if (pwalletMain && pwalletMain->GetCScript(scriptID, subscript)) {
            std::vector<CTxDestination> addresses;
            txnouttype whichType;
            int nRequired;
            ExtractDestinations(subscript, whichType, addresses, nRequired);
            obj.push_back(Pair("script", GetTxnOutputType(whichType)));
            obj.push_back(Pair("hex", HexStr(subscript.begin(), subscript.end())));
            UniValue a(UniValue::VARR);
            KeyIO keyIO(Params());
            for (const CTxDestination& addr : addresses) {
                a.push_back(keyIO.EncodeDestination(addr));
            }
            obj.push_back(Pair("addresses", a));
            if (whichType == TX_MULTISIG)
                obj.push_back(Pair("sigsrequired", nRequired));
        }
        return obj;
    }
};
#endif

/*
    Used for updating/reading spork settings on the network
*/
UniValue spork(const UniValue& params, bool fHelp)
{
    if (params.size() == 1 && params[0].get_str() == "show") {
        UniValue ret(UniValue::VOBJ);
        for (const auto& sporkDef : sporkDefs) {
            ret.push_back(Pair(sporkDef.name, sporkManager.GetSporkValue(sporkDef.sporkId)));
        }
        return ret;
    } else if (params.size() == 1 && params[0].get_str() == "active") {
        UniValue ret(UniValue::VOBJ);
        for (const auto& sporkDef : sporkDefs) {
            ret.push_back(Pair(sporkDef.name, sporkManager.IsSporkActive(sporkDef.sporkId)));
        }
        return ret;
    } else if (params.size() == 2) {
        SporkId nSporkID = sporkManager.GetSporkIDByName(params[0].get_str());
        if (nSporkID == SPORK_INVALID) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid spork name");
        }

        // SPORK VALUE
        int64_t nValue = params[1].get_int();

        // broadcast new spork
        if (sporkManager.UpdateSpork(nSporkID, nValue)) {
            return "success";
        } else {
            return "failure";
        }
    }

    throw runtime_error(
        "spork <name> [<value>]\n"
        "<name> is the corresponding spork name, or 'show' to show all current spork settings, active to show which sporks are active"
        "<value> is a epoch datetime to enable or disable spork" +
        HelpRequiringPassphrase());
}
UniValue validateaddress(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "validateaddress \"gemlinkaddress\"\n"
            "\nReturn information about the given Gemlink address.\n"
            "\nArguments:\n"
            "1. \"gemlinkaddress\"     (string, required) The Gemlink address to validate\n"
            "\nResult:\n"
            "{\n"
            "  \"isvalid\" : true|false,         (boolean) If the address is valid or not. If not, this is the only property returned.\n"
            "  \"address\" : \"gemlinkaddress\",   (string) The Gemlink address validated\n"
            "  \"scriptPubKey\" : \"hex\",       (string) The hex encoded scriptPubKey generated by the address\n"
            "  \"ismine\" : true|false,          (boolean) If the address is yours or not\n"
            "  \"isscript\" : true|false,        (boolean) If the key is a script\n"
            "  \"pubkey\" : \"publickeyhex\",    (string) The hex value of the raw public key\n"
            "  \"iscompressed\" : true|false,    (boolean) If the address is compressed\n"
            "  \"account\" : \"account\"         (string) DEPRECATED. The account associated with the address, \"\" is the default account\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("validateaddress", "\"1PSSGeFHDnKNxiEyFrD1wcEaHr9hrQDDWc\"") + HelpExampleRpc("validateaddress", "\"1PSSGeFHDnKNxiEyFrD1wcEaHr9hrQDDWc\""));

#ifdef ENABLE_WALLET
    LOCK2(cs_main, pwalletMain ? &pwalletMain->cs_wallet : NULL);
#else
    LOCK(cs_main);
#endif
    KeyIO keyIO(Params());
    CTxDestination dest = keyIO.DecodeDestination(params[0].get_str());
    bool isValid = IsValidDestination(dest);

    UniValue ret(UniValue::VOBJ);
    ret.push_back(Pair("isvalid", isValid));
    if (isValid) {
        std::string currentAddress = keyIO.EncodeDestination(dest);
        ret.push_back(Pair("address", currentAddress));

        CScript scriptPubKey = GetScriptForDestination(dest);
        ret.push_back(Pair("scriptPubKey", HexStr(scriptPubKey.begin(), scriptPubKey.end())));

#ifdef ENABLE_WALLET
        isminetype mine = pwalletMain ? IsMine(*pwalletMain, dest) : ISMINE_NO;
        ret.push_back(Pair("ismine", (mine & ISMINE_SPENDABLE) ? true : false));
        ret.push_back(Pair("iswatchonly", (mine & ISMINE_WATCH_ONLY) ? true : false));
        UniValue detail = std::visit(DescribeAddressVisitor(), dest);
        ret.pushKVs(detail);
        if (pwalletMain && pwalletMain->mapAddressBook.count(dest))
            ret.push_back(Pair("account", pwalletMain->mapAddressBook[dest].name));
#endif
    }
    return ret;
}


class DescribePaymentAddressVisitor
{
public:
    UniValue operator()(const libzcash::InvalidEncoding& zaddr) const { return UniValue(UniValue::VOBJ); }

    UniValue operator()(const libzcash::SproutPaymentAddress& zaddr) const
    {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("type", "sprout");
        obj.pushKV("payingkey", zaddr.a_pk.GetHex());
        obj.pushKV("transmissionkey", zaddr.pk_enc.GetHex());
#ifdef ENABLE_WALLET
        if (pwalletMain) {
            obj.pushKV("ismine", HaveSpendingKeyForPaymentAddress(pwalletMain)(zaddr));
        }
#endif
        return obj;
    }

    UniValue operator()(const libzcash::SaplingPaymentAddress& zaddr) const
    {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("type", "sapling");
        obj.pushKV("diversifier", HexStr(zaddr.d));
        obj.pushKV("diversifiedtransmissionkey", zaddr.pk_d.GetHex());
#ifdef ENABLE_WALLET
        if (pwalletMain) {
            obj.pushKV("ismine", HaveSpendingKeyForPaymentAddress(pwalletMain)(zaddr));
        }
#endif
        return obj;
    }
};

UniValue z_validateaddress(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "z_validateaddress \"zaddr\"\n"
            "\nReturn information about the given z address.\n"
            "\nArguments:\n"
            "1. \"zaddr\"     (string, required) The z address to validate\n"
            "\nResult:\n"
            "{\n"
            "  \"isvalid\" : true|false,      (boolean) If the address is valid or not. If not, this is the only property returned.\n"
            "  \"address\" : \"zaddr\",         (string) The z address validated\n"
            "  \"type\" : \"xxxx\",             (string) \"sprout\" or \"sapling\"\n"
            "  \"ismine\" : true|false,       (boolean) If the address is yours or not\n"
            "  \"payingkey\" : \"hex\",         (string) [sprout] The hex value of the paying key, a_pk\n"
            "  \"transmissionkey\" : \"hex\",   (string) [sprout] The hex value of the transmission key, pk_enc\n"
            "  \"diversifier\" : \"hex\",       (string) [sapling] The hex value of the diversifier, d\n"
            "  \"diversifiedtransmissionkey\" : \"hex\", (string) [sapling] The hex value of pk_d\n"

            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("z_validateaddress", "\"zcWsmqT4X2V4jgxbgiCzyrAfRT1vi1F4sn7M5Pkh66izzw8Uk7LBGAH3DtcSMJeUb2pi3W4SQF8LMKkU2cUuVP68yAGcomL\"") + HelpExampleRpc("z_validateaddress", "\"zcWsmqT4X2V4jgxbgiCzyrAfRT1vi1F4sn7M5Pkh66izzw8Uk7LBGAH3DtcSMJeUb2pi3W4SQF8LMKkU2cUuVP68yAGcomL\""));

    KeyIO keyIO(Params());
#ifdef ENABLE_WALLET
    LOCK2(cs_main, pwalletMain->cs_wallet);
#else
    LOCK(cs_main);
#endif

    string strAddress = params[0].get_str();
    auto address = keyIO.DecodePaymentAddress(strAddress);
    bool isValid = IsValidPaymentAddress(address);

    UniValue ret(UniValue::VOBJ);
    ret.push_back(Pair("isvalid", isValid));
    if (isValid) {
        ret.push_back(Pair("address", strAddress));
        UniValue detail = std::visit(DescribePaymentAddressVisitor(), address);
        ret.pushKVs(detail);
    }
    return ret;
}


/**
 * Used by addmultisigaddress / createmultisig:
 */
CScript _createmultisig_redeemScript(const UniValue& params)
{
    int nRequired = params[0].get_int();
    const UniValue& keys = params[1].get_array();

    // Gather public keys
    if (nRequired < 1)
        throw runtime_error("a multisignature address must require at least one key to redeem");
    if ((int)keys.size() < nRequired)
        throw runtime_error(
            strprintf("not enough keys supplied "
                      "(got %u keys, but need at least %d to redeem)",
                      keys.size(), nRequired));
    if (keys.size() > 16)
        throw runtime_error("Number of addresses involved in the multisignature address creation > 16\nReduce the number");
    std::vector<CPubKey> pubkeys;
    pubkeys.resize(keys.size());
    for (unsigned int i = 0; i < keys.size(); i++) {
        const std::string& ks = keys[i].get_str();
#ifdef ENABLE_WALLET
        // Case 1: Bitcoin address and we have full public key:
        KeyIO keyIO(Params());
        CTxDestination dest = keyIO.DecodeDestination(ks);
        if (pwalletMain && IsValidDestination(dest)) {
            const CKeyID* keyID = std::get_if<CKeyID>(&dest);
            if (!keyID) {
                throw std::runtime_error(strprintf("%s does not refer to a key", ks));
            }
            CPubKey vchPubKey;
            if (!pwalletMain->GetPubKey(*keyID, vchPubKey)) {
                throw std::runtime_error(strprintf("no full public key for address %s", ks));
            }
            if (!vchPubKey.IsFullyValid())
                throw runtime_error(" Invalid public key: " + ks);
            pubkeys[i] = vchPubKey;
        }

        // Case 2: hex public key
        else
#endif
            if (IsHex(ks)) {
            CPubKey vchPubKey(ParseHex(ks));
            if (!vchPubKey.IsFullyValid())
                throw runtime_error(" Invalid public key: " + ks);
            pubkeys[i] = vchPubKey;
        } else {
            throw runtime_error(" Invalid public key: " + ks);
        }
    }
    CScript result = GetScriptForMultisig(nRequired, pubkeys);

    if (result.size() > MAX_SCRIPT_ELEMENT_SIZE)
        throw runtime_error(
            strprintf("redeemScript exceeds size limit: %d > %d", result.size(), MAX_SCRIPT_ELEMENT_SIZE));

    return result;
}

UniValue createmultisig(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 2) {
        string msg = "createmultisig nrequired [\"key\",...]\n"
                     "\nCreates a multi-signature address with n signature of m keys required.\n"
                     "It returns a json object with the address and redeemScript.\n"

                     "\nArguments:\n"
                     "1. nrequired      (numeric, required) The number of required signatures out of the n keys or addresses.\n"
                     "2. \"keys\"       (string, required) A json array of keys which are Gemlink addresses or hex-encoded public keys\n"
                     "     [\n"
                     "       \"key\"    (string) Gemlink address or hex-encoded public key\n"
                     "       ,...\n"
                     "     ]\n"

                     "\nResult:\n"
                     "{\n"
                     "  \"address\":\"multisigaddress\",  (string) The value of the new multisig address.\n"
                     "  \"redeemScript\":\"script\"       (string) The string value of the hex-encoded redemption script.\n"
                     "}\n"

                     "\nExamples:\n"
                     "\nCreate a multisig address from 2 addresses\n" +
                     HelpExampleCli("createmultisig", "2 \"[\\\"t16sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\",\\\"t171sgjn4YtPu27adkKGrdDwzRTxnRkBfKV\\\"]\"") +
                     "\nAs a json rpc call\n" + HelpExampleRpc("createmultisig", "2, \"[\\\"t16sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\",\\\"t171sgjn4YtPu27adkKGrdDwzRTxnRkBfKV\\\"]\"");
        throw runtime_error(msg);
    }

    // Construct using pay-to-script-hash:
    CScript inner = _createmultisig_redeemScript(params);
    CScriptID innerID(inner);

    UniValue result(UniValue::VOBJ);
    KeyIO keyIO(Params());
    result.push_back(Pair("address", keyIO.EncodeDestination(innerID)));
    result.push_back(Pair("redeemScript", HexStr(inner.begin(), inner.end())));

    return result;
}

UniValue verifymessage(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 3)
        throw runtime_error(
            "verifymessage \"gemlinkaddress\" \"signature\" \"message\"\n"
            "\nVerify a signed message\n"
            "\nArguments:\n"
            "1. \"gemlinkaddress\"    (string, required) The Gemlink address to use for the signature.\n"
            "2. \"signature\"       (string, required) The signature provided by the signer in base 64 encoding (see signmessage).\n"
            "3. \"message\"         (string, required) The message that was signed.\n"
            "\nResult:\n"
            "true|false   (boolean) If the signature is verified or not.\n"
            "\nExamples:\n"
            "\nUnlock the wallet for 30 seconds\n" +
            HelpExampleCli("walletpassphrase", "\"mypassphrase\" 30") +
            "\nCreate the signature\n" + HelpExampleCli("signmessage", "\"t14oHp2v54vfmdgQ3v3SNuQga8JKHTNi2a1\" \"my message\"") +
            "\nVerify the signature\n" + HelpExampleCli("verifymessage", "\"t14oHp2v54vfmdgQ3v3SNuQga8JKHTNi2a1\" \"signature\" \"my message\"") +
            "\nAs json rpc\n" + HelpExampleRpc("verifymessage", "\"t14oHp2v54vfmdgQ3v3SNuQga8JKHTNi2a1\", \"signature\", \"my message\""));

    LOCK(cs_main);

    string strAddress = params[0].get_str();
    string strSign = params[1].get_str();
    string strMessage = params[2].get_str();

    KeyIO keyIO(Params());
    CTxDestination destination = keyIO.DecodeDestination(strAddress);
    if (!IsValidDestination(destination)) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");
    }

    const CKeyID* keyID = std::get_if<CKeyID>(&destination);
    if (!keyID) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");
    }

    bool fInvalid = false;
    vector<unsigned char> vchSig = DecodeBase64(strSign.c_str(), &fInvalid);

    if (fInvalid)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Malformed base64 encoding");

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    CPubKey pubkey;
    if (!pubkey.RecoverCompact(ss.GetHash(), vchSig))
        return false;

    return (pubkey.GetID() == *keyID);
}

UniValue setmocktime(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "setmocktime timestamp\n"
            "\nSet the local time to given timestamp (-regtest only)\n"
            "\nArguments:\n"
            "1. timestamp  (integer, required) Unix seconds-since-epoch timestamp\n"
            "   Pass 0 to go back to using the system time.");

    if (!Params().MineBlocksOnDemand())
        throw runtime_error("setmocktime for regression testing (-regtest mode) only");

    // cs_vNodes is locked and node send/receive times are updated
    // atomically with the time change to prevent peers from being
    // disconnected because we think we haven't communicated with them
    // in a long time.
    LOCK2(cs_main, cs_vNodes);

    RPCTypeCheck(params, boost::assign::list_of(UniValue::VNUM));
    SetMockTime(params[0].get_int64());

    uint64_t t = GetTime();
    BOOST_FOREACH (CNode* pnode, vNodes) {
        pnode->nLastSend = pnode->nLastRecv = t;
    }

    return NullUniValue;
}

bool getAddressFromIndex(const int& type, const uint160& hash, std::string& address)
{
    KeyIO keyIO(Params());
    if (type == 2) {
        address = keyIO.EncodeDestination(CScriptID(hash));
    } else if (type == 1) {
        address = keyIO.EncodeDestination(CKeyID(hash));
    } else {
        return false;
    }
    return true;
}

// This function accepts an address and returns in the output parameters
// the version and raw bytes for the RIPEMD-160 hash.
bool getIndexKey(
    const CTxDestination& dest,
    uint160& hashBytes,
    int& type)
{
    if (!IsValidDestination(dest)) {
        return false;
    }
    if (IsKeyDestination(dest)) {
        auto x = std::get_if<CKeyID>(&dest);
        memcpy(&hashBytes, x->begin(), 20);
        type = CScript::P2PKH;
        return true;
    }
    if (IsScriptDestination(dest)) {
        auto x = std::get_if<CScriptID>(&dest);
        memcpy(&hashBytes, x->begin(), 20);
        type = CScript::P2SH;
        return true;
    }
    return false;
}

bool getAddressesFromParams(const UniValue& params, std::vector<std::pair<uint160, int>>& addresses)
{
    KeyIO keyIO(Params());
    if (params[0].isStr()) {
        CTxDestination address = keyIO.DecodeDestination(params[0].get_str());
        uint160 hashBytes;
        int type = 0;
        if (!getIndexKey(address, hashBytes, type)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
        }
        addresses.push_back(std::make_pair(hashBytes, type));
    } else if (params[0].isObject()) {
        UniValue addressValues = find_value(params[0].get_obj(), "addresses");
        if (!addressValues.isArray()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Addresses is expected to be an array");
        }

        std::vector<UniValue> values = addressValues.getValues();

        for (std::vector<UniValue>::iterator it = values.begin(); it != values.end(); ++it) {
            CTxDestination address = keyIO.DecodeDestination(it->get_str());
            uint160 hashBytes;
            int type = 0;
            if (!getIndexKey(address, hashBytes, type)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
            }
            addresses.push_back(std::make_pair(hashBytes, type));
        }
    } else {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    return true;
}

bool heightSort(std::pair<CAddressUnspentKey, CAddressUnspentValue> a,
                std::pair<CAddressUnspentKey, CAddressUnspentValue> b)
{
    return a.second.blockHeight < b.second.blockHeight;
}

bool timestampSort(std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta> a,
                   std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta> b)
{
    return a.second.time < b.second.time;
}
UniValue getaddressmempool(const UniValue& params, bool fHelp)
{
    std::string disabledMsg = "";
    if (!(fExperimentalInsightExplorer || fExperimentalLightWalletd)) {
        disabledMsg = experimentalDisabledHelpMsg("getaddressmempool", {"insightexplorer", "lightwalletd"});
    }
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getaddressmempool {\"addresses\": [\"taddr\", ...]}\n"
            "\nReturns all mempool deltas for an address.\n" +
            disabledMsg +
            "\nArguments:\n"
            "{\n"
            "  \"addresses\":\n"
            "    [\n"
            "      \"address\"  (string) The base58check encoded address\n"
            "      ,...\n"
            "    ]\n"
            "}\n"
            "(or)\n"
            "\"address\"  (string) The base58check encoded address\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"address\"  (string) The base58check encoded address\n"
            "    \"txid\"  (string) The related txid\n"
            "    \"index\"  (number) The related input or output index\n"
            "    \"satoshis\"  (number) The difference of zatoshis\n"
            "    \"timestamp\"  (number) The time the transaction entered the mempool (seconds)\n"
            "    \"prevtxid\"  (string) The previous txid (if spending)\n"
            "    \"prevout\"  (string) The previous transaction output index (if spending)\n"
            "  }\n"
            "]\n"
            "\nExamples:\n" +
            HelpExampleCli("getaddressmempool", "'{\"addresses\": [\"tmYXBYJj1K7vhejSec5osXK2QsGa5MTisUQ\"]}'") + HelpExampleRpc("getaddressmempool", "{\"addresses\": [\"tmYXBYJj1K7vhejSec5osXK2QsGa5MTisUQ\"]}"));

    if (!(fExperimentalInsightExplorer || fExperimentalLightWalletd)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Error: getaddressmempool is disabled. "
                                           "Run './zcash-cli help getaddressmempool' for instructions on how to enable this feature.");
    }

    std::vector<std::pair<uint160, int>> addresses;

    if (!getAddressesFromParams(params, addresses)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }
    std::vector<std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta>> indexes;
    mempool.getAddressIndex(addresses, indexes);
    std::sort(indexes.begin(), indexes.end(),
              [](const std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta>& a,
                 const std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta>& b) -> bool {
                  return a.second.time < b.second.time;
              });

    UniValue result(UniValue::VARR);

    for (const auto& it : indexes) {
        std::string address;
        if (!getAddressFromIndex(it.first.type, it.first.addressBytes, address)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unknown address type");
        }
        UniValue delta(UniValue::VOBJ);
        delta.pushKV("address", address);
        delta.pushKV("txid", it.first.txhash.GetHex());
        delta.pushKV("index", (int)it.first.index);
        delta.pushKV("satoshis", it.second.amount);
        delta.pushKV("timestamp", it.second.time);
        if (it.second.amount < 0) {
            delta.pushKV("prevtxid", it.second.prevhash.GetHex());
            delta.pushKV("prevout", (int)it.second.prevout);
        }
        result.push_back(delta);
    }
    return result;
}


// insightexplorer
UniValue getaddressutxos(const UniValue& params, bool fHelp)
{
    std::string disabledMsg = "";
    if (!(fExperimentalInsightExplorer || fExperimentalLightWalletd)) {
        disabledMsg = experimentalDisabledHelpMsg("getaddressutxos", {"insightexplorer", "lightwalletd"});
    }
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getaddressutxos {\"addresses\": [\"taddr\", ...], (\"chainInfo\": true|false)}\n"
            "\nReturns all unspent outputs for an address.\n" +
            disabledMsg +
            "\nArguments:\n"
            "{\n"
            "  \"addresses\":\n"
            "    [\n"
            "      \"address\"  (string) The base58check encoded address\n"
            "      ,...\n"
            "    ],\n"
            "  \"chainInfo\"  (boolean, optional, default=false) Include chain info with results\n"
            "}\n"
            "(or)\n"
            "\"address\"  (string) The base58check encoded address\n"
            "\nResult\n"
            "[\n"
            "  {\n"
            "    \"address\"  (string) The address base58check encoded\n"
            "    \"txid\"  (string) The output txid\n"
            "    \"height\"  (number) The block height\n"
            "    \"outputIndex\"  (number) The output index\n"
            "    \"script\"  (string) The script hex encoded\n"
            "    \"satoshis\"  (number) The number of zatoshis of the output\n"
            "  }, ...\n"
            "]\n\n"
            "(or, if chainInfo is true):\n\n"
            "{\n"
            "  \"utxos\":\n"
            "    [\n"
            "      {\n"
            "        \"address\"     (string)  The address base58check encoded\n"
            "        \"txid\"        (string)  The output txid\n"
            "        \"height\"      (number)  The block height\n"
            "        \"outputIndex\" (number)  The output index\n"
            "        \"script\"      (string)  The script hex encoded\n"
            "        \"satoshis\"    (number)  The number of zatoshis of the output\n"
            "      }, ...\n"
            "    ],\n"
            "  \"hash\"              (string)  The block hash\n"
            "  \"height\"            (numeric) The block height\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("getaddressutxos", "'{\"addresses\": [\"tmYXBYJj1K7vhejSec5osXK2QsGa5MTisUQ\"], \"chainInfo\": true}'") + HelpExampleRpc("getaddressutxos", "{\"addresses\": [\"tmYXBYJj1K7vhejSec5osXK2QsGa5MTisUQ\"], \"chainInfo\": true}"));

    if (!(fExperimentalInsightExplorer || fExperimentalLightWalletd)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Error: getaddressutxos is disabled. "
                                           "Run './zcash-cli help getaddressutxos' for instructions on how to enable this feature.");
    }

    bool includeChainInfo = false;
    if (params[0].isObject()) {
        UniValue chainInfo = find_value(params[0].get_obj(), "chainInfo");
        if (!chainInfo.isNull()) {
            includeChainInfo = chainInfo.get_bool();
        }
    }
    std::vector<std::pair<uint160, int>> addresses;
    if (!getAddressesFromParams(params, addresses)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }
    std::vector<CAddressUnspentDbEntry> unspentOutputs;
    for (const auto& it : addresses) {
        if (!GetAddressUnspent(it.first, it.second, unspentOutputs)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
        }
    }
    std::sort(unspentOutputs.begin(), unspentOutputs.end(),
              [](const CAddressUnspentDbEntry& a, const CAddressUnspentDbEntry& b) -> bool {
                  return a.second.blockHeight < b.second.blockHeight;
              });

    UniValue utxos(UniValue::VARR);
    for (const auto& it : unspentOutputs) {
        UniValue output(UniValue::VOBJ);
        std::string address;
        if (!getAddressFromIndex(it.first.type, it.first.hashBytes, address)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unknown address type");
        }

        output.pushKV("address", address);
        output.pushKV("txid", it.first.txhash.GetHex());
        output.pushKV("outputIndex", (int)it.first.index);
        output.pushKV("script", HexStr(it.second.script.begin(), it.second.script.end()));
        output.pushKV("satoshis", it.second.satoshis);
        output.pushKV("height", it.second.blockHeight);
        utxos.push_back(output);
    }

    if (!includeChainInfo)
        return utxos;

    UniValue result(UniValue::VOBJ);
    result.pushKV("utxos", utxos);

    LOCK(cs_main); // for chainActive
    result.pushKV("hash", chainActive.Tip()->GetBlockHash().GetHex());
    result.pushKV("height", (int)chainActive.Height());
    return result;
}


UniValue getaddressdeltas(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1 || !params[0].isObject())
        throw runtime_error(
            "getaddressdeltas\n"
            "\nReturns all changes for an address (requires addressindex to be enabled).\n"
            "\nArguments:\n"
            "{\n"
            "  \"addresses\"\n"
            "    [\n"
            "      \"address\"  (string) The base58check encoded address\n"
            "      ,...\n"
            "    ]\n"
            "  \"start\" (number) The start block height\n"
            "  \"end\" (number) The end block height\n"
            "  \"chainInfo\" (boolean) Include chain info in results, only applies if start and end specified\n"
            "}\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"satoshis\"  (number) The difference of satoshis\n"
            "    \"txid\"  (string) The related txid\n"
            "    \"index\"  (number) The related input or output index\n"
            "    \"height\"  (number) The block height\n"
            "    \"address\"  (string) The base58check encoded address\n"
            "  }\n"
            "]\n"
            "\nExamples:\n" +
            HelpExampleCli("getaddressdeltas", "'{\"addresses\": [\"12c6DSiU4Rq3P4ZxziKxzrL5LmMBrzjrJX\"]}'") + HelpExampleRpc("getaddressdeltas", "{\"addresses\": [\"12c6DSiU4Rq3P4ZxziKxzrL5LmMBrzjrJX\"]}"));


    UniValue startValue = find_value(params[0].get_obj(), "start");
    UniValue endValue = find_value(params[0].get_obj(), "end");

    UniValue chainInfo = find_value(params[0].get_obj(), "chainInfo");
    bool includeChainInfo = false;
    if (chainInfo.isBool()) {
        includeChainInfo = chainInfo.get_bool();
    }

    int start = 0;
    int end = 0;

    if (startValue.isNum() && endValue.isNum()) {
        start = startValue.get_int();
        end = endValue.get_int();
        if (start <= 0 || end <= 0) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Start and end is expected to be greater than zero");
        }
        if (end < start) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "End value is expected to be greater than start");
        }
    }

    std::vector<std::pair<uint160, int>> addresses;

    if (!getAddressesFromParams(params, addresses)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    std::vector<std::pair<CAddressIndexKey, CAmount>> addressIndex;

    for (std::vector<std::pair<uint160, int>>::iterator it = addresses.begin(); it != addresses.end(); it++) {
        if (start > 0 && end > 0) {
            if (!GetAddressIndex((*it).first, (*it).second, addressIndex, start, end)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
            }
        } else {
            if (!GetAddressIndex((*it).first, (*it).second, addressIndex)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
            }
        }
    }

    UniValue deltas(UniValue::VARR);

    for (std::vector<std::pair<CAddressIndexKey, CAmount>>::const_iterator it = addressIndex.begin(); it != addressIndex.end(); it++) {
        std::string address;
        if (!getAddressFromIndex(it->first.type, it->first.hashBytes, address)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unknown address type");
        }

        UniValue delta(UniValue::VOBJ);
        delta.push_back(Pair("satoshis", it->second));
        delta.push_back(Pair("txid", it->first.txhash.GetHex()));
        delta.push_back(Pair("index", (int)it->first.index));
        delta.push_back(Pair("blockindex", (int)it->first.txindex));
        delta.push_back(Pair("height", it->first.blockHeight));
        delta.push_back(Pair("address", address));
        deltas.push_back(delta);
    }

    UniValue result(UniValue::VOBJ);

    if (includeChainInfo && start > 0 && end > 0) {
        LOCK(cs_main);

        if (start > chainActive.Height() || end > chainActive.Height()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Start or end is outside chain range");
        }

        CBlockIndex* startIndex = chainActive[start];
        CBlockIndex* endIndex = chainActive[end];

        UniValue startInfo(UniValue::VOBJ);
        UniValue endInfo(UniValue::VOBJ);

        startInfo.push_back(Pair("hash", startIndex->GetBlockHash().GetHex()));
        startInfo.push_back(Pair("height", start));

        endInfo.push_back(Pair("hash", endIndex->GetBlockHash().GetHex()));
        endInfo.push_back(Pair("height", end));

        result.push_back(Pair("deltas", deltas));
        result.push_back(Pair("start", startInfo));
        result.push_back(Pair("end", endInfo));

        return result;
    } else {
        return deltas;
    }
}

UniValue getaddressbalance(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getaddressbalance\n"
            "\nReturns the balance for an address(es) (requires addressindex to be enabled).\n"
            "\nArguments:\n"
            "{\n"
            "  \"addresses\"\n"
            "    [\n"
            "      \"address\"  (string) The base58check encoded address\n"
            "      ,...\n"
            "    ]\n"
            "}\n"
            "\nResult:\n"
            "{\n"
            "  \"balance\"  (string) The current balance in satoshis\n"
            "  \"received\"  (string) The total number of satoshis received (including change)\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("getaddressbalance", "'{\"addresses\": [\"12c6DSiU4Rq3P4ZxziKxzrL5LmMBrzjrJX\"]}'") + HelpExampleRpc("getaddressbalance", "{\"addresses\": [\"12c6DSiU4Rq3P4ZxziKxzrL5LmMBrzjrJX\"]}"));

    std::vector<std::pair<uint160, int>> addresses;

    if (!getAddressesFromParams(params, addresses)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    std::vector<std::pair<CAddressIndexKey, CAmount>> addressIndex;

    for (std::vector<std::pair<uint160, int>>::iterator it = addresses.begin(); it != addresses.end(); it++) {
        if (!GetAddressIndex((*it).first, (*it).second, addressIndex)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
        }
    }

    CAmount balance = 0;
    CAmount received = 0;

    for (std::vector<std::pair<CAddressIndexKey, CAmount>>::const_iterator it = addressIndex.begin(); it != addressIndex.end(); it++) {
        if (it->second > 0) {
            received += it->second;
        }
        balance += it->second;
    }

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("balance", balance));
    result.push_back(Pair("received", received));

    return result;
}

UniValue getaddresstxids(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getaddresstxids\n"
            "\nReturns the txids for an address(es) (requires addressindex to be enabled).\n"
            "\nArguments:\n"
            "{\n"
            "  \"addresses\"\n"
            "    [\n"
            "      \"address\"  (string) The base58check encoded address\n"
            "      ,...\n"
            "    ]\n"
            "  \"start\" (number) The start block height\n"
            "  \"end\" (number) The end block height\n"
            "}\n"
            "\nResult:\n"
            "[\n"
            "  \"transactionid\"  (string) The transaction id\n"
            "  ,...\n"
            "]\n"
            "\nExamples:\n" +
            HelpExampleCli("getaddresstxids", "'{\"addresses\": [\"12c6DSiU4Rq3P4ZxziKxzrL5LmMBrzjrJX\"]}'") + HelpExampleRpc("getaddresstxids", "{\"addresses\": [\"12c6DSiU4Rq3P4ZxziKxzrL5LmMBrzjrJX\"]}"));

    std::vector<std::pair<uint160, int>> addresses;

    if (!getAddressesFromParams(params, addresses)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    int start = 0;
    int end = 0;
    if (params[0].isObject()) {
        UniValue startValue = find_value(params[0].get_obj(), "start");
        UniValue endValue = find_value(params[0].get_obj(), "end");
        if (startValue.isNum() && endValue.isNum()) {
            start = startValue.get_int();
            end = endValue.get_int();
        }
    }

    std::vector<std::pair<CAddressIndexKey, CAmount>> addressIndex;

    for (std::vector<std::pair<uint160, int>>::iterator it = addresses.begin(); it != addresses.end(); it++) {
        if (start > 0 && end > 0) {
            if (!GetAddressIndex((*it).first, (*it).second, addressIndex, start, end)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
            }
        } else {
            if (!GetAddressIndex((*it).first, (*it).second, addressIndex)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available for address");
            }
        }
    }

    std::set<std::pair<int, std::string>> txids;
    UniValue result(UniValue::VARR);

    for (std::vector<std::pair<CAddressIndexKey, CAmount>>::const_iterator it = addressIndex.begin(); it != addressIndex.end(); it++) {
        int height = it->first.blockHeight;
        std::string txid = it->first.txhash.GetHex();

        if (addresses.size() > 1) {
            txids.insert(std::make_pair(height, txid));
        } else {
            if (txids.insert(std::make_pair(height, txid)).second) {
                result.push_back(txid);
            }
        }
    }

    if (addresses.size() > 1) {
        for (std::set<std::pair<int, std::string>>::const_iterator it = txids.begin(); it != txids.end(); it++) {
            result.push_back(it->second);
        }
    }

    return result;
}

UniValue getspentinfo(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1 || !params[0].isObject())
        throw runtime_error(
            "getspentinfo\n"
            "\nReturns the txid and index where an output is spent.\n"
            "\nArguments:\n"
            "{\n"
            "  \"txid\" (string) The hex string of the txid\n"
            "  \"index\" (number) The start block height\n"
            "}\n"
            "\nResult:\n"
            "{\n"
            "  \"txid\"  (string) The transaction id\n"
            "  \"index\"  (number) The spending input index\n"
            "  ,...\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("getspentinfo", "'{\"txid\": \"0437cd7f8525ceed2324359c2d0ba26006d92d856a9c20fa0241106ee5a597c9\", \"index\": 0}'") + HelpExampleRpc("getspentinfo", "{\"txid\": \"0437cd7f8525ceed2324359c2d0ba26006d92d856a9c20fa0241106ee5a597c9\", \"index\": 0}"));

    UniValue txidValue = find_value(params[0].get_obj(), "txid");
    UniValue indexValue = find_value(params[0].get_obj(), "index");

    if (!txidValue.isStr() || !indexValue.isNum()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid txid or index");
    }

    uint256 txid = ParseHashV(txidValue, "txid");
    int outputIndex = indexValue.get_int();

    CSpentIndexKey key(txid, outputIndex);
    CSpentIndexValue value;

    if (!GetSpentIndex(key, value)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unable to get spent info");
    }

    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("txid", value.txid.GetHex()));
    obj.push_back(Pair("index", (int)value.inputIndex));
    obj.push_back(Pair("height", value.blockHeight));

    return obj;
}

static const CRPCCommand commands[] =
    {
        //  category              name                      actor (function)         okSafeMode
        //  --------------------- ------------------------  -----------------------  ----------
        {"control", "getalldata", &getalldata, true},
        {"control", "getamiinfo", &getamiinfo, true},
        {"control", "getinfo", &getinfo, true},                  /* uses wallet if enabled */
        {"util", "validateaddress", &validateaddress, true},     /* uses wallet if enabled */
        {"util", "z_validateaddress", &z_validateaddress, true}, /* uses wallet if enabled */
        {"util", "createmultisig", &createmultisig, true},
        {"util", "verifymessage", &verifymessage, true},
        /* Address index */
        {"addressindex", "getaddresstxids", &getaddresstxids, false},     /* insight explorer */
        {"addressindex", "getaddressbalance", &getaddressbalance, false}, /* insight explorer */
        {"addressindex", "getaddressdeltas", &getaddressdeltas, false},   /* insight explorer */
        {"addressindex", "getaddressutxos", &getaddressutxos, false},     /* insight explorer */
        {"addressindex", "getaddressmempool", &getaddressmempool, true},  /* insight explorer */
        {"blockchain", "getspentinfo", &getspentinfo, false},             /* insight explorer */
        /* Not shown in help */
        {"hidden", "setmocktime", &setmocktime, true},
};

void RegisterMiscRPCCommands(CRPCTable& tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
