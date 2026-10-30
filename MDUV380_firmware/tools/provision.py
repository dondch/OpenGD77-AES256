#!/usr/bin/env python3
"""Remote provisioning over USB: pair a radio, queue a job on the master, read the status.
Needs an ENABLE_REMOTE_PROVISION build (the dev-prov or fleet profile). See PROVISIONING.md
and docs/FLEET_SETUP.md.

  # 1. pair (every radio of the fleet): the 256-bit pairing key and the master ID(s) a slave
  #    accepts. The key never leaves the radio again; the stored sequence counters are kept
  #    unless --reset-seq (only with a NEW key: it reopens the old traffic to replay).
  python3 provision.py pair --key <64hex> --master-id 1234567 [--master-id 2345678] [--own-seq N]
  python3 provision.py pair --gen-key --master-id 1234567      # prints the generated key once
  python3 provision.py pair --from-json private/fleet.json     # fleet.pairing_key and fleet.masters

  # 1b. the radio's own identity and the menu PIN, written into the codeplug over USB (the
  #     radio must be power-cycled afterwards; the PIN is 1..6 digits, "off" removes it)
  python3 provision.py identity --dmr-id 1234567 --callsign ALICE [--pin 1234]
  python3 provision.py identity --from-json private/fleet.json --radio radio2 [--pin 1234]

  # 2. what the radio holds (no key bytes): masters, stored/seen sequence numbers, counters,
  #    the master job state and the last STATUS it received
  python3 provision.py show

  # 3. a job for the master radio: items (same options as prov_packet.py), an implicit COMMIT,
  #    or a single STATUS / REBOOT; --wait polls until the job ends and prints the outcome
  python3 provision.py job --slave 7654321 --txn 7 --chan 5 "PMR 1" 446006250 446006250 \\
        --zone 2 "Fleet" 5,6 --setting txPowerLevel 3 --wait
  python3 provision.py job --slave 7654321 --status --wait
  python3 provision.py job --from-json private/fleet.json --radio radio2 --wait   # a whole radio
  python3 provision.py job --slave 7654321 --from-json job.json --wait            # see JOB_JSON

  python3 provision.py cancel        # abandon a running job
  python3 provision.py reload        # re-read the PROV block after a raw flash write
  python3 provision.py close-cps     # pop the CPS screen if an older tool left it up (no RF while it shows)

JOB_JSON (every key optional; a fleet file, FLEET_SETUP.md, is the same with "radios" and "fleet"):
  {"txn": 7,
   "channels": [{"index": 5, "name": "PMR 1", "rx_hz": 446006250, "tx_hz": 446006250, "mode": "digital",
                 "color_code": 1, "timeslot": 1, "contact": 1, "power_w": 0.5, "tot": 0, "bandwidth_25k": false,
                 "rx_only": false, "rx_tone": null, "tx_tone": null, "encrypt": 0}],
   "zones": [{"index": 2, "name": "Fleet", "channels": [5, 6]}],
   "vfos": [{"vfo": "A", "hex": "<112 hex chars>"}],
   "contacts": [{"index": 1, "name": "Fleet", "id": 1, "type": "group"},
                {"index": 2, "name": "Alice", "id": 1234567, "type": "private"}],
   "delete_contacts": [6, 7, 8],                            # slots a radio may still hold from before
   "message_presets": ["All well", "Need help"],            # all 10 slots are written, the rest cleared
   "settings": {"txPowerLevel": 3},                        # "per-channel" skips a field
   "home": {"zone": 0, "channel": 1, "idle_return_minutes": 5},
   "simple_mode": true,
   "identity": {"dmr_id": 1234567, "callsign": "ALICE"},   # sent last: the slave answers under its old ID
   "voice_keys": [{"slot": 1, "key": "<64 hex>"}],
   "reboot": false}
With --radio UNIT the identity and simple_mode come from that entry of "radios" (unit, dmr_id,
callsign, simple_mode) and the slave ID defaults to its dmr_id (--slave overrides it: the ID the
radio still has). A job above one USB command (2040 bytes) is split into transactions sent one
after the other, the identity in the last one.
"""
import argparse, json, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prov_crypto as pc
import prov_packet as pp

BLOCK_LEN = 96
USB_JOB_MAX = 2040            # one 0x9A command
JOB_HDR = 6                   # slaveId(4) txn(1) flags(1)

# the MD-UV380 "EEPROM" is the first 64 KB of the SPI flash (codeplug.c reads it through
# EEPROM_Read, which the hardware layer maps to SPI_Flash_read at the same address), so these
# are raw flash addresses for the CPS 'X' sector write
ADDR_CALLSIGN = 0x00E0        # 8 bytes, 0xFF padded
ADDR_DMRID    = 0x00E8        # 4 bytes BCD, byte-swapped
ADDR_PIN      = 0x7519        # [0] bit0 = PIN enabled, [3..5] = up to 6 BCD digits, 0xF ends

def build_block(key, masters, counters, own_seq):
    """The 96-byte PROV block (PROVISIONING.md §2.1)."""
    b = bytearray(BLOCK_LEN)
    b[0:4] = b"PROV"; b[4] = 1; b[5] = len(masters)
    b[8:40] = key
    for i, m in enumerate(masters[:4]):
        struct.pack_into("<II", b, 40 + 8 * i, m & 0xFFFFFF, counters.get(m, 0))
    struct.pack_into("<I", b, 72, own_seq)
    return bytes(b)

def load_json(path):
    return json.load(open(path))

def pick_radio(j, unit):
    for r in j.get("radios", []):
        if r.get("unit") == unit:
            return r
    sys.exit("no radio %r in %s (units: %s)" % (unit, "radios", [r.get("unit") for r in j.get("radios", [])]))

def key_bytes(hexstr, what):
    if not isinstance(hexstr, str) or hexstr.upper() == "GENERATE":
        sys.exit("%s is not set: run `provision.py pair --gen-key ...` once and store the printed key in the file" % what)
    k = bytes.fromhex(hexstr)
    if len(k) != 32 or k == bytes(32):
        sys.exit("%s must be 32 non-zero bytes" % what)
    return k

def items_from_json(j, a, radio=None):
    """The job items of a job or fleet JSON, in write order; identity last. Returns (items, single)."""
    items = []
    for c in j.get("channels", []):
        rec = pp.channel_record(c["name"], int(c["rx_hz"]), int(c["tx_hz"]), c.get("mode", "digital"),
                                c.get("color_code", 1), c.get("timeslot", 1), c.get("contact", 1), c.get("rx_group", 0),
                                c.get("power", 0), c.get("tot", 0), c.get("bandwidth_25k", False), c.get("rx_only", False),
                                c.get("rx_tone"), c.get("tx_tone"), c.get("squelch", 0), c.get("encrypt", 0),
                                power_w=c.get("power_w"))
        items.append(pp.item_channel(int(c["index"]), rec))
    for z in j.get("zones", []):
        items.append(pp.item_zone(int(z["index"]), pp.zone_record(z["name"], z["channels"])))
    for v in j.get("vfos", []):
        items.append(pp.item_vfo(0 if str(v["vfo"]).upper() == "A" else 1, bytes.fromhex(v["hex"])))
    for c in j.get("contacts", []):
        items.append(pp.item_contact(int(c["index"]), pp.contact_record(c["name"], int(c["id"]), c.get("type", "private"))))
    for idx in j.get("delete_contacts", []):
        items.append(pp.item_contact(int(idx), pp.contact_delete_record()))
    keys = j.get("voice_keys")
    if keys is None:
        keys = j.get("fleet", {}).get("voice_keys", [])
    for k in keys:
        items.append(pp.item_key(int(k["slot"]), key_bytes(k["key"], "voice key slot %s" % k["slot"])))
    if "message_presets" in j:
        items += pp.preset_items(j["message_presets"])
    for k, v in j.get("settings", {}).items():
        if isinstance(v, str) and not v.lstrip("-").isdigit():
            print("note: setting %s = %r is not a number, skipped (channels carry their own power)" % (k, v))
            continue
        items.append(pp.item_setting(k, int(v)))
    home = j.get("home")
    if home:
        items.append(pp.item_setting("simpleHomeZone", int(home.get("zone", 0))))
        items.append(pp.item_setting("simpleHomeChannel", int(home.get("channel", 0))))
        items.append(pp.item_setting("simpleIdleMinutes", int(home.get("idle_return_minutes", 0))))
    simple = radio.get("simple_mode") if radio is not None else j.get("simple_mode")
    if simple is not None:
        items.append(pp.item_setting("simpleMode", 1 if simple else 0))
    ident = {"dmr_id": radio["dmr_id"], "callsign": radio["callsign"]} if radio is not None else j.get("identity")
    if ident:
        items.append(pp.item_identity(int(ident["dmr_id"]), ident["callsign"]))
    single = [pp.item_reboot()] if j.get("reboot") else []
    if j.get("txn") is not None and a is not None:
        a.txn = int(j["txn"])
    return items, single

def split_jobs(items):
    """Pack the items into as few USB commands as fit, keeping their order (the identity stays
    last because it is last in the list)."""
    jobs, cur, size = [], [], JOB_HDR
    for it in items:
        n = 3 + len(it) - 1
        if cur and (size + n > USB_JOB_MAX):
            jobs.append(cur); cur, size = [], JOB_HDR
        cur.append(it); size += n
    if cur:
        jobs.append(cur)
    return jobs

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

def send_job(ser, slave, txn, items, single, wait, dump):
    if single:
        job = pp.job_bytes(slave, txn, single[:1], commit=False, single=True)
    else:
        job = pp.job_bytes(slave, txn, items, commit=True)
    print("job: slave %d txn %d, %d item(s), %d bytes" % (slave, txn, len(items) + len(single), len(job)))
    for it in (items or single):
        print("  " + pp.describe_body(it))
    if items:
        print("  " + pp.describe_body(pp.commit_item(items)) + " (implicit)")
    if len(job) > USB_JOB_MAX:
        sys.exit("job too large for one USB command (%d > %d bytes)" % (len(job), USB_JOB_MAX))
    if dump:
        open(dump, "wb").write(job); print("written to", dump); return True
    r = pp.cmd(ser, bytes([0x9A, len(job) >> 8, len(job) & 0xFF]) + job, 3, 0.3)
    if len(r) < 3 or r[0] != ord("C") or r[1] != 0x9A:
        sys.exit("no reply to the job command (is this an ENABLE_REMOTE_PROVISION build?): %r" % r)
    res = struct.unpack("b", r[2:3])[0]
    why = {0: "queued", -1: "busy (a job runs, or the radio is staging as a slave)", -2: "not paired", -3: "arena held by another feature",
           -4: "malformed", -5: "too big", -6: "sequence exhausted: re-key",
           -7: "the CPS screen is up on the radio (no RF in that state): run provision.py close-cps"}.get(res, res)
    print("master:", why)
    if res != 0:
        sys.exit(1)
    if not wait:
        return True
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
            return j.get("outcome") == "done"
    sys.exit("timed out waiting for the job")

def bcd32(v):
    out = 0
    for i in range(8):
        out |= (v % 10) << (4 * i); v //= 10
    return out

def write_identity(ser, dmr_id, callsign, pin):
    """DMR ID, callsign and PIN into the codeplug through the CPS sector write (aes_key_store.py
    does the same for the key block). None leaves a field alone; pin "off" disables the PIN."""
    import aes_key_store as ks
    if dmr_id is not None:
        if not (1 <= int(dmr_id) <= 0xFFFFFF):
            sys.exit("the DMR ID must be 1..16777215")
        ks.flash_write_block(ser, ADDR_DMRID, struct.pack(">I", bcd32(int(dmr_id))))   # byte-swapped LE = big-endian bytes
        print("DMR ID %d written" % int(dmr_id))
    if callsign is not None:
        ks.flash_write_block(ser, ADDR_CALLSIGN, pp.callsign_bytes(callsign))
        print("callsign %r written" % callsign[:8])
    if pin is not None:
        cur = ks.flash_read(ser, ADDR_PIN, 6)
        b = bytearray(cur)
        if str(pin).lower() == "off":
            b[0] &= 0xFE
            b[3:6] = b"\xff\xff\xff"
        else:
            digits = str(pin)
            if not digits.isdigit() or not (1 <= len(digits) <= 6):
                sys.exit("the PIN is 1..6 digits")
            nib = digits + "f" * (6 - len(digits))
            b[0] |= 0x01
            b[3:6] = bytes.fromhex(nib)
        ks.flash_write_block(ser, ADDR_PIN, bytes(b))
        print("PIN %s" % ("disabled" if str(pin).lower() == "off" else "set (%d digits)" % len(str(pin))))
    print("power-cycle the radio: the ID and callsign are read at boot")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pair")
    p.add_argument("--key", help="pairing key, 64 hex chars")
    p.add_argument("--gen-key", action="store_true", help="generate a random key (printed once)")
    p.add_argument("--from-json", help="fleet file: fleet.pairing_key and fleet.masters")
    p.add_argument("--master-id", type=int, action="append", default=[], help="a master this radio accepts (up to 4)")
    p.add_argument("--own-seq", type=int, help="set this radio's own (master) counter reservation")
    p.add_argument("--reset-seq", action="store_true", help="zero every counter (only with a new key)")
    p.add_argument("--seq", nargs=2, action="append", metavar=("MASTER", "LASTSEQ"), default=[], help="set the stored seq for a master")
    p = sub.add_parser("identity", help="write the radio's own DMR ID, callsign and menu PIN over USB")
    p.add_argument("--dmr-id", type=int)
    p.add_argument("--callsign")
    p.add_argument("--pin", help="1..6 digits, or off")
    p.add_argument("--from-json", help="fleet file; with --radio UNIT its dmr_id and callsign")
    p.add_argument("--radio", help="the unit in the fleet file's radios list")
    sub.add_parser("show")
    sub.add_parser("reload")
    sub.add_parser("cancel")
    sub.add_parser("close-cps", help="pop the CPS screen an older tool left up (the radio transmits nothing while it shows)")
    p = sub.add_parser("job")
    p.add_argument("--slave", type=int, help="the slave's current DMR ID (default: the --radio entry's dmr_id)")
    p.add_argument("--txn", type=int, default=1)
    p.add_argument("--from-json")
    p.add_argument("--radio", help="the unit in the fleet file's radios list: its identity and simple_mode")
    pp.add_item_args(p)
    p.add_argument("--wait", action="store_true", help="poll until the job ends")
    p.add_argument("--dump", help="write the job bytes to this file instead of sending (file.N when split)")
    a = ap.parse_args()

    if a.cmd == "job":
        items, single = pp.items_from_args(a)
        radio = None
        if a.from_json:
            j = load_json(a.from_json)
            radio = pick_radio(j, a.radio) if a.radio else None
            ji, js = items_from_json(j, a, radio)
            items += ji; single += js
        elif a.radio:
            sys.exit("--radio needs --from-json")
        if a.slave is None:
            if radio is None:
                sys.exit("pass --slave, or --radio with --from-json")
            a.slave = int(radio["dmr_id"])
        if items and single:
            sys.exit("a job is either staged items (+COMMIT) or one STATUS/REBOOT, not both")
        if not items and not single:
            sys.exit("nothing to send")
        jobs = split_jobs(items) if items else [[]]
        if len(jobs) > 1:
            print("%d items do not fit one USB command: %d transactions (txn %d..%d), sent one after the other" % (
                len(items), len(jobs), a.txn, a.txn + len(jobs) - 1))
            a.wait = True
        if a.dump:
            for i, chunk in enumerate(jobs):
                send_job(None, a.slave, a.txn + i, chunk, single if not items else [], False, a.dump + (".%d" % i if len(jobs) > 1 else ""))
            return
        with pp.open_radio(a.port) as ser:
            for i, chunk in enumerate(jobs):
                ok = send_job(ser, a.slave, a.txn + i, chunk, single if not items else [], a.wait, None)
                if not ok:
                    sys.exit(2)
                if i + 1 < len(jobs):
                    time.sleep(1.0)
        return

    if a.cmd == "identity":
        dmr_id, callsign = a.dmr_id, a.callsign
        if a.from_json:
            if not a.radio:
                sys.exit("--from-json needs --radio UNIT")
            r = pick_radio(load_json(a.from_json), a.radio)
            dmr_id = int(r["dmr_id"]) if dmr_id is None else dmr_id
            callsign = r["callsign"] if callsign is None else callsign
        if dmr_id is None and callsign is None and a.pin is None:
            sys.exit("nothing to write: --dmr-id, --callsign, --pin or --from-json --radio")
        with pp.open_radio(a.port) as ser:
            write_identity(ser, dmr_id, callsign, a.pin)
        return

    with pp.open_radio(a.port) as ser:
        if a.cmd == "show":
            print_show(pp.decode_diag(pp.read_diag(ser)))
        elif a.cmd == "reload":
            pp.cmd(ser, bytes([0x9B, 1]), 3, 0.3); print_show(pp.decode_diag(pp.read_diag(ser)))
        elif a.cmd == "cancel":
            pp.cmd(ser, bytes([0x9B, 4]), 3, 0.3); print_show(pp.decode_diag(pp.read_diag(ser)))
        elif a.cmd == "close-cps":
            pp.close_cps(ser); print("CPS screen closed")
        elif a.cmd == "pair":
            masters = list(a.master_id)
            if a.from_json:
                f = load_json(a.from_json).get("fleet", {})
                key = key_bytes(f.get("pairing_key"), "fleet.pairing_key")
                masters = masters or [int(m) for m in f.get("masters", [])]
            elif a.gen_key:
                key = os.urandom(32)
                print("generated pairing key (keep it, it is never shown again): %s" % key.hex())
            elif a.key:
                key = bytes.fromhex(a.key)
            else:
                sys.exit("pass --key, --gen-key or --from-json")
            if len(key) != 32 or key == bytes(32):
                sys.exit("the key must be 32 non-zero bytes")
            d = pp.decode_diag(pp.read_diag(ser))
            counters = {} if a.reset_seq else {m["id"]: m["lastSeq"] for m in d.get("masters", [])}
            own = 0 if a.reset_seq else d.get("ownSeqReserved", 0)
            for m, s in a.seq:
                counters[int(m)] = int(s)
            if a.own_seq is not None:
                own = a.own_seq
            masters = masters or [m["id"] for m in d.get("masters", [])]
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
