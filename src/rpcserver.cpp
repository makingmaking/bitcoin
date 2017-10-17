// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpcserver.h"

#include "base58.h"
#include "init.h"
#include "random.h"
#include "sync.h"
#include "ui_interface.h"
#include "util.h"
#include "utilstrencodings.h"

#include <univalue.h>

#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/foreach.hpp>
#include <boost/iostreams/concepts.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/signals2/signal.hpp>
#include <boost/thread.hpp>
#include <boost/algorithm/string/case_conv.hpp> // for to_upper()

using namespace RPCServer;
using namespace std;

static bool fRPCRunning = false;
static bool fRPCInWarmup = true;
static std::string rpcWarmupStatus("RPC server started");
static CCriticalSection cs_rpcWarmup;
/* Timer-creating functions */
static std::vector<RPCTimerInterface*> timerInterfaces;
/* Map of name to timer.
 * @note Can be changed to std::unique_ptr when C++11 */
static std::map<std::string, boost::shared_ptr<RPCTimerBase> > deadlineTimers;

static struct CRPCSignals
{
    boost::signals2::signal<void ()> Started;
    boost::signals2::signal<void ()> Stopped;
    boost::signals2::signal<void (const CRPCCommand&)> PreCommand;
    boost::signals2::signal<void (const CRPCCommand&)> PostCommand;
} g_rpcSignals;

void RPCServer::OnStarted(boost::function<void ()> slot)
{
    g_rpcSignals.Started.connect(slot);
}

void RPCServer::OnStopped(boost::function<void ()> slot)
{
    g_rpcSignals.Stopped.connect(slot);
}

void RPCServer::OnPreCommand(boost::function<void (const CRPCCommand&)> slot)
{
    g_rpcSignals.PreCommand.connect(boost::bind(slot, _1));
}

void RPCServer::OnPostCommand(boost::function<void (const CRPCCommand&)> slot)
{
    g_rpcSignals.PostCommand.connect(boost::bind(slot, _1));
}

void RPCTypeCheck(const UniValue& params,
                  const list<UniValue::VType>& typesExpected,
                  bool fAllowNull)
{
    unsigned int i = 0;
    BOOST_FOREACH(UniValue::VType t, typesExpected)
    {
        if (params.size() <= i)
            break;

        const UniValue& v = params[i];
        if (!((v.type() == t) || (fAllowNull && (v.isNull()))))
        {
            string err = strprintf("Expected type %s, got %s",
                                   uvTypeName(t), uvTypeName(v.type()));
            throw JSONRPCError(RPC_TYPE_ERROR, err);
        }
        i++;
    }
}

void RPCTypeCheckObj(const UniValue& o,
                  const map<string, UniValue::VType>& typesExpected,
                  bool fAllowNull)
{
    BOOST_FOREACH(const PAIRTYPE(string, UniValue::VType)& t, typesExpected)
    {
        const UniValue& v = find_value(o, t.first);
        if (!fAllowNull && v.isNull())
            throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Missing %s", t.first));

        if (!((v.type() == t.second) || (fAllowNull && (v.isNull()))))
        {
            string err = strprintf("Expected type %s for %s, got %s",
                                   uvTypeName(t.second), t.first, uvTypeName(v.type()));
            throw JSONRPCError(RPC_TYPE_ERROR, err);
        }
    }
}

CAmount AmountFromValue(const UniValue& value)
{
    if (!value.isNum() && !value.isStr())
        throw JSONRPCError(RPC_TYPE_ERROR, "Amount is not a number or string");
    CAmount amount;
    if (!ParseFixedPoint(value.getValStr(), 8, &amount))
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");
    if (!MoneyRange(amount))
        throw JSONRPCError(RPC_TYPE_ERROR, "Amount out of range");
    return amount;
}

UniValue ValueFromAmount(const CAmount& amount)
{
    bool sign = amount < 0;
    int64_t n_abs = (sign ? -amount : amount);
    int64_t quotient = n_abs / COIN;
    int64_t remainder = n_abs % COIN;
    return UniValue(UniValue::VNUM,
            strprintf("%s%d.%08d", sign ? "-" : "", quotient, remainder));
}

uint256 ParseHashV(const UniValue& v, string strName)
{
    string strHex;
    if (v.isStr())
        strHex = v.get_str();
    if (!IsHex(strHex)) // Note: IsHex("") is false
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName+" must be hexadecimal string (not '"+strHex+"')");
    uint256 result;
    result.SetHex(strHex);
    return result;
}
uint256 ParseHashO(const UniValue& o, string strKey)
{
    return ParseHashV(find_value(o, strKey), strKey);
}
vector<unsigned char> ParseHexV(const UniValue& v, string strName)
{
    string strHex;
    if (v.isStr())
        strHex = v.get_str();
    if (!IsHex(strHex))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName+" must be hexadecimal string (not '"+strHex+"')");
    return ParseHex(strHex);
}
vector<unsigned char> ParseHexO(const UniValue& o, string strKey)
{
    return ParseHexV(find_value(o, strKey), strKey);
}

/**
 * Note: This interface may still be subject to change.
 */

std::string CRPCTable::help(const std::string& strCommand) const
{
    string strRet;
    string category;
    set<rpcfn_type> setDone;
    vector<pair<string, const CRPCCommand*> > vCommands;

    for (map<string, const CRPCCommand*>::const_iterator mi = mapCommands.begin(); mi != mapCommands.end(); ++mi)
        vCommands.push_back(make_pair(mi->second->category + mi->first, mi->second));
    sort(vCommands.begin(), vCommands.end());

    BOOST_FOREACH(const PAIRTYPE(string, const CRPCCommand*)& command, vCommands)
    {
        const CRPCCommand *pcmd = command.second;
        string strMethod = pcmd->name;
        // We already filter duplicates, but these deprecated screw up the sort order
        if (strMethod.find("label") != string::npos)
            continue;
        if ((strCommand != "" || pcmd->category == "hidden") && strMethod != strCommand)
            continue;
        try
        {
            UniValue params;
            rpcfn_type pfn = pcmd->actor;
            if (setDone.insert(pfn).second)
                (*pfn)(params, true);
        }
        catch (const std::exception& e)
        {
            // Help text is returned in an exception
            string strHelp = string(e.what());
            if (strCommand == "")
            {
                if (strHelp.find('\n') != string::npos)
                    strHelp = strHelp.substr(0, strHelp.find('\n'));

                if (category != pcmd->category)
                {
                    if (!category.empty())
                        strRet += "\n";
                    category = pcmd->category;
                    string firstLetter = category.substr(0,1);
                    boost::to_upper(firstLetter);
                    strRet += "== " + firstLetter + category.substr(1) + " ==\n";
                }
            }
            strRet += strHelp + "\n";
        }
    }
    if (strRet == "")
        strRet = strprintf("help: unknown command: %s\n", strCommand);
    strRet = strRet.substr(0,strRet.size()-1);
    return strRet;
}

UniValue help(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "help ( \"command\" )\n"
            "\nList all commands, or get help for a specified command.\n"
            "\nArguments:\n"
            "1. \"command\"     (string, optional) The command to get help on\n"
            "\nResult:\n"
            "\"text\"     (string) The help text\n"
        );

    string strCommand;
    if (params.size() > 0)
        strCommand = params[0].get_str();

    return tableRPC.help(strCommand);
}


UniValue stop(const UniValue& params, bool fHelp)
{
    // Accept the deprecated and ignored 'detach' boolean argument
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "stop\n"
            "\nStop Bitcoin server.");
    // Event loop will exit after current HTTP requests have been handled, so
    // this reply will get back to the client.
    StartShutdown();
    return "Bitcoin server stopping";
}

/**
 * Call Table
 */
static const CRPCCommand vRPCCommands[] =
{ //  category              name                      actor (function)         okSafeMode
  //  --------------------- ------------------------  -----------------------  ----------
    /* Overall control/query calls */
    { "control",            "getinfo",                &getinfo,                true  }, /* uses wallet if enabled */
    { "control",            "help",                   &help,                   true  },
    { "control",            "stop",                   &stop,                   true  },

    /* P2P networking */
    { "network",            "getnetworkinfo",         &getnetworkinfo,         true  },
    { "network",            "addnode",                &addnode,                true  },
    { "network",            "disconnectnode",         &disconnectnode,         true  },
    { "network",            "getaddednodeinfo",       &getaddednodeinfo,       true  },
    { "network",            "getconnectioncount",     &getconnectioncount,     true  },
    { "network",            "getnettotals",           &getnettotals,           true  },
    { "network",            "getpeerinfo",            &getpeerinfo,            true  },
    { "network",            "ping",                   &ping,                   true  },
    { "network",            "setban",                 &setban,                 true  },
    { "network",            "listbanned",             &listbanned,             true  },
    { "network",            "clearbanned",            &clearbanned,            true  },

    /* Block chain and UTXO */
    { "blockchain",         "getblockchaininfo",      &getblockchaininfo,      true  },
    { "blockchain",         "getbestblockhash",       &getbestblockhash,       true  },
    { "blockchain",         "getblockcount",          &getblockcount,          true  },
    { "blockchain",         "getblock",               &getblock,               true  },
    { "blockchain",         "getblockhash",           &getblockhash,           true  },
    { "blockchain",         "getblockheader",         &getblockheader,         true  },
    { "blockchain",         "getchaintips",           &getchaintips,           true  },
    { "blockchain",         "getdifficulty",          &getdifficulty,          true  },
    { "blockchain",         "getmempoolinfo",         &getmempoolinfo,         true  },
    { "blockchain",         "getrawmempool",          &getrawmempool,          true  },
    { "blockchain",         "gettxout",               &gettxout,               true  },
    { "blockchain",         "gettxoutproof",          &gettxoutproof,          true  },
    { "blockchain",         "verifytxoutproof",       &verifytxoutproof,       true  },
    { "blockchain",         "gettxoutsetinfo",        &gettxoutsetinfo,        true  },
    { "blockchain",         "verifychain",            &verifychain,            true  },

    /* Mining */
    { "mining",             "getblocktemplate",       &getblocktemplate,       true  },
    { "mining",             "getmininginfo",          &getmininginfo,          true  },
    { "mining",             "getnetworkhashps",       &getnetworkhashps,       true  },
    { "mining",             "prioritisetransaction",  &prioritisetransaction,  true  },
    { "mining",             "submitblock",            &submitblock,            true  },

    /* Coin generation */
    { "generating",         "getgenerate",            &getgenerate,            true  },
    { "generating",         "setgenerate",            &setgenerate,            true  },
    { "generating",         "generate",               &generate,               true  },

    /* Raw transactions */
    { "rawtransactions",    "createrawtransaction",   &createrawtransaction,   true  },
    { "rawtransactions",    "decoderawtransaction",   &decoderawtransaction,   true  },
    { "rawtransactions",    "decodescript",           &decodescript,           true  },
    { "rawtransactions",    "getrawtransaction",      &getrawtransaction,      true  },
    { "rawtransactions",    "sendrawtransaction",     &sendrawtransaction,     false },
    { "rawtransactions",    "signrawtransaction",     &signrawtransaction,     false }, /* uses wallet if enabled */
#ifdef ENABLE_WALLET
    { "rawtransactions",    "fundrawtransaction",     &fundrawtransaction,     false },
#endif

    /* Utility functions */
    { "util",               "createmultisig",         &createmultisig,         true  },
    { "util",               "validateaddress",        &validateaddress,        true  }, /* uses wallet if enabled */
    { "util",               "verifymessage",          &verifymessage,          true  },
    { "util",               "estimatefee",            &estimatefee,            true  },
    { "util",               "estimatepriority",       &estimatepriority,       true  },

    /* Not shown in help */
    { "hidden",             "invalidateblock",        &invalidateblock,        true  },
    { "hidden",             "reconsiderblock",        &reconsiderblock,        true  },
    { "hidden",             "setmocktime",            &setmocktime,            true  },
#ifdef ENABLE_WALLET
    { "hidden",             "resendwallettransactions", &resendwallettransactions, true},
#endif

#ifdef ENABLE_WALLET
    /* Wallet */
    { "wallet",             "addmultisigaddress",     &addmultisigaddress,     true  },
    { "wallet",             "backupwallet",           &backupwallet,           true  },
    { "wallet",             "dumpprivkey",            &dumpprivkey,            true  },
    { "wallet",             "dumpwallet",             &dumpwallet,             true  },
    { "wallet",             "encryptwallet",          &encryptwallet,          true  },
    { "wallet",             "getaccountaddress",      &getaccountaddress,      true  },
    { "wallet",             "getaccount",             &getaccount,             true  },
    { "wallet",             "getaddressesbyaccount",  &getaddressesbyaccount,  true  },
    { "wallet",             "getbalance",             &getbalance,             false },
    { "wallet",             "getnewaddress",          &getnewaddress,          true  },
    { "wallet",             "getrawchangeaddress",    &getrawchangeaddress,    true  },
    { "wallet",             "getreceivedbyaccount",   &getreceivedbyaccount,   false },
    { "wallet",             "getreceivedbyaddress",   &getreceivedbyaddress,   false },
    { "wallet",             "gettransaction",         &gettransaction,         false },
    { "wallet",             "getunconfirmedbalance",  &getunconfirmedbalance,  false },
    { "wallet",             "getwalletinfo",          &getwalletinfo,          false },
    { "wallet",             "importprivkey",          &importprivkey,          true  },
    { "wallet",             "importwallet",           &importwallet,           true  },
    { "wallet",             "importaddress",          &importaddress,          true  },
    { "wallet",             "importpubkey",           &importpubkey,           true  },
    { "wallet",             "keypoolrefill",          &keypoolrefill,          true  },
    { "wallet",             "listaccounts",           &listaccounts,           false },
    { "wallet",             "listaddressgroupings",   &listaddressgroupings,   false },
    { "wallet",             "listlockunspent",        &listlockunspent,        false },
    { "wallet",             "listreceivedbyaccount",  &listreceivedbyaccount,  false },
    { "wallet",             "listreceivedbyaddress",  &listreceivedbyaddress,  false },
    { "wallet",             "listsinceblock",         &listsinceblock,         false },
    { "wallet",             "listtransactions",       &listtransactions,       false },
    { "wallet",             "listunspent",            &listunspent,            false },
    { "wallet",             "lockunspent",            &lockunspent,            true  },
    { "wallet",             "move",                   &movecmd,                false },
    { "wallet",             "sendfrom",               &sendfrom,               false },
    { "wallet",             "sendmany",               &sendmany,               false },
    { "wallet",             "sendtoaddress",          &sendtoaddress,          false },
    { "wallet",             "setaccount",             &setaccount,             true  },
    { "wallet",             "settxfee",               &settxfee,               true  },
    { "wallet",             "signmessage",            &signmessage,            true  },
    { "wallet",             "walletlock",             &walletlock,             true  },
    { "wallet",             "walletpassphrasechange", &walletpassphrasechange, true  },
    { "wallet",             "walletpassphrase",       &walletpassphrase,       true  },
#endif // ENABLE_WALLET
};

CRPCTable::CRPCTable()
{
    unsigned int vcidx;
    for (vcidx = 0; vcidx < (sizeof(vRPCCommands) / sizeof(vRPCCommands[0])); vcidx++)
    {
        const CRPCCommand *pcmd;

        pcmd = &vRPCCommands[vcidx];
        mapCommands[pcmd->name] = pcmd;
    }
}

const CRPCCommand *CRPCTable::operator[](const std::string &name) const
{
    map<string, const CRPCCommand*>::const_iterator it = mapCommands.find(name);
    if (it == mapCommands.end())
        return NULL;
    return (*it).second;
}

bool StartRPC()
{
<<<<<<< HEAD
    LogPrint("rpc", "Starting RPC\n");
=======
    CNetAddr netaddr;
    // Make sure that IPv4-compatible and IPv4-mapped IPv6 addresses are treated as IPv4 addresses
    if (address.is_v6()
     && (address.to_v6().is_v4_compatible()
      || address.to_v6().is_v4_mapped()))
        address = address.to_v6().to_v4();

    if(address.is_v4())
    {
        boost::asio::ip::address_v4::bytes_type bytes = address.to_v4().to_bytes();
        netaddr.SetRaw(NET_IPV4, &bytes[0]);
    }
    else
    {
        boost::asio::ip::address_v6::bytes_type bytes = address.to_v6().to_bytes();
        netaddr.SetRaw(NET_IPV6, &bytes[0]);
    }
    return netaddr;
}

bool ClientAllowed(const boost::asio::ip::address& address)
{
    CNetAddr netaddr = BoostAsioToCNetAddr(address);
    BOOST_FOREACH(const CSubNet &subnet, rpc_allow_subnets)
        if (subnet.Match(netaddr))
            return true;
    return false;
}

template <typename Protocol>
class AcceptedConnectionImpl : public AcceptedConnection
{
public:
    AcceptedConnectionImpl(
            asio::io_service& io_service,
            ssl::context &context,
            bool fUseSSL) :
        sslStream(io_service, context),
        _d(sslStream, fUseSSL),
        _stream(_d)
    {
    }

    virtual std::iostream& stream()
    {
        return _stream;
    }

    virtual std::string peer_address_to_string() const
    {
        return peer.address().to_string();
    }

    virtual void close()
    {
        _stream.close();
    }

    typename Protocol::endpoint peer;
    asio::ssl::stream<typename Protocol::socket> sslStream;

private:
    SSLIOStreamDevice<Protocol> _d;
    iostreams::stream< SSLIOStreamDevice<Protocol> > _stream;
};

void ServiceConnection(AcceptedConnection *conn);

//! Forward declaration required for RPCListen
template <typename Protocol, typename SocketAcceptorService>
static void RPCAcceptHandler(boost::shared_ptr< basic_socket_acceptor<Protocol, SocketAcceptorService> > acceptor,
                             ssl::context& context,
                             bool fUseSSL,
                             boost::shared_ptr< AcceptedConnection > conn,
                             const boost::system::error_code& error);

/**
 * Sets up I/O resources to accept and handle a new connection.
 */
template <typename Protocol, typename SocketAcceptorService>
static void RPCListen(boost::shared_ptr< basic_socket_acceptor<Protocol, SocketAcceptorService> > acceptor,
                   ssl::context& context,
                   const bool fUseSSL)
{
    // Accept connection
    boost::shared_ptr< AcceptedConnectionImpl<Protocol> > conn(new AcceptedConnectionImpl<Protocol>(acceptor->get_io_service(), context, fUseSSL));

    acceptor->async_accept(
            conn->sslStream.lowest_layer(),
            conn->peer,
            boost::bind(&RPCAcceptHandler<Protocol, SocketAcceptorService>,
                acceptor,
                boost::ref(context),
                fUseSSL,
                conn,
                _1));
}


/**
 * Accept and handle incoming connection.
 */
template <typename Protocol, typename SocketAcceptorService>
static void RPCAcceptHandler(boost::shared_ptr< basic_socket_acceptor<Protocol, SocketAcceptorService> > acceptor,
                             ssl::context& context,
                             const bool fUseSSL,
                             boost::shared_ptr< AcceptedConnection > conn,
                             const boost::system::error_code& error)
{
    // Immediately start accepting new connections, except when we're cancelled or our socket is closed.
    if (error != asio::error::operation_aborted && acceptor->is_open())
        RPCListen(acceptor, context, fUseSSL);

    AcceptedConnectionImpl<ip::tcp>* tcp_conn = dynamic_cast< AcceptedConnectionImpl<ip::tcp>* >(conn.get());

    if (error)
    {
        // TODO: Actually handle errors
        LogPrintf("%s: Error: %s\n", __func__, error.message());
    }
    // Restrict callers by IP.  It is important to
    // do this before starting client thread, to filter out
    // certain DoS and misbehaving clients.
    else if (tcp_conn && !ClientAllowed(tcp_conn->peer.address()))
    {
        // Only send a 403 if we're not using SSL to prevent a DoS during the SSL handshake.
        if (!fUseSSL)
            conn->stream() << HTTPError(HTTP_FORBIDDEN, false) << std::flush;
        conn->close();
    }
    else {
        ServiceConnection(conn.get());
        conn->close();
    }
}

static ip::tcp::endpoint ParseEndpoint(const std::string &strEndpoint, int defaultPort)
{
    std::string addr;
    int port = defaultPort;
    SplitHostPort(strEndpoint, port, addr);
    return ip::tcp::endpoint(asio::ip::address::from_string(addr), port);
}

void StartRPCThreads()
{
    rpc_allow_subnets.clear();
    rpc_allow_subnets.push_back(CSubNet("127.0.0.0/8")); // always allow IPv4 local subnet
    rpc_allow_subnets.push_back(CSubNet("::1")); // always allow IPv6 localhost
    if (mapMultiArgs.count("-rpcallowip"))
    {
        const vector<string>& vAllow = mapMultiArgs["-rpcallowip"];
        BOOST_FOREACH(string strAllow, vAllow)
        {
            CSubNet subnet(strAllow);
            if(!subnet.IsValid())
            {
                uiInterface.ThreadSafeMessageBox(
                    strprintf("Invalid -rpcallowip subnet specification: %s. Valid are a single IP (e.g. 1.2.3.4), a network/netmask (e.g. 1.2.3.4/255.255.255.0) or a network/CIDR (e.g. 1.2.3.4/24).", strAllow),
                    "", CClientUIInterface::MSG_ERROR);
                StartShutdown();
                return;
            }
            rpc_allow_subnets.push_back(subnet);
        }
    }
    std::string strAllowed;
    BOOST_FOREACH(const CSubNet &subnet, rpc_allow_subnets)
        strAllowed += subnet.ToString() + " ";
    LogPrint("rpc", "Allowing RPC connections from: %s\n", strAllowed);

    strRPCUserColonPass = mapArgs["-rpcuser"] + ":" + mapArgs["-rpcpassword"];
    if (((mapArgs["-rpcpassword"] == "") ||
         (mapArgs["-rpcuser"] == mapArgs["-rpcpassword"])) && Params().RequireRPCPassword())
    {
        unsigned char rand_pwd[32];
        GetRandBytes(rand_pwd, 32);
        uiInterface.ThreadSafeMessageBox(strprintf(
            _("To use bitcoind, or the -server option to bitcoin-qt, you must set an rpcpassword in the configuration file:\n"
              "%s\n"
              "It is recommended you use the following random password:\n"
              "rpcuser=bitcoinrpc\n"
              "rpcpassword=%s\n"
              "(you do not need to remember this password)\n"
              "The username and password MUST NOT be the same.\n"
              "If the file does not exist, create it with owner-readable-only file permissions.\n"
              "It is also recommended to set alertnotify so you are notified of problems;\n"
              "for example: alertnotify=echo %%s | mail -s \"Bitcoin Alert\" admin@foo.com\n"),
                GetConfigFile().string(),
                EncodeBase58(&rand_pwd[0],&rand_pwd[0]+32)),
                "", CClientUIInterface::MSG_ERROR | CClientUIInterface::SECURE);
        StartShutdown();
        return;
    }

    assert(rpc_io_service == NULL);
    rpc_io_service = new asio::io_service();
    rpc_ssl_context = new ssl::context(*rpc_io_service, ssl::context::sslv23);

    const bool fUseSSL = GetBoolArg("-rpcssl", false);

    if (fUseSSL)
    {
        rpc_ssl_context->set_options(ssl::context::no_sslv2 | ssl::context::no_sslv3);

        filesystem::path pathCertFile(GetArg("-rpcsslcertificatechainfile", "server.cert"));
        if (!pathCertFile.is_complete()) pathCertFile = filesystem::path(GetDataDir()) / pathCertFile;
        if (filesystem::exists(pathCertFile)) rpc_ssl_context->use_certificate_chain_file(pathCertFile.string());
        else LogPrintf("ThreadRPCServer ERROR: missing server certificate file %s\n", pathCertFile.string());

        filesystem::path pathPKFile(GetArg("-rpcsslprivatekeyfile", "server.pem"));
        if (!pathPKFile.is_complete()) pathPKFile = filesystem::path(GetDataDir()) / pathPKFile;
        if (filesystem::exists(pathPKFile)) rpc_ssl_context->use_private_key_file(pathPKFile.string(), ssl::context::pem);
        else LogPrintf("ThreadRPCServer ERROR: missing server private key file %s\n", pathPKFile.string());

        string strCiphers = GetArg("-rpcsslciphers", "TLSv1.2+HIGH:TLSv1+HIGH:!SSLv2:!aNULL:!eNULL:!3DES:@STRENGTH");
        SSL_CTX_set_cipher_list(rpc_ssl_context->impl(), strCiphers.c_str());
    }

    std::vector<ip::tcp::endpoint> vEndpoints;
    bool bBindAny = false;
    int defaultPort = GetArg("-rpcport", BaseParams().RPCPort());
    if (!mapArgs.count("-rpcallowip")) // Default to loopback if not allowing external IPs
    {
        vEndpoints.push_back(ip::tcp::endpoint(asio::ip::address_v6::loopback(), defaultPort));
        vEndpoints.push_back(ip::tcp::endpoint(asio::ip::address_v4::loopback(), defaultPort));
        if (mapArgs.count("-rpcbind"))
        {
            LogPrintf("WARNING: option -rpcbind was ignored because -rpcallowip was not specified, refusing to allow everyone to connect\n");
        }
    } else if (mapArgs.count("-rpcbind")) // Specific bind address
    {
        BOOST_FOREACH(const std::string &addr, mapMultiArgs["-rpcbind"])
        {
            try {
                vEndpoints.push_back(ParseEndpoint(addr, defaultPort));
            }
            catch(const boost::system::system_error &)
            {
                uiInterface.ThreadSafeMessageBox(
                    strprintf(_("Could not parse -rpcbind value %s as network address"), addr),
                    "", CClientUIInterface::MSG_ERROR);
                StartShutdown();
                return;
            }
        }
    } else { // No specific bind address specified, bind to any
        vEndpoints.push_back(ip::tcp::endpoint(asio::ip::address_v6::any(), defaultPort));
        vEndpoints.push_back(ip::tcp::endpoint(asio::ip::address_v4::any(), defaultPort));
        // Prefer making the socket dual IPv6/IPv4 instead of binding
        // to both addresses separately.
        bBindAny = true;
    }

    bool fListening = false;
    std::string strerr;
    std::string straddress;
    BOOST_FOREACH(const ip::tcp::endpoint &endpoint, vEndpoints)
    {
        try {
            asio::ip::address bindAddress = endpoint.address();
            straddress = bindAddress.to_string();
            LogPrintf("Binding RPC on address %s port %i (IPv4+IPv6 bind any: %i)\n", straddress, endpoint.port(), bBindAny);
            boost::system::error_code v6_only_error;
            boost::shared_ptr<ip::tcp::acceptor> acceptor(new ip::tcp::acceptor(*rpc_io_service));

            acceptor->open(endpoint.protocol());
            acceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));

            // Try making the socket dual IPv6/IPv4 when listening on the IPv6 "any" address
            acceptor->set_option(boost::asio::ip::v6_only(
                !bBindAny || bindAddress != asio::ip::address_v6::any()), v6_only_error);

            acceptor->bind(endpoint);
            acceptor->listen(socket_base::max_connections);

            RPCListen(acceptor, *rpc_ssl_context, fUseSSL);

            fListening = true;
            rpc_acceptors.push_back(acceptor);
            // If dual IPv6/IPv4 bind successful, skip binding to IPv4 separately
            if(bBindAny && bindAddress == asio::ip::address_v6::any() && !v6_only_error)
                break;
        }
        catch(boost::system::system_error &e)
        {
            LogPrintf("ERROR: Binding RPC on address %s port %i failed: %s\n", straddress, endpoint.port(), e.what());
            strerr = strprintf(_("An error occurred while setting up the RPC address %s port %u for listening: %s"), straddress, endpoint.port(), e.what());
        }
    }

    if (!fListening) {
        uiInterface.ThreadSafeMessageBox(strerr, "", CClientUIInterface::MSG_ERROR);
        StartShutdown();
        return;
    }

    rpc_worker_group = new boost::thread_group();
    for (int i = 0; i < GetArg("-rpcthreads", 4); i++)
        rpc_worker_group->create_thread(boost::bind(&asio::io_service::run, rpc_io_service));
>>>>>>> refs/remotes/origin/0.10
    fRPCRunning = true;
    g_rpcSignals.Started();
    return true;
}

void InterruptRPC()
{
    LogPrint("rpc", "Interrupting RPC\n");
    // Interrupt e.g. running longpolls
    fRPCRunning = false;
}

void StopRPC()
{
    LogPrint("rpc", "Stopping RPC\n");
    deadlineTimers.clear();
    g_rpcSignals.Stopped();
}

bool IsRPCRunning()
{
    return fRPCRunning;
}

void SetRPCWarmupStatus(const std::string& newStatus)
{
    LOCK(cs_rpcWarmup);
    rpcWarmupStatus = newStatus;
}

void SetRPCWarmupFinished()
{
    LOCK(cs_rpcWarmup);
    assert(fRPCInWarmup);
    fRPCInWarmup = false;
}

bool RPCIsInWarmup(std::string *outStatus)
{
    LOCK(cs_rpcWarmup);
    if (outStatus)
        *outStatus = rpcWarmupStatus;
    return fRPCInWarmup;
}

void JSONRequest::parse(const UniValue& valRequest)
{
    // Parse request
    if (!valRequest.isObject())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Invalid Request object");
    const UniValue& request = valRequest.get_obj();

    // Parse id now so errors from here on will have the id
    id = find_value(request, "id");

    // Parse method
    UniValue valMethod = find_value(request, "method");
    if (valMethod.isNull())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Missing method");
    if (!valMethod.isStr())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Method must be a string");
    strMethod = valMethod.get_str();
    if (strMethod != "getblocktemplate")
        LogPrint("rpc", "ThreadRPCServer method=%s\n", SanitizeString(strMethod));

    // Parse params
    UniValue valParams = find_value(request, "params");
    if (valParams.isArray())
        params = valParams.get_array();
    else if (valParams.isNull())
        params = UniValue(UniValue::VARR);
    else
        throw JSONRPCError(RPC_INVALID_REQUEST, "Params must be an array");
}

static UniValue JSONRPCExecOne(const UniValue& req)
{
    UniValue rpc_result(UniValue::VOBJ);

    JSONRequest jreq;
    try {
        jreq.parse(req);

        UniValue result = tableRPC.execute(jreq.strMethod, jreq.params);
        rpc_result = JSONRPCReplyObj(result, NullUniValue, jreq.id);
    }
    catch (const UniValue& objError)
    {
        rpc_result = JSONRPCReplyObj(NullUniValue, objError, jreq.id);
    }
    catch (const std::exception& e)
    {
        rpc_result = JSONRPCReplyObj(NullUniValue,
                                     JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
    }

    return rpc_result;
}

std::string JSONRPCExecBatch(const UniValue& vReq)
{
    UniValue ret(UniValue::VARR);
    for (unsigned int reqIdx = 0; reqIdx < vReq.size(); reqIdx++)
        ret.push_back(JSONRPCExecOne(vReq[reqIdx]));

    return ret.write() + "\n";
}

UniValue CRPCTable::execute(const std::string &strMethod, const UniValue &params) const
{
    // Return immediately if in warmup
    {
<<<<<<< HEAD
        LOCK(cs_rpcWarmup);
        if (fRPCInWarmup)
            throw JSONRPCError(RPC_IN_WARMUP, rpcWarmupStatus);
=======
        int nProto = 0;
        map<string, string> mapHeaders;
        string strRequest, strMethod, strURI;

        // Read HTTP request line
        if (!ReadHTTPRequestLine(conn->stream(), nProto, strMethod, strURI))
            break;

        // Read HTTP message headers and body
        ReadHTTPMessage(conn->stream(), mapHeaders, strRequest, nProto, MAX_SIZE);

        // HTTP Keep-Alive is false; close connection immediately
        if ((mapHeaders["connection"] == "close") || (!GetBoolArg("-rpckeepalive", true)))
            fRun = false;

        // Process via JSON-RPC API
        if (strURI == "/") {
            if (!HTTPReq_JSONRPC(conn, strRequest, mapHeaders, fRun))
                break;

        // Process via HTTP REST API
        } else if (strURI.substr(0, 6) == "/rest/" && GetBoolArg("-rest", false)) {
            if (!HTTPReq_REST(conn, strURI, mapHeaders, fRun))
                break;

        } else {
            conn->stream() << HTTPError(HTTP_NOT_FOUND, false) << std::flush;
            break;
        }
>>>>>>> refs/remotes/origin/0.10
    }

    // Find method
    const CRPCCommand *pcmd = tableRPC[strMethod];
    if (!pcmd)
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found");

    g_rpcSignals.PreCommand(*pcmd);

    try
    {
        // Execute
        return pcmd->actor(params, false);
    }
    catch (const std::exception& e)
    {
        throw JSONRPCError(RPC_MISC_ERROR, e.what());
    }

    g_rpcSignals.PostCommand(*pcmd);
}

std::string HelpExampleCli(const std::string& methodname, const std::string& args)
{
    return "> bitcoin-cli " + methodname + " " + args + "\n";
}

std::string HelpExampleRpc(const std::string& methodname, const std::string& args)
{
    return "> curl --user myusername --data-binary '{\"jsonrpc\": \"1.0\", \"id\":\"curltest\", "
        "\"method\": \"" + methodname + "\", \"params\": [" + args + "] }' -H 'content-type: text/plain;' http://127.0.0.1:8332/\n";
}

void RPCRegisterTimerInterface(RPCTimerInterface *iface)
{
    timerInterfaces.push_back(iface);
}

void RPCUnregisterTimerInterface(RPCTimerInterface *iface)
{
    std::vector<RPCTimerInterface*>::iterator i = std::find(timerInterfaces.begin(), timerInterfaces.end(), iface);
    assert(i != timerInterfaces.end());
    timerInterfaces.erase(i);
}

void RPCRunLater(const std::string& name, boost::function<void(void)> func, int64_t nSeconds)
{
    if (timerInterfaces.empty())
        throw JSONRPCError(RPC_INTERNAL_ERROR, "No timer handler registered for RPC");
    deadlineTimers.erase(name);
    RPCTimerInterface* timerInterface = timerInterfaces[0];
    LogPrint("rpc", "queue run of timer %s in %i seconds (using %s)\n", name, nSeconds, timerInterface->Name());
    deadlineTimers.insert(std::make_pair(name, boost::shared_ptr<RPCTimerBase>(timerInterface->NewTimer(func, nSeconds*1000))));
}

const CRPCTable tableRPC;
