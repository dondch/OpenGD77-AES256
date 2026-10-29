#!/usr/bin/env python3
"""Remote provisioning over USB: pair a radio, queue a job on the master, read the status.
Needs an ENABLE_REMOTE_PROVISION build (the dev-prov profile). See PROVISIONING.md.

  # 1. pair (every radio of the fleet): the 256-bit pairing key and the master ID(s) a slave
  #    accepts. The key never leaves the radio again; the stored sequence counters are kept
  #    unless --reset-seq (only with a NEW key: it reopens the old traffic to replay).
  python3 provision.py pair --key <64hex> --master-id 1234567 [--master-id 2345678] [--own-seq N]
  python3 provision.py pair --gen-key --master-id 1234567      # prints the generated key once

  # 2. what the radio holds (no key bytes): masters, stored/seen sequence numbers, counters,
  #    the master job state and the last STATUS it received
  python3 provision.py show

  # 3. a job for the master radio: items (same options as prov_packet.py), an implicit COMMIT,
  #    or a single STATUS / REBOOT; --wait polls until the job ends and prints the outcome
  python3 provision.py job --slave 7654321 --txn 7 --chan 5 "PMR 1" 446006250 446006250 \\
        --zone 2 "Fleet" 5,6 --setting txPowerLevel 3 --wait
  python3 provision.py job --slave 7654321 --status --wait
  python3 provision.py job --slave 7654321 --from-json fleet.json --wait   # see JOB_JSON below

  python3 provision.py cancel        # abandon a running job
  python3 provision.py reload        # re-read the PROV block after a raw flash write

JOB_JSON = {"txn": 7, "channels": [{"index": 5, "name": "PMR 1", "rx_hz": 446006250, "tx_hz": 446006250,
            "mode": "digital", "color_code": 1, "timeslot": 1, "contact": 1, "power": 0, "tot": 0,
            "bandwidth_25k": false, "rx_only": false, "rx_tone": null, "tx_tone": null, "encrypt": 0}],
            "zones": [{"index": 2, "name": "Fleet", "channels": [5, 6]}],
            "vfos": [{"vfo": "A", "hex": "<112 hex chars>"}],
            "settings": {"txPowerLevel": 3}, "voice_keys": [{"slot": 1, "key": "<64 hex>"}],
            "reboot": false}
"""
import argparse, json, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prov_crypto as pc
import prov_packet as pp

BLOCK_LEN = 96

def build_block(key, masters, counters, own_seq):
    """The 96-byte PROV block (PROVISIONING.md §2.1)."""
    b = bytearray(BLOCK_LEN)
    b[0:4] = b"PROV"; b[4] = 1; b[5] = len(masters)
    b[8:40] = key
    for i, m in enumerate(masters[:4]):
        struct.pack_into("<II", b, 40 + 8 * i, m & 0xFFFFFF, counters.get(m, 0))
    struct.pack_into("<I", b, 72, own_seq)
    return bytes(b)

def items_from_json(path, a):
    j = json.load(open(path))
    items = []
    for c in j.get("channels", []):
        rec = pp.channel_record(c["name"], int(c["rx_hz"]), int(c["tx_hz"]), c.get("mode", "digital"),
                                c.get("color_code", 1), c.get("timeslot", 1), c.get("contact", 1), c.get("rx_group", 0),
                                c.get("power", 0), c.get("tot", 0), c.get("bandwidth_25k", False), c.get("rx_only", False),
                                c.get("rx_tone"), c.get("tx_tone"), c.get("squelch", 0), c.get("encrypt", 0))
        items.append(pp.item_channel(int(c["index"]), rec))
    for z in j.get("zones", []):
        items.append(pp.item_zone(int(z["index"]), pp.zone_record(z["name"], z["channels"])))
    for v in j.get("vfos", []):
        items.append(pp.item_vfo(0 if str(v["vfo"]).upper() == "A" else 1, bytes.fromhex(v["hex"])))
    for k, v in j.get("settings", {}).items():
        items.append(pp.item_setting(k, int(v)))
    for k in j.get("voice_keys", []):
        items.append(pp.item_key(int(k["slot"]), bytes.fromhex(k["key"])))
    single = [pp.item_reboot()] if j.get("reboot") else []
    if j.get("txn") is not None:
        a.txn = int(j["txn"])
    return items, single

def print_show(d):
    if "raw" in d:
        print("unexpected diag reply:", d["raw"]); return
    print("paired: %s   masters: %s" % ("yes" if d["paired"] else "NO (no PROV block / no key)",
          ", ".join("%d (lastSeq %d, seen %d)" % (m["id"], m["lastSeq"], m["lastSeen"]) for m in d["masters"]) or "none"))
    print("own counter: next %d, reserved %d" % (d["ownSeqNext"], d["ownSeqReserved"]))
    print("slave: txnActive %d txn %d staged %d (%d B) stageErr %d commits %d lastCommitSeq %d codeplugCrc %08x" % (
        d["txnActive"], d["txn"], d["stagedCount"], d["stagedUsed"], d["stageErr"], d["commits"], d["lastCommitSeq"], d["lastCodeplugCrc"]))
    print("rejected: failedMacs %d seq %d addr %d frame %d   ackInhibit %d replyPending %d" % (
        d["failedMacs"], d["rejSeq"], d["rejAddr"], d["rejFrame"], d["ackInhibit"], d["replyPending"]))
    if "job" in d:
        j = d["job"]
        print("job: %s item %d/%d tries %d outcome %s%s seq %d ack %s" % (
            j["phase"], j["item"], j["count"], j["tries"], j["outcome"],
            (" (%s)" % j["failReason"]) if j["outcome"] == "failed" else "", j["seq"], j["ack"]))
        if "status" in j:
            print("last STATUS:", j["status"])

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pair")
    p.add_argument("--key", help="pairing key, 64 hex chars")
    p.add_argument("--gen-key", action="store_true", help="generate a random key (printed once)")
    p.add_argument("--master-id", type=int, action="append", default=[], help="a master this radio accepts (up to 4)")
    p.add_argument("--own-seq", type=int, help="set this radio's own (master) counter reservation")
    p.add_argument("--reset-seq", action="store_true", help="zero every counter (only with a new key)")
    p.add_argument("--seq", nargs=2, action="append", metavar=("MASTER", "LASTSEQ"), default=[], help="set the stored seq for a master")
    sub.add_parser("show")
    sub.add_parser("reload")
    sub.add_parser("cancel")
    p = sub.add_parser("job")
    p.add_argument("--slave", type=int, required=True)
    p.add_argument("--txn", type=int, default=1)
    p.add_argument("--from-json")
    pp.add_item_args(p)
    p.add_argument("--wait", action="store_true", help="poll until the job ends")
    p.add_argument("--dump", help="write the job bytes to this file instead of sending")
    a = ap.parse_args()

    if a.cmd == "job":
        items, single = pp.items_from_args(a)
        if a.from_json:
            ji, js = items_from_json(a.from_json, a)
            items += ji; single += js
        if items and single:
            sys.exit("a job is either staged items (+COMMIT) or one STATUS/REBOOT, not both")
        if not items and not single:
            sys.exit("nothing to send")
        if single:
            job = pp.job_bytes(a.slave, a.txn, single[:1], commit=False, single=True)
        else:
            job = pp.job_bytes(a.slave, a.txn, items, commit=True)
        print("job: slave %d txn %d, %d item(s), %d bytes" % (a.slave, a.txn, len(items) + len(single), len(job)))
        for it in (items or single):
            print("  " + pp.describe_body(it))
        if items:
            print("  " + pp.describe_body(pp.commit_item(items)) + " (implicit)")
        if len(job) > 2040:
            sys.exit("job too large for one USB command (%d > 2040 bytes): split it into transactions" % len(job))
        if a.dump:
            open(a.dump, "wb").write(job); print("written to", a.dump); return
        with pp.open_radio(a.port) as ser:
            r = pp.cmd(ser, bytes([0x9A, len(job) >> 8, len(job) & 0xFF]) + job, 3, 0.3)
            if len(r) < 3 or r[0] != ord("C") or r[1] != 0x9A:
                sys.exit("no reply to the job command (is this an ENABLE_REMOTE_PROVISION build?): %r" % r)
            res = struct.unpack("b", r[2:3])[0]
            why = {0: "queued", -1: "busy (a job runs, or the radio is staging as a slave)", -2: "not paired", -3: "arena held by another feature",
                   -4: "malformed", -5: "too big", -6: "sequence exhausted: re-key"}.get(res, res)
            print("master:", why)
            if res != 0:
                sys.exit(1)
            if a.wait:
                t0 = time.time()
                while time.time() - t0 < 180:
                    time.sleep(1.0)
                    d = pp.decode_diag(pp.read_diag(ser))
                    j = d.get("job", {})
                    print("  %s item %d/%d tries %d seq %d" % (j.get("phase"), j.get("item", 0), j.get("count", 0), j.get("tries", 0), j.get("seq", 0)))
                    if j.get("phase") == "idle":
                        print("outcome:", j.get("outcome"), ("(%s)" % j["failReason"]) if j.get("outcome") == "failed" else "")
                        if "status" in j:
                            print("STATUS:", j["status"])
                        sys.exit(0 if j.get("outcome") == "done" else 2)
                sys.exit("timed out waiting for the job")
        return

    with pp.open_radio(a.port) as ser:
        if a.cmd == "show":
            print_show(pp.decode_diag(pp.read_diag(ser)))
        elif a.cmd == "reload":
            pp.cmd(ser, bytes([0x9B, 1]), 3, 0.3); print_show(pp.decode_diag(pp.read_diag(ser)))
        elif a.cmd == "cancel":
            pp.cmd(ser, bytes([0x9B, 4]), 3, 0.3); print_show(pp.decode_diag(pp.read_diag(ser)))
        elif a.cmd == "pair":
            if a.gen_key:
                key = os.urandom(32)
                print("generated pairing key (keep it, it is never shown again): %s" % key.hex())
            elif a.key:
                key = bytes.fromhex(a.key)
            else:
                sys.exit("pass --key or --gen-key")
            if len(key) != 32 or key == bytes(32):
                sys.exit("the key must be 32 non-zero bytes")
            d = pp.decode_diag(pp.read_diag(ser))
            counters = {} if a.reset_seq else {m["id"]: m["lastSeq"] for m in d.get("masters", [])}
            own = 0 if a.reset_seq else d.get("ownSeqReserved", 0)
            for m, s in a.seq:
                counters[int(m)] = int(s)
            if a.own_seq is not None:
                own = a.own_seq
            masters = a.master_id or [m["id"] for m in d.get("masters", [])]
            if len(masters) > 4:
                sys.exit("at most 4 master IDs")
            blk = build_block(key, masters, counters, own)
            r = pp.cmd(ser, bytes([0x9B, 5]) + blk, 3, 1.5)
            if len(r) < 3 or r[0] != ord("C") or r[2] != 1:
                sys.exit("block write failed: %r" % r)
            print("paired: masters %s, counters %s, own reservation %d" % (masters, counters, own))
            print_show(pp.decode_diag(pp.read_diag(ser)))

if __name__ == "__main__":
    main()
