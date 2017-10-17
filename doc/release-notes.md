Bitcoin Core version 0.10.4 is now available from:

<<<<<<< HEAD
Notable changes
===============

SSL support for RPC dropped
----------------------------
=======
  <https://bitcoin.org/bin/bitcoin-core-0.10.4/>

This is a new minor version release, bringing bug fixes, the BIP65
(CLTV) consensus change, and relay policy preparation for BIP113. It is
recommended to upgrade to this version as soon as possible.

Please report bugs using the issue tracker at github:

  <https://github.com/bitcoin/bitcoin/issues>

Upgrading and downgrading
=========================

How to Upgrade
--------------

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes for older versions), then run the
installer (on Windows) or just copy over /Applications/Bitcoin-Qt (on Mac) or
bitcoind/bitcoin-qt (on Linux).

Downgrade warning
------------------

Because release 0.10.0 and later makes use of headers-first synchronization and
parallel block download (see further), the block files and databases are not
backwards-compatible with pre-0.10 versions of Bitcoin Core or other software:
>>>>>>> refs/remotes/origin/0.10

SSL support for RPC, previously enabled by the option `rpcssl` has been dropped
from both the client and the server. This was done in preparation for removing
the dependency on OpenSSL for the daemon completely.

Trying to use `rpcssl` will result in an error:

<<<<<<< HEAD
    Error: SSL mode for RPC (-rpcssl) is no longer supported.

If you are one of the few people that relies on this feature, a flexible
migration path is to use `stunnel`. This is an utility that can tunnel
arbitrary TCP connections inside SSL. On e.g. Ubuntu it can be installed with:

    sudo apt-get install stunnel4

Then, to tunnel a SSL connection on 28332 to a RPC server bound on localhost on port 18332 do:

    stunnel -d 28332 -r 127.0.0.1:18332 -p stunnel.pem -P ''

It can also be set up system-wide in inetd style.

Another way to re-attain SSL would be to setup a httpd reverse proxy. This solution
would allow the use of different authentication, loadbalancing, on-the-fly compression and
caching. A sample config for apache2 could look like:

    Listen 443

    NameVirtualHost *:443
    <VirtualHost *:443>

    SSLEngine On
    SSLCertificateFile /etc/apache2/ssl/server.crt
    SSLCertificateKeyFile /etc/apache2/ssl/server.key

    <Location /bitcoinrpc>
        ProxyPass http://127.0.0.1:8332/
        ProxyPassReverse http://127.0.0.1:8332/
        # optional enable digest auth
        # AuthType Digest
        # ...

        # optional bypass bitcoind rpc basic auth
        # RequestHeader set Authorization "Basic <hash>"
        # get the <hash> from the shell with: base64 <<< bitcoinrpc:<password>
    </Location>

    # Or, balance the load:
    # ProxyPass / balancer://balancer_cluster_name

    </VirtualHost>

Random-cookie RPC authentication
---------------------------------

When no `-rpcpassword` is specified, the daemon now uses a special 'cookie'
file for authentication. This file is generated with random content when the
daemon starts, and deleted when it exits. Its contents are used as
authentication token. Read access to this file controls who can access through
RPC. By default it is stored in the data directory but its location can be
overridden with the option `-rpccookiefile`.

This is similar to Tor's CookieAuthentication: see
https://www.torproject.org/docs/tor-manual.html.en

This allows running bitcoind without having to do any manual configuration.

Low-level RPC API changes
--------------------------

- Monetary amounts can be provided as strings. This means that for example the
  argument to sendtoaddress can be "0.0001" instead of 0.0001. This can be an
  advantage if a JSON library insists on using a lossy floating point type for
  numbers, which would be dangerous for monetary amounts.

Option parsing behavior
-----------------------

Command line options are now parsed strictly in the order in which they are
specified. It used to be the case that `-X -noX` ends up, unintuitively, with X
set, as `-X` had precedence over `-noX`. This is no longer the case. Like for
other software, the last specified value for an option will hold.

`NODE_BLOOM` service bit
------------------------

Support for the `NODE_BLOOM` service bit, as described in [BIP
111](https://github.com/bitcoin/bips/blob/master/bip-0111.mediawiki), has been
added to the P2P protocol code.

BIP 111 defines a service bit to allow peers to advertise that they support
bloom filters (such as used by SPV clients) explicitly. It also bumps the protocol
version to allow peers to identify old nodes which allow bloom filtering of the
connection despite lacking the new service bit.

In this version, it is only enforced for peers that send protocol versions
`>=70011`. For the next major version it is planned that this restriction will be
removed. It is recommended to update SPV clients to check for the `NODE_BLOOM`
service bit for nodes that report versions newer than 70011.

Any sequence of pushdatas in OP_RETURN outputs now allowed
----------------------------------------------------------

Previously OP_RETURN outputs with a payload were only relayed and mined if they
had a single pushdata. This restriction has been lifted to allow any
combination of data pushes and numeric constant opcodes (OP_1 to OP_16). The
limit on OP_RETURN output size is now applied to the entire serialized
scriptPubKey, 83 bytes by default. (the previous 80 byte default plus three
bytes overhead)

Merkle branches removed from wallet
-----------------------------------

Previously, every wallet transaction stored a Merkle branch to prove its
presence in blocks. This wasn't being used for more than an expensive
sanity check. Since 0.12, these are no longer stored. When loading a
0.12 wallet into an older version, it will automatically rescan to avoid
failed checks.

BIP65 - CHECKLOCKTIMEVERIFY
---------------------------

Previously it was impossible to create a transaction output that was guaranteed
to be unspendable until a specific date in the future. CHECKLOCKTIMEVERIFY is a
new opcode that allows a script to check if a specific block height or time has
been reached, failing the script otherwise. This enables a wide variety of new
functionality such as time-locked escrows, secure payment channels, etc.

BIP65 implements CHECKLOCKTIMEVERIFY by introducing block version 4, which adds
additional restrictions to the NOP2 opcode. The same miner-voting mechanism as
in BIP34 and BIP66 is used: when 751 out of a sequence of 1001 blocks have
version number 4 or higher, the new consensus rule becomes active for those
blocks. When 951 out of a sequence of 1001 blocks have version number 4 or
higher, it becomes mandatory for all blocks and blocks with versions less than
4 are rejected.

Bitcoin Core's block templates are now for version 4 blocks only, and any
mining software relying on its `getblocktemplate` must be updated in parallel
to use either libblkmaker version 0.4.3 or any version from 0.5.2 onward. If
you are solo mining, this will affect you the moment you upgrade Bitcoin Core,
which must be done prior to BIP65 achieving its 951/1001 status.  If you are
mining with the stratum mining protocol: this does not affect you.  If you are
mining with the getblocktemplate protocol to a pool: this will affect you at
the pool operator's discretion, which must be no later than BIP65 achieving its
951/1001 status.

Automatically listen on Tor
----------------------------

Starting with Tor version 0.2.7.1 it is possible, through Tor's control socket
API, to create and destroy 'ephemeral' hidden services programmatically.
Bitcoin Core has been updated to make use of this.

This means that if Tor is running (and proper authorization is available),
Bitcoin Core automatically creates a hidden service to listen on, without
manual configuration. This will positively affect the number of available
.onion nodes.

This new feature is enabled by default if Bitcoin Core is listening, and
a connection to Tor can be made. It can be configured with the `-listenonion`,
`-torcontrol` and `-torpassword` settings. To show verbose debugging
information, pass `-debug=tor`.

Reduce upload traffic
---------------------

A major part of the outbound traffic is caused by serving historic blocks to
other nodes in initial block download state.

It is now possible to reduce the total upload traffic via the `-maxuploadtarget`
parameter. This is *not* a hard limit but a threshold to minimize the outbound
traffic. When the limit is about to be reached, the uploaded data is cut by not
serving historic blocks (blocks older than one week).
Moreover, any SPV peer is disconnected when they request a filtered block.

This option can be specified in MiB per day and is turned off by default
(`-maxuploadtarget=0`).
The recommended minimum is 144 * MAX_BLOCK_SIZE (currently 144MB) per day.

Whitelisted peers will never be disconnected, although their traffic counts for
calculating the target.

A more detailed documentation about keeping traffic low can be found in
[/doc/reducetraffic.md](/doc/reducetraffic.md).

0.12.0 Change log
=======
If you want to be able to downgrade smoothly, make a backup of your entire data
directory. Without this your node will need start syncing (or importing from
bootstrap.dat) anew afterwards. It is possible that the data from a completely
synchronised 0.10 node may be usable in older versions as-is, but this is not
supported and may break as soon as the older version attempts to reindex.

This does not affect wallet forward or backward compatibility. There are no
known problems when downgrading from 0.11.x to 0.10.x.

Notable changes since 0.10.3
============================

BIP65 soft fork to enforce OP_CHECKLOCKTIMEVERIFY opcode
--------------------------------------------------------

This release includes several changes related to the [BIP65][] soft fork
which redefines the existing OP_NOP2 opcode as OP_CHECKLOCKTIMEVERIFY
(CLTV) so that a transaction output can be made unspendable until a
specified point in the future.

1. This release will only relay and mine transactions spending a CLTV
   output if they comply with the BIP65 rules as provided in code.

2. This release will produce version 4 blocks by default. Please see the
   *notice to miners* below.

3. Once 951 out of a sequence of 1,001 blocks on the local node's best block
   chain contain version 4 (or higher) blocks, this release will no
   longer accept new version 3 blocks and it will only accept version 4
   blocks if they comply with the BIP65 rules for CLTV.

For more information about the soft-forking change, please see
<https://github.com/bitcoin/bitcoin/pull/6351>

Graphs showing the progress towards block version 4 adoption may be
found at the URLs below:

- Block versions over the last 50,000 blocks as progress towards BIP65
  consensus enforcement: <http://bitcoin.sipa.be/ver-50k.png>

- Block versions over the last 2,000 blocks showing the days to the
  earliest possible BIP65 consensus-enforced block: <http://bitcoin.sipa.be/ver-2k.png>

**Notice to miners:** Bitcoin Core’s block templates are now for
version 4 blocks only, and any mining software relying on its
getblocktemplate must be updated in parallel to use libblkmaker either
version FIXME or any version from FIXME onward.

- If you are solo mining, this will affect you the moment you upgrade
  Bitcoin Core, which must be done prior to BIP65 achieving its 951/1001
  status.

- If you are mining with the stratum mining protocol: this does not
  affect you.

- If you are mining with the getblocktemplate protocol to a pool: this
  will affect you at the pool operator’s discretion, which must be no
  later than BIP65 achieving its 951/1001 status.

[BIP65]: https://github.com/bitcoin/bips/blob/master/bip-0065.mediawiki

Windows bug fix for corrupted UTXO database on unclean shutdowns
----------------------------------------------------------------

Several Windows users reported that they often need to reindex the
entire blockchain after an unclean shutdown of Bitcoin Core on Windows
(or an unclean shutdown of Windows itself). Although unclean shutdowns
remain unsafe, this release no longer relies on memory-mapped files for
the UTXO database, which significantly reduced the frequency of unclean
shutdowns leading to required reindexes during testing.

For more information, see: <https://github.com/bitcoin/bitcoin/pull/6917>

Other fixes for database corruption on Windows are expected in the
next major release.

0.10.4 Change log
>>>>>>> refs/remotes/origin/0.10
=================

Detailed release notes follow. This overview includes changes that affect
behavior, not code moves, refactors and string updates. For convenience in locating
the code changes and accompanying discussion, both the pull request and
git merge commit are mentioned.

<<<<<<< HEAD
### RPC and REST

Asm representations of scriptSig signatures now contain SIGHASH type decodes
----------------------------------------------------------------------------

The `asm` property of each scriptSig now contains the decoded signature hash
type for each signature that provides a valid defined hash type.

The following items contain assembly representations of scriptSig signatures
and are affected by this change:

- RPC `getrawtransaction`
- RPC `decoderawtransaction`
- REST `/rest/tx/` (JSON format)
- REST `/rest/block/` (JSON format when including extended tx details)
- `bitcoin-tx -json`

For example, the `scriptSig.asm` property of a transaction input that
previously showed an assembly representation of:

    304502207fa7a6d1e0ee81132a269ad84e68d695483745cde8b541e3bf630749894e342a022100c1f7ab20e13e22fb95281a870f3dcf38d782e53023ee313d741ad0cfbc0c509001

now shows as:

    304502207fa7a6d1e0ee81132a269ad84e68d695483745cde8b541e3bf630749894e342a022100c1f7ab20e13e22fb95281a870f3dcf38d782e53023ee313d741ad0cfbc0c5090[ALL]

Note that the output of the RPC `decodescript` did not change because it is
configured specifically to process scriptPubKey and not scriptSig scripts.

### Configuration and command-line options

### Block and transaction handling

### P2P protocol and network code

### Validation

### Build system

### Wallet

### GUI

### Tests

### Miscellaneous

- Removed bitrpc.py from contrib

Addition of ZMQ-based Notifications
==================================

Bitcoind can now (optionally) asynchronously notify clients through a
ZMQ-based PUB socket of the arrival of new transactions and blocks.
This feature requires installation of the ZMQ C API library 4.x and
configuring its use through the command line or configuration file.
Please see docs/zmq.md for details of operation.
=======
- #6953 `8b3311f` alias -h for --help
- #6953 `97546fc` Change URLs to https in debian/control
- #6953 `38671bf` Update debian/changelog and slight tweak to debian/control
- #6953 `256321e` Correct spelling mistakes in doc folder
- #6953 `eae0350` Clarification of unit test build instructions
- #6953 `90897ab` Update bluematt-key, the old one is long-since revoked
- #6953 `a2f2fb6` build: disable -Wself-assign
- #6953 `cf67d8b` Bugfix: Allow mining on top of old tip blocks for testnet (fixes testnet-in-a-box use case)
- #6953 `b3964e3` Drop "with minimal dependencies" from description
- #6953 `43c2789` Split bitcoin-tx into its own package
- #6953 `dfe0d4d` Include bitcoin-tx binary on Debian/Ubuntu
- #6953 `612efe8` [Qt] Raise debug window when requested
- #6953 `3ad96bd` Fix locking in GetTransaction
- #6953 `9c81005` Fix spelling of Qt
- #6946 `94b67e5` Update LevelDB
- #6706 `5dc72f8` CLTV: Add more tests to improve coverage
- #6706 `6a1343b` Add RPC tests for the CHECKLOCKTIMEVERIFY (BIP65) soft-fork
- #6706 `4137248` Add CHECKLOCKTIMEVERIFY (BIP65) soft-fork logic
- #6706 `0e01d0f` Enable CHECKLOCKTIMEVERIFY as a standard script verify flag
- #6706 `6d01325` Replace NOP2 with CHECKLOCKTIMEVERIFY (BIP65)
- #6706 `750d54f` Move LOCKTIME_THRESHOLD to src/script/script.h
- #6706 `6897468` Make CScriptNum() take nMaxNumSize as an argument
- #6867 `5297194` Set TCP_NODELAY on P2P sockets
- #6836 `fb818b6` Bring historical release notes up to date
- #6852 `0b3fd07` build: make sure OpenSSL heeds noexecstack

Credits
=======

Thanks to everyone who directly contributed to this release:

- Alex Morcos
- Daniel Cousens
- Diego Viola
- Eric Lombrozo
- Esteban Ordano
- Gregory Maxwell
- Luke Dashjr
- MarcoFalke
- Matt Corallo
- Micha
- Mitchell Cash
- Peter Todd
- Pieter Wuille
- Wladimir J. van der Laan
- Zak Wilcox

And those who contributed additional code review and/or security research.

As well as everyone that helped translating on [Transifex](https://www.transifex.com/projects/p/bitcoin/).
>>>>>>> refs/remotes/origin/0.10
