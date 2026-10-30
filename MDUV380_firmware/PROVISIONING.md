# Secure remote provisioning (ENABLE_REMOTE_PROVISION)

A **master** radio running this firmware pushes configuration over the air to **slave** radios
running this firmware. Nothing else on the channel can read, forge, replay or redirect those
writes. This note is the design: PDU layout, keys, nonce and sequence rules, the transaction state
machine, the flash write path, the RAM budget, the threat model and the test plan. It is kept
current with the code (`functions/dmr_prov.c`, `functions/dmr_prov_apply.c`, `crypto/dmr_cmac.c`).

Build: `make ENABLE_REMOTE_PROVISION=1 ENABLE_DMR_DATA=1`. The flag needs `ENABLE_DMR_DATA`
(the DMR data path it rides on); the Makefile errors out otherwise. AES-256 is always compiled in
on this tree (the `ENABLE_AES` opt-in was dropped in PR 6), so the only dependency is the data
path. A plain `make` is unchanged.

## 1. What already exists and what is reused

| Piece | Where | Used for |
| --- | --- | --- |
| Burst queue TX (CSBK preamble, data header, rate-1/2 blocks, keyed data call) | `functions/dmr_data.c`, `dmrSmsSend()` in `dmr_sms.c` | every provisioning PDU and the STATUS reply |
| ISR reassembler (unconfirmed and confirmed delivery, rate 1/2 and 3/4, CRC32-gated completion, retransmission merge) | `dmrSmsRxBurst()` | receiving provisioning PDUs unchanged |
| Link-layer acknowledgement (ETSI Response PDU with N(S), sent in the next timeslot by the HR-C6000 ISR) | `dmrSmsAckTake()` / `hrc6000DataAckStart()` | the per-PDU ACK the master waits for |
| Main-loop decoder tick | `dmrSmsRxTick()` | dispatches SAP 9 PDUs without an ENC header to `dmrProvRxPdu()` |
| AES-256 block cipher | `crypto/dmr_aes.c` (`aes256_ecb_encrypt`) | CMAC, KDF and CTR are built on it |
| Key store for voice keys | `dmrAesStoreKey()` (stock TYT key table) | the STORE_KEY operation writes through it |
| Custom-data blocks in SPI flash | `codeplugGet/SetOpenGD77CustomData()` | the new `PROV` block (pairing key, master list, sequence counters) |
| Feature arena (11,904 B of SRAM shared by features that are never live together) | `functions/featureArena.c` | the slave's staging image and the master's job |
| USB dev commands, bracketed flash writes | `usb/usb_com.c` (`TASK_UNLOCK_WRITE`) | key loading, job queueing, status polling, burst replay |

Nothing provisioning-related is compiled into a build without the flag; `dmr_sms.c` gains four
small `#if defined(ENABLE_REMOTE_PROVISION)` hooks (SAP capture, dispatch, Response-PDU capture,
ACK gating).

## 2. Keys

### 2.1 Pairing key

A 256-bit **pairing key** per fleet (one key shared by the master and all its slaves; a per-slave
key is possible by loading a different key into the master before each slave, see §9). It is
entered on each radio over USB (`tools/provision.py --pair-key ... --master-id ...`), never over
the air, and lives in a new custom-data block:

```
CODEPLUG_CUSTOM_DATA_TYPE_PROVISION (= 9), 96 bytes, little-endian:
  0   "PROV"            magic
  4   u8   version      1
  5   u8   nMasters     0..4
  6   u16  reserved     0
  8   u8[32] pairingKey
 40   4 x { u32 masterId; u32 lastSeq; }   masters this radio accepts, and the highest sequence
                                           number committed from each (slave role)
 72   u32  ownSeqReserved   the next sequence number this radio may use as a master is >= this
                            value (master role, see §4)
 76   u8[20] reserved   0
```

The voice path only ever reads the stock TYT key table (slots 1..15); it cannot select the
pairing key, which is in a different structure altogether. The pairing key is never printed,
dumped or sent anywhere by the firmware; `--show` in the tool prints everything but the key.

### 2.2 Derived keys (SP 800-108 counter mode, CMAC-AES-256 as the PRF)

```
K_enc = KDF(K_pair, Label="enc")      K_mac = KDF(K_pair, Label="mac")
KDF(K, Label) = CMAC(K, 0x01 || Label || 0x00 || 0x01 0x00) || CMAC(K, 0x02 || Label || 0x00 || 0x01 0x00)
```
`i` is an 8-bit counter, `Label` is the ASCII bytes with no terminator, `Context` is empty, `L`
is the 16-bit big-endian output length in bits (256 = `01 00`). Two 16-byte CMAC outputs make
one 32-byte AES-256 key. The reference is `tools/prov_crypto.py` (pure Python, also generates the
vectors in `tests/test_prov_crypto.c`); CMAC itself is tested against the RFC 4493 vectors
(AES-128) and the NIST SP 800-38B AES-256 vectors.

The derived keys are computed once at boot and after every re-key (`dmrProvInit`), and held in
CCM RAM (64 bytes).

## 3. PDU layout

### 3.1 On the air

A provisioning PDU is an ETSI **confirmed-delivery individual data PDU** with **SAP 9
(proprietary packet data) and no proprietary header**: CSBK preamble, one data header, then
rate-1/2 confirmed blocks (2-byte DBSN/CRC-9 field + 10 payload bytes each). A stock TYT drops
SAP 9 without an extended header (observed on the bench for SMS), the fork's SMS decoder sees
SAP 9 without an ENC header and hands the PDU to the provisioning module, and an encrypted SMS
(SAP 9 **with** the Motorola ENC header) still goes to the SMS decoder. So the two services share
the reassembler and ignore each other's PDUs.

```
Data header (12 bytes, CRC mask 0xCCCC):
  [0] 0x43           individual, A = response requested, DPF 3 (confirmed)
  [1] 0x90 | POC     SAP 9, padding octet count (low nibble; bits 4..5 of [0] carry POC 16..47)
  [2..4] destination (slave ID, or the master ID for the reply)
  [5..7] source      (master ID, or the slave ID for the reply)
  [8] 0x80 | N       full message, N blocks to follow
  [9] N(S) << 4      S = 0, N(S) = seq & 7, FSN = 0
Blocks: DBSN(7) | CRC-9(9) | 10 payload bytes.  PDU = frame || zero padding || CRC32 (DMR).
```
The CRC-9 follows ETSI TS 102 361-1 B.3.11 in the form DSD-FME checks it (`dmr_dburst.c`,
`ComputeCrc9Bit`): the register (polynomial x^9+x^6+x^4+x^3+1, initial 0) runs over the 80 data bits
then the 7 DBSN bits, MSB first; DSD-FME compares the field XOR the rate-1/2 mask 0x0F0 with the
inverted register, so the field on the air is the register XOR 0x10F (`dmrProvCrc9()` and
`prov_packet.crc9()` agree byte for byte). Confirmed on the air 2026-10-30: DSD-FME checks every
rate-1/2 confirmed block and flagged none of the 72 blocks of three captures (it does flag CRC
errors elsewhere in the same captures), and reassembled the PDUs. Our own receiver still ignores
the CRC-9 (the HR-C6000 burst CRC already gates the block).

### 3.2 The provisioning frame (the PDU payload)

```
offset len  field                         protection
 0      1   0x50  'P' magic               authenticated
 1      1   version = 1                   authenticated
 2      1   txn   transaction id          authenticated
 3      1   flags bit0 = reply (dir)      authenticated
 4      4   seq   32-bit big-endian       authenticated
 8      2   plen  body length, BE         authenticated
10      plen  body, AES-256-CTR under K_enc    encrypted + authenticated
10+plen 16  tag = AES-CMAC(K_mac, A || header || body_ct)  (full 128 bits)
```
`A = masterId(4 BE) || slaveId(4 BE)` taken **from the data header** the PDU arrived in (for a
request: src = master, dst = slave; for a reply the same two values, master first). The MAC
therefore binds the frame to the (master, slave) pair as addressed on the air: a PDU captured
and re-sent to another slave, or from another source, fails. `seq`, `txn`, the direction flag
and the body (whose first byte is the operation) are inside the MAC, so a PDU cannot be moved to
another transaction, replayed, or turned into a reply.

Decrypted body: `op(1) || arguments`. The operation code is encrypted too; an eavesdropper sees
only the sizes and the sequence number.

CTR counter block (16 bytes): `masterId(4) || slaveId(4) || seq(4) || dir(1) || blockIndex(3)`.
One (master, slave, seq, dir) is used for exactly one plaintext, ever (§4), so no keystream is
reused. The KDF labels keep K_enc and K_mac independent.

Verification order on the slave (`dmrProvRxPdu`): addressing (dst is our DMR ID, not a group,
src is a configured master) → framing (magic, version, lengths consistent with the PDU) → MAC
(constant-time compare) → sequence (> last seen) → decrypt → operation. Nothing is written
anywhere, and the staging area is not touched, before the MAC and sequence checks pass.

### 3.3 Operations (v1)

| op | name | body after `op` | effect |
| --- | --- | --- | --- |
| 0x01 | STATUS | none | reply STATUS |
| 0x10 | WRITE_CHANNEL | `idx(2 BE) || record(56)` | staged: write channel idx (1..1024), raw codeplug record, marks it in use |
| 0x11 | WRITE_ZONE | `zone(1) || name(16) || channels(80 x u16 LE)` | staged: write zone record `zone` (0..79), marks it in use |
| 0x12 | SET_VFO | `vfo(1: 0 = A, 1 = B) || record(56)` | staged: write the VFO record |
| 0x13 | SET_SETTING | `field(1) || value(4 LE)` | staged: set a whitelisted `nonVolatileSettings` field (§6), saved with `settingsSaveSettings(true)` |
| 0x14 | STORE_KEY | `slot(1) || key(32)` | staged: `dmrAesStoreKey(slot, key)`, slot 1..15 (voice keys only) |
| 0x20 | COMMIT | `count(1) || crc32(4 BE)` | verify the staged image, write it, reply STATUS |
| 0x21 | ABORT | none | discard the staged image |
| 0x30 | REBOOT | none | persist seq, reply STATUS, reboot 1.5 s later |
| 0x81 | STATUS_REPLY (slave → master) | see below | the signed reply |

Anything else, and any op whose body length is wrong, is rejected with result `BAD_OP`. There is
no operation that reads or writes the `PROV` block (pairing key, master list, sequence numbers):
those change only over USB.

STATUS_REPLY body: `result(1) || txn(1) || lastSeq(4) || codeplugCrc(4) || gitHash(8 ASCII) ||
batteryCentivolts(2) || staged(1) || failedMacs(2)`. `codeplugCrc` is the CRC32 of the records of
the last committed transaction **read back from flash** after the write (the master compares it
with the CRC of the image it sent), 0 when nothing was committed since boot. Result codes:
`0 OK, 1 BAD_OP, 2 BAD_SEQ (reply only ever says this for a stale COMMIT), 3 TXN_MISMATCH,
4 CRC_MISMATCH, 5 NO_SPACE, 6 BUSY (arena held by another feature), 7 WRITE_FAILED, 8 TIMEOUT`.

## 4. Sequence numbers, replay, rollover

* The master owns a 32-bit counter, one per master radio across all its slaves; every PDU it
  sends uses the next value. Retransmissions of an unacknowledged PDU re-send **byte-identical**
  bursts (the frame is rebuilt from the same op and seq, so the ciphertext and tag are the same).
  A different payload never goes out under a used seq.
* The master persists its counter in the `PROV` block as a **reservation**: before using a value
  at or above `ownSeqReserved` it writes `ownSeqReserved + 64` to flash, so there is one flash
  write per 64 PDUs and after a reboot the counter resumes above everything it may have used.
* The slave keeps, per configured master, `lastSeq` in flash (the `PROV` block) and `lastSeen`
  in RAM (>= `lastSeq`). A PDU is accepted only if `seq > lastSeen`; accepting it sets `lastSeen`.
  `lastSeq` is written to flash only when an operation **commits** (COMMIT, REBOOT) and then
  equals that PDU's seq, which is above every PDU of the transaction. A staged-but-never-committed
  transaction can thus be replayed after a slave reboot, which only re-stages the same bytes; it
  cannot be committed without a fresh, higher-numbered COMMIT from the real master.
* STATUS requests do not persist (a flash sector erase per poll would wear the flash out), so a
  captured STATUS request can be replayed **once per slave power cycle**, costing one reply
  transmission to the master ID. Documented in the threat model.
* A stale COMMIT (seq == the last committed seq, i.e. the master's retry after it missed the
  STATUS reply) is answered with the **cached** STATUS reply, byte for byte; no new encryption,
  no new nonce, no flash access. That re-send is bounded: at most 2 times, and only within 30 s
  of the first reply (the master's three 6 s tries fit inside), so a captured COMMIT or STATUS
  replayed later gets nothing (bench 2026-10-29 found the unbounded version made the slave
  transmit its reply on every replay).
* Rollover: the master refuses to send once its counter reaches `0xFFFFFF00` and shows
  "Prov: re-key needed". The slave never accepts `0xFFFFFFFF`. At one PDU per second a counter
  lasts 136 years, so this is a guard, not an expected event.
* A desynchronised master (its counter below what the slave has stored: a master restored from an
  old flash image, or two masters with the same DMR ID) has every PDU rejected with no reply.
  Recovery is **re-keying over USB** with `provision.py pair`. The tool reads the radio's stored
  counters first (`show` data, no key bytes) and writes them back unchanged with the new key, so
  re-pairing never lowers a counter by accident; `--reset-seq` zeroes them and is only safe together
  with a new key (it reopens the old traffic to replay under the old key, which is then gone);
  `--seq M N` sets the stored counter for master M and `--own-seq N` the radio's own reservation,
  for the deliberately desynchronised master (set its reservation above the slaves' stored values,
  or just re-key everybody).

## 5. Transaction state machine (slave)

```
            PDU(op in 0x10..0x14, txn T)                  COMMIT(T, count, crc) ok
  IDLE ------------------------------------> STAGING(T) ---------------------------> COMMITTING -> IDLE
   ^   claim arena; stage item                 | stage more items (same T)              | write items in order,
   |                                           | PDU with txn != T: discard, restage     | persist lastSeq, read back,
   |          ABORT / timeout (120 s idle)     | (a new transaction started)             | cache + send STATUS
   +-------------------------------------------+                                         |
   +-------------------------------------------------------------------------------------+
```
* Staged items live in the feature arena (owner `FEATURE_ARENA_PROVISION`), claimed on the first
  staged item and released at IDLE. If another feature holds the arena (the satellite screen, a
  running VFO sweep) the slave replies `BUSY` (as a STATUS reply to the COMMIT, since only COMMIT
  gets a reply) and stages nothing.
* Image format in the arena: `{ u8 txn; u8 count; u16 used; }` then items `op(1) || len(2 LE) ||
  body`. COMMIT's `count` must equal the number of staged items and its `crc32` must equal the
  CRC32 (IEEE, as in `crc32_ieee` in `dmr_prov.c` and `zlib.crc32` on the host) over the item
  bytes in staging order; a mismatch discards the image (`CRC_MISMATCH`), so a clobbered or
  spliced image never reaches flash.
* Writes happen in the main loop task (`dmrProvTick`), never in the ISR or the USB critical
  section, item by item through `dmr_prov_apply.c`; a failing write stops the commit, the STATUS
  reply says `WRITE_FAILED` and which item (in `staged`), the sequence number is still persisted
  (the master must not retry the same seq with new data; it starts a new transaction).
* Single-PDU operations (STATUS, REBOOT) do not touch the staging area and work inside or
  outside a transaction; REBOOT inside STAGING discards the image first.

## 6. Flash write path

All through existing, main-loop-safe functions:

| op | writer | notes |
| --- | --- | --- |
| WRITE_CHANNEL | `codeplugChannelSaveDataForIndex()` after `bcd2int`/CSS conversion of the raw record (the function converts back), then `codeplugAllChannelsIndexSetUsed()` | channels 1..128 are in EEPROM, 129.. in SPI flash sectors |
| WRITE_ZONE | `EEPROM_Write(CODEPLUG_ADDR_EX_ZONE_LIST + zone * (16 + 2 * channelsPerZone), name || channels)` and the in-use bit at `CODEPLUG_ADDR_EX_ZONE_INUSE_PACKED_DATA` | a 16-channel codeplug takes the first 16 channel entries of the record |
| SET_VFO | `codeplugSetVFO_ChannelData()` plus the live copy in `settingsVFOChannel[]` | |
| SET_SETTING | field table in `dmr_prov_apply.c` (`txPowerLevel`, `backlightMode`, `backLightTimeout`, `displayContrast`, `scanDelay`, `scanModePause`, `scanStepTime`, `dmrDestinationFilter`, `dmrCcTsFilter`, `privateCalls`, `txTimeoutBeepX5Secs`, `micGainDMR`, `micGainFM`, `ecoLevel`, `apo`, `autolockTimer`, `currentZone`, `beepVolumeDivider`, `bitfieldOptions[0..1]`), then `settingsSaveSettings(true)` once per commit | the DMR ID, callsign and anything in the `PROV` block are not settings fields |
| STORE_KEY | `dmrAesStoreKey()` | the only way key bytes travel: encrypted and authenticated like everything else |
| REBOOT | `settingsSaveSettings(true)` then `NVIC_SystemReset()` from a timer callback | after the STATUS reply has been transmitted |

The sequence number is persisted (`codeplugSetOpenGD77CustomData` on the `PROV` block) after the
items, before the STATUS reply is built, so a reply never claims a commit the flash does not hold.

## 7. Master (sender)

The master has no keypad UI in v1. A **job** (slave ID, txn, a list of items in the same
`op || len || body` format as the staging image, followed by an implicit COMMIT, or a single
STATUS / REBOOT) is handed over USB (`provision.py job ...`, CPS command `0x9A`) into the feature
arena and `dmrProvTick()` walks it:

```
NEXT_ITEM -> build frame(seq = next) -> dmrDataTxLoad(bursts) -> SENDING (wait for the data call
to end) -> WAIT_ACK (Response PDU from the slave with N(S) == seq & 7, 1.5 s) -> NEXT_ITEM
                                                            \-> no ACK: resend the same bursts, 3 tries, then FAILED
COMMIT / STATUS / REBOOT items additionally -> WAIT_STATUS (5 s for the signed STATUS_REPLY with
seq == the request's seq; verified like a request, master and slave swapped) -> DONE / FAILED
```
The link-layer Response PDU is captured by a hook in `dmrSmsRxBurst()` (DPF 1, individual, to our
ID) and consumed by the master state machine; it is not authenticated (§9) and only advances the
sender. On the air (bench 2026-10-30, two fork radios, HackRF) the slave's Response PDU starts
30 ms after the master's last block ends, in the next slot of the same timeslot. The data-call
harness used to follow the last block with two terminator bursts and only then return to
receive, so the master never heard it. In direct mode a data call now ends with its last block:
the HR-C6000 is put back into receive in the same interrupt, the RF chip from the timer context
(as the SMS acknowledgement's end does), and `dmrDataTxFinishPoll` waits 200 ms before the un-key
that re-initialises the RF chip, and never un-keys once the next call has been loaded (the master
loads the next PDU ~60 ms after the Response, inside that window; the first on-air build broke
every second PDU that way). Repeater mode keeps the terminator. This also applies to the SMS
sender, which shares the harness. Verified on the air 2026-10-30, two fork radios: jobs complete
in both directions, the signed STATUS verifies, the codeplug is written. The link ACK path sets
the PA drive for the TX frequency itself (a radio that had not transmitted since boot sent its
ACK about 31 dB down before that). Progress and the final STATUS (result, codeplug CRC match, battery, git hash) are shown
with `uiNotificationShow()` ("Prov 2345678: sent 17/17, OK, crc match") and are readable over USB
(`0x9B`) so the bench runs hands-free. The master never writes anything to its own codeplug.

## 8. RAM budget

Measured with xPack arm-none-eabi-gcc 14.2.1 at -Os (2026-10-29), `objdump -h` section sizes:

| build | .text | .rodata | .data | .ccmram | .bss | SRAM spare | CCM spare |
| --- | --- | --- | --- | --- | --- | --- | --- |
| stock (`make`, byte-identical to the tree before this work) | 0x4113C | 0xCE78 | 0x325C | 0xD890 | 0x18374 | 17,452 | 10,096 |
| fw_sms (`ENABLE_DMR_DATA ENABLE_KEY_INJECTION`) | 0x4430C | 0xD030 | 0x32CC | 0xE500 | 0x1893C | 15,860 | 6,912 |
| fw_sms + `ENABLE_REMOTE_PROVISION` (= `PROFILE=dev-prov`) | 0x46204 | 0xD100 | 0x32CC | 0xE6BC | 0x1893C | 15,860 | 6,468 |

The flag costs 8,136 bytes of code, **0 bytes of main SRAM** (`.bss` and `.data` are unchanged: the two SAP
bytes in `dmr_sms.c` fit in alignment padding) and 444 bytes of CCM, all of it the module's static state
(`dmr_prov.c`: 417 bytes):

| item | bytes | where |
| --- | --- | --- |
| derived keys K_enc, K_mac | 64 | CCM |
| `PROV` block copy (key, masters, counters) | 96 | CCM |
| cached STATUS reply PDU | 80 | CCM |
| slave state (txn, lastSeq/lastSeen x4, timers, counters, hold-off) | ~80 | CCM |
| master state (phase, cursor, tries, timers, last ACK, last STATUS body) | ~70 | CCM |
| staging image / job | 0 | feature arena (11,904 B, shared with the satellite screen and the sweep) |
| frame assembly | 0 | the master builds its PDU at the top of the arena; the slave decrypts in place in `dmrSmsRxTick`'s stack copy; bursts go through the 520-byte queue scratch already static in `dmr_sms.c` (`dmrSmsBurstQueueScratch()`) |

No new large buffers: the PDU arrives in the SMS reassembler's existing CCM buffer, the staging
image and the job use the arena, and the outgoing bursts use the SMS burst-queue scratch and
`dmr_data.c`'s queue. CCM is not zeroed at boot; `dmrProvInit` sets every field.

## 9. Threat model

| Attacker | Capability | Protected? | How / why not |
| --- | --- | --- | --- |
| Passive listener (HackRF, DSD-FME) | sees every burst | yes | bodies are AES-256-CTR; the listener learns sizes, IDs, seq and that provisioning is happening |
| Active, no key | injects, modifies, replays, re-addresses PDUs | yes | 128-bit CMAC over IDs, header and body; seq > last seen; nothing is staged or written before the MAC passes |
| Active, no key | replays an old transaction | yes | monotonic seq persisted on commit; the slave answers a stale COMMIT only with the cached reply |
| Active, no key | replays a captured STATUS request or COMMIT | partly | a STATUS request is accepted at most once per slave power cycle (not persisted); the cached reply to the last answered request is re-sent at most twice within 30 s of it; the only effect is a STATUS transmission to the master ID |
| Active, no key | sends garbage under a master ID to provoke transmissions | partly | the hold-off stops the ISR ACK; a valid PDU (real master) during the hold-off is still processed and answered |
| Active, no key | spoofs the master ID to make the slave transmit | partly | the slave sends the one-burst link ACK to any well-formed SAP 9 PDU from a configured master ID (the ISR cannot verify a MAC); after 4 failed MACs in a row the slave stops acknowledging for 10 s; it never sends a STATUS |
| Active, no key | spoofs the slave's link ACK / STATUS to the master | ACK: no, STATUS: yes | a forged ACK only makes the master move on (the slave then rejects or ignores the later PDUs; the STATUS at COMMIT is signed and tells the truth); a forged STATUS fails its MAC |
| Active, no key | floods the slave with bad PDUs | partly | each costs one CMAC (~30 µs); the hold-off stops ACKs; the SMS reassembler is shared, so a flood also delays SMS. Denial of service on the channel itself is not addressable by any protocol |
| Physical access to a paired radio | reads SPI flash (CPS, DFU, chip-off) | **no** | the pairing key is in the codeplug region in the clear. Treat a lost radio as a key compromise: re-key the fleet |
| Insider with the fleet key | anything a master can do | **no** | one key per fleet; use per-slave keys (load a different key into the master per slave) when that matters |
| Firmware of the master itself | | out of scope | the host tool and the USB port are trusted |
| Second master with the same DMR ID and key | nonce reuse | **no** | master IDs must be unique per pairing key; the tool warns when the key it writes matches another radio's with the same ID |

Regulatory: DMR data on amateur bands may not be encrypted in many countries; this is for licensed
commercial/PMR use (same as the AES voice this fork exists for).

## 10. Test plan

Host (`sh tests/run_all.sh`, plain gcc):

* `test_prov_crypto`: AES-CMAC against RFC 4493 (AES-128, the four message lengths) and SP 800-38B
  AES-256 vectors; the KDF against vectors produced by `tools/prov_crypto.py`; CTR against NIST
  SP 800-38A F.5.5; encrypt-then-MAC frame round trip; constant-time compare.
* `test_prov_rx`: the slave through the **real** `dmr_sms.c` reassembler, bursts played into
  `dmrSmsRxBurst()` and `dmrSmsRxTick()` as the ISR and main loop do, with the master's real frame
  builder (`dmrProvBuildPdu`) producing the bursts:
  * a full multi-PDU transaction (channels + zone + COMMIT) commits through the apply stubs, in
    order, with the right bytes; STATUS reply verifies and its codeplug CRC matches;
  * MAC failure, replay (same seq), reordered (lower seq), wrong slave ID, wrong master ID,
    truncated PDU, spliced transaction (txn mismatch), wrong COMMIT crc: each rejected without
    touching the staging area, with the failed-MAC counter and hold-off behaving as specified;
  * a lost ACK: the master's byte-identical retransmission is link-acknowledged and ignored; a
    master retry of COMMIT gets the cached reply;
  * an encrypted SMS (SAP 9 + ENC header) still decodes, and a provisioning PDU never reaches
    the SMS inbox.
* `test_sms_rx` and the seven other programs keep passing unchanged.

* `test_sms_rx_prov`: the whole SMS receive test again with the provisioning hooks compiled into
  `dmr_sms.c` and an unpaired radio (the hooks must change nothing).
* The Python builder is checked against the C builder byte for byte (same key, IDs, seq, txn and
  body give the same 16 bursts, CRC-9 and header CRCs included), and `prov_packet.py decode` reads
  the C builder's blocks back.

Bench (one fork radio, one stock TYT, HackRF + DSD-FME), see BUILD.md "Remote provisioning":

1. Flash `PROFILE=dev-prov` (`ENABLE_DMR_DATA ENABLE_REMOTE_PROVISION ENABLE_KEY_INJECTION`); load a
   pairing key and master ID with `provision.py pair --gen-key --master-id <M>` (USB `0x9B/5`, the
   block goes through `codeplugSetOpenGD77CustomData` in the main task); `provision.py show` reads
   the masters, counters and job state back (no key bytes). Report: the `show` output.
2. Slave path without RF: `prov_packet.py replay --key ... --master <M> --slave <this ID> --seq 1
   --txn 7 --chan 5 ... --commit` builds the transaction on the host with the same key and injects
   its bursts over USB (`0x9B/2` → `dmrSmsRxBurst`, `0x9B/3` → terminator); it prints the slave's
   counters after every PDU. Report: staged count after each item, `commits 1` after COMMIT,
   `provision.py show` (stored lastSeq = the COMMIT's seq), the channel read back with the CPS
   (`R` command, channel 5 in EEPROM at 0x3790 + 4 x 56), and whether the radio keyed a reply
   (red LED / HackRF: a 6 CSBK + header + 6 block data call to the master ID, `decode --reply`).
   Then run the same command again: every PDU must be rejected (`rejSeq`), nothing re-committed.
3. Master path: `provision.py job --slave <S> --txn 7 --chan 5 ... --wait`, HackRF capture, DSD-FME.
   Report: DSD-FME's view of the data call (CSBK preamble, "Confirmed" header, SAP 9, the block
   count, and its CRC-9 verdict on each block: that verdict is the one open question), the block
   bytes fed to `prov_packet.py decode` (must print WRITE_CHANNEL 5 ... then COMMIT), the master's
   `--wait` outcome (with no slave on the air: `failed (TIMEOUT)` after 3 sends of seq 0, about 7 s
   each), and whether the stock TYT showed anything (it must not).
4. Loopback on the one radio: the capture of step 3 replayed into the same radio with
   `prov_packet.py replay` under its own ID is rejected (`rejAddr`: its own ID is not a configured
   master); with its own ID paired as a master it is accepted once and rejected on replay.
5. Hold-off: inject a PDU with a flipped ciphertext byte four times (`replay` with `--seq` reused,
   or any garbage frame with the right magic): `failedMacs 4`, `ackInhibit 1`, no red LED for the
   next good PDU within 10 s, ACKs again after.
