#!/usr/bin/env python3
"""End-to-end merged mining (AuxPoW) test on a private FlapX testnet node.

Acts like a merged mining pool: asks the node for a block (createauxblock),
builds a Litecoin-style parent block whose coinbase commits to it, grinds the
parent's scrypt hash below FlapX's target, and submits the proof
(submitauxblock). Then checks that bad proofs are rejected, that the blocks
show their AuxPoW in getblock, that a transaction in a merge-mined block can
be read back from disk, and (after restarting the node, done by the caller)
that the blocks are still there.

Usage: auxpow-test.py <rpc port> <rpc user> <rpc password> mine|verify
Only the Python standard library is used.
"""
import base64
import hashlib
import json
import multiprocessing
import os
import struct
import sys
import time
import urllib.request

PORT, USER, PASSWORD, MODE = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
BLOCKS_TO_MINE = 3
STATE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "auxpow-test-state.json")


def rpc(method, *params):
    req = urllib.request.Request(
        "http://127.0.0.1:%s/" % PORT,
        data=json.dumps({"method": method, "params": list(params), "id": 1}).encode(),
        headers={"Authorization": "Basic " + base64.b64encode(("%s:%s" % (USER, PASSWORD)).encode()).decode(),
                 "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            reply = json.loads(resp.read())
    except urllib.error.HTTPError as e:
        reply = json.loads(e.read())
    if reply.get("error"):
        raise RuntimeError("%s: %s" % (method, reply["error"]))
    return reply["result"]


def rpc_retry(method, *params, tries=30):
    """Retry while the node says it's still 'downloading blocks' (it does for
    10 s after each new block on a chain whose tip is old, like a fresh testnet)."""
    for _ in range(tries):
        try:
            return rpc(method, *params)
        except RuntimeError as e:
            if "downloading" not in str(e) and "-10" not in str(e):
                raise
            time.sleep(2)
    raise RuntimeError("%s: node stayed in initial block download" % method)


def dsha(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def scrypt(b):
    return hashlib.scrypt(b, salt=b, n=1024, r=1, p=1, dklen=32)


def varint(n):
    if n < 0xfd:
        return bytes([n])
    if n <= 0xffff:
        return b"\xfd" + struct.pack("<H", n)
    return b"\xfe" + struct.pack("<I", n)


def push(data):
    assert len(data) < 0x4c
    return bytes([len(data)]) + data


def parent_coinbase(aux_hash_hex, extra=b""):
    """Litecoin-format coinbase whose scriptSig carries the merged mining data:
    marker, root (the FlapX block hash, as displayed), tree size 1, nonce 0."""
    mm = b"\xfa\xbemm" + bytes.fromhex(aux_hash_hex) + struct.pack("<I", 1) + struct.pack("<I", 0)
    script_sig = push(os.urandom(4)) + push(mm) + extra
    tx = struct.pack("<i", 1)                                   # version
    tx += varint(1) + b"\x00" * 32 + struct.pack("<I", 0xffffffff)  # null prevout
    tx += varint(len(script_sig)) + script_sig + struct.pack("<I", 0xffffffff)
    tx += varint(1) + struct.pack("<q", 0) + varint(1) + b"\x51"   # one output, OP_TRUE
    tx += struct.pack("<I", 0)                                  # lock time
    return tx


def parent_header(merkle_root, version=1, nonce=0):
    return (struct.pack("<i", version) + os.urandom(32) + merkle_root
            + struct.pack("<III", int(time.time()), 0x1e0ffff0, nonce))


def _grind(args):
    header, target, start, step = args
    nonce = start
    while True:
        h = header[:76] + struct.pack("<I", nonce & 0xffffffff)
        if int.from_bytes(scrypt(h), "little") <= target:
            return h
        nonce += step


def grind(header, target):
    """Find a nonce so the parent header's scrypt hash meets FlapX's target."""
    n = max(1, os.cpu_count() or 1)
    with multiprocessing.Pool(n) as pool:
        for found in pool.imap_unordered(_grind, [(header, target, i, n) for i in range(n)]):
            pool.terminate()
            return found


def auxpow_hex(coinbase, header):
    return (coinbase + b"\x00" * 32        # coinbase tx, (unused) block hash
            + varint(0) + struct.pack("<i", 0)   # coinbase merkle branch (single tx), index 0
            + varint(0) + struct.pack("<i", 0)   # chain merkle branch (FlapX only), slot 0
            + header).hex()


def solve(block, version=1, extra=b""):
    target = int.from_bytes(bytes.fromhex(block["_target"]), "little")
    coinbase = parent_coinbase(block["hash"], extra)
    header = grind(parent_header(dsha(coinbase), version), target)
    return coinbase, header


def expect_rejected(name, block, aux_hex):
    try:
        ok = rpc("submitauxblock", block["hash"], aux_hex)
    except RuntimeError as e:
        ok = False
        print("  (%s)" % e)
    if ok:
        raise SystemExit("FAIL: %s was accepted" % name)
    print("ok: %s rejected" % name)


def mine():
    print("node:", rpc_retry("getinfo")["version"])
    address = rpc("getnewaddress")
    mined = []

    for i in range(BLOCKS_TO_MINE):
        block = rpc_retry("createauxblock", address)
        assert block["chainid"] == 0x0F1A, block
        height_before = rpc("getblockcount")

        if i == 0:
            # Negative tests on the first block, before solving it properly
            coinbase, header = solve(block)
            bad_root = header[:36] + bytes(32) + header[68:]                      # parent merkle root wrong
            expect_rejected("AuxPoW with a wrong parent merkle root", block, auxpow_hex(coinbase, bad_root))
            other_cb, other_hdr = solve({"hash": "00" * 32, "_target": block["_target"]})  # valid work, other block
            expect_rejected("AuxPoW committing to another block", block, auxpow_hex(other_cb, other_hdr))
            coinbase2, header2 = solve(block, version=(0x0F1A << 16) | 1)          # parent with our chain ID
            expect_rejected("AuxPoW whose parent has FlapX's chain ID", block, auxpow_hex(coinbase2, header2))
            weak = header[:76] + struct.pack("<I", (struct.unpack("<I", header[76:])[0] + 1))
            target = int.from_bytes(bytes.fromhex(block["_target"]), "little")
            if int.from_bytes(scrypt(weak), "little") > target:
                expect_rejected("AuxPoW whose parent misses the target", block, auxpow_hex(coinbase, weak))
            assert rpc("getblockcount") == height_before, "a bad AuxPoW changed the chain"

        coinbase, header = solve(block)
        if not rpc("submitauxblock", block["hash"], auxpow_hex(coinbase, header)):
            raise SystemExit("FAIL: valid AuxPoW rejected (see debug.log)")
        assert rpc("getblockcount") == height_before + 1
        print("ok: merge-mined block %d accepted: %s" % (height_before + 1, block["hash"]))
        mined.append(block["hash"])
        time.sleep(11)   # past the node's 10 s 'downloading blocks' window

    with open(STATE_FILE, "w") as f:
        json.dump(mined, f)
    check(mined)


def check(mined):
    for h in mined:
        b = rpc("getblock", h, True)
        assert b["version"] & 0x100 and b["version"] >> 16 == 0x0F1A, "version %08x" % b["version"]
        assert "auxpow" in b and b["auxpow"]["chainid"] == 0x0F1A, b
        assert b["proofhash"] == b["auxpow"]["parentpowhash"], "proof hash is not the parent's scrypt hash"
        # Reading a transaction of a merge-mined block back from disk checks
        # that its position on disk accounts for the AuxPoW
        txid = b["tx"][0]["txid"] if isinstance(b["tx"][0], dict) else b["tx"][0]
        tx = rpc("getrawtransaction", txid, 1)
        assert tx["txid"] == txid, "coinbase read back from disk is wrong"
    print("ok: %d merge-mined blocks have their AuxPoW, the parent's hash as proof, and readable transactions" % len(mined))


if __name__ == "__main__":
    if MODE == "mine":
        mine()
    else:
        with open(STATE_FILE) as f:
            mined = json.load(f)
        assert rpc_retry("getblockcount") >= BLOCKS_TO_MINE, "blocks missing after restart"
        check(mined)
        print("ok: still there after a restart")
    print("PASS")
