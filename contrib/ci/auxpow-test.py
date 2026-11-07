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


def expected_index(nonce, chain_id, height):
    """FlapX's slot in the merged mining tree (same formula as the node)"""
    r = nonce & 0xffffffff
    r = (r * 1103515245 + 12345) & 0xffffffff
    r = (r + chain_id) & 0xffffffff
    r = (r * 1103515245 + 12345) & 0xffffffff
    return r % (1 << height)


def mm_tree(aux_hash_hex, two_chains, wrong_slot=False):
    """Merged mining tree: FlapX alone (size 1), or FlapX plus another chain
    (size 2, like a pool merge-mining DOGE and FLAPX). Returns the root as
    written in the coinbase, size, nonce, FlapX's slot and its branch."""
    ours = bytes.fromhex(aux_hash_hex)[::-1]          # internal byte order
    if not two_chains:
        return ours[::-1], 1, 0, 0, []
    nonce = struct.unpack("<I", os.urandom(4))[0]
    slot = expected_index(nonce, 0x0F1A, 1)
    if wrong_slot:
        slot = 1 - slot                                 # consistent tree, but not FlapX's slot
    other = os.urandom(32)                              # the other chain's block
    root = dsha(ours + other) if slot == 0 else dsha(other + ours)
    return root[::-1], 2, nonce, slot, [other]


def parent_coinbase(root_display, size=1, nonce=0, extra=b""):
    """Litecoin-format coinbase whose scriptSig carries the merged mining data:
    marker, root (as displayed), tree size, nonce."""
    mm = b"\xfa\xbemm" + root_display + struct.pack("<I", size) + struct.pack("<I", nonce)
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


def auxpow_hex(coinbase, header, slot=0, branch=()):
    return (coinbase + b"\x00" * 32        # coinbase tx, (unused) block hash
            + varint(0) + struct.pack("<i", 0)   # coinbase merkle branch (single tx), index 0
            + varint(len(branch)) + b"".join(branch) + struct.pack("<i", slot)  # chain branch, slot
            + header).hex()


def solve(block, version=1, two_chains=False, merkle_root=None, wrong_slot=False):
    """Returns the AuxPoW (hex) for a parent block that meets the target"""
    target = int.from_bytes(bytes.fromhex(block["_target"]), "little")
    root, size, nonce, slot, branch = mm_tree(block["hash"], two_chains, wrong_slot)
    coinbase = parent_coinbase(root, size, nonce)
    header = grind(parent_header(merkle_root or dsha(coinbase), version), target)
    return coinbase, header, slot, branch


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
            # Negative tests on the first block, before solving it properly.
            # Each bad parent still meets the target, so the AuxPoW check
            # itself has to catch it.
            cb, hdr, _, _ = solve(block, merkle_root=bytes(32))                     # coinbase not in parent
            expect_rejected("AuxPoW whose coinbase isn't in the parent block", block, auxpow_hex(cb, hdr))
            cb, hdr, _, _ = solve({"hash": "00" * 32, "_target": block["_target"]})  # commits to another block
            expect_rejected("AuxPoW committing to another block", block, auxpow_hex(cb, hdr))
            cb, hdr, _, _ = solve(block, version=(0x0F1A << 16) | 1)                # parent has our chain ID
            expect_rejected("AuxPoW whose parent has FlapX's chain ID", block, auxpow_hex(cb, hdr))
            cb, hdr, slot, branch = solve(block, two_chains=True, wrong_slot=True)  # FlapX in the other slot
            expect_rejected("AuxPoW with FlapX in the wrong tree slot", block, auxpow_hex(cb, hdr, slot, branch))
            # Parent that misses the target (otherwise valid)
            target = int.from_bytes(bytes.fromhex(block["_target"]), "little")
            cb, hdr, _, _ = solve(block)
            for n in range(1, 1000):
                weak = hdr[:76] + struct.pack("<I", (struct.unpack("<I", hdr[76:])[0] + n) & 0xffffffff)
                if int.from_bytes(scrypt(weak), "little") > target:
                    break
            expect_rejected("AuxPoW whose parent misses the target", block, auxpow_hex(cb, weak))
            assert rpc("getblockcount") == height_before, "a bad AuxPoW changed the chain"

        # Block 2 uses a two-chain tree (FlapX plus another chain), as a pool
        # merge-mining DOGE and FLAPX would
        coinbase, header, slot, branch = solve(block, two_chains=(i == 1))
        if not rpc("submitauxblock", block["hash"], auxpow_hex(coinbase, header, slot, branch)):
            raise SystemExit("FAIL: valid AuxPoW rejected (see debug.log)")
        assert rpc("getblockcount") == height_before + 1
        print("ok: merge-mined block %d accepted%s: %s" % (height_before + 1, " (two-chain tree)" if i == 1 else "", block["hash"]))
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
