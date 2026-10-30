# Setting up a fleet radio

How a new MD-UV390 Plus 10W becomes a member of a provisioned fleet: two kinds of radio, the
**masters** (a base station and a handheld, full menus, they push the configuration over the
air) and the **handhelds** in simple mode (a fixed channel list, private calls by name, preset
messages, everything else behind a PIN). Every example below uses placeholders; the real names,
DMR IDs, callsigns, keys, frequencies and presets live in `MDUV380_firmware/private/fleet.json`,
which git ignores (`private/`, `*.fleet.json`, `*.key`, `*.codeplug.bin`) and which must never be
copied into a tracked file, a commit message, a test or a document.

What the pieces are: `BUILD.md` (build, flags, flashing), `MDUV380_firmware/PROVISIONING.md`
(the over-the-air protocol), `tools/provision.py` (USB: pairing, identity, jobs, status),
`tools/prov_packet.py` (records, items, bench replay and decode).

## 1. The fleet file

`private/fleet.json`, one per fleet:

```json
{
  "fleet": { "name": "Example", "pairing_key": "GENERATE",
             "voice_keys": [ { "slot": 1, "key": "GENERATE" } ], "masters": [ 1234567, 2345678 ] },
  "radios": [
    { "unit": "base",   "person": "Base",  "dmr_id": 1234567, "callsign": "BASE",  "role": "base-master", "simple_mode": false },
    { "unit": "radio4", "person": "Alice", "dmr_id": 2345678, "callsign": "ALICE", "role": "master",      "simple_mode": false },
    { "unit": "radio2", "person": "Bob",   "dmr_id": 3456789, "callsign": "BOB",   "role": "slave",       "simple_mode": true }
  ],
  "contacts": [
    { "index": 1, "name": "Fleet", "id": 1,       "type": "group" },
    { "index": 2, "name": "Base",  "id": 1234567, "type": "private" },
    { "index": 3, "name": "Alice", "id": 2345678, "type": "private" },
    { "index": 4, "name": "Bob",   "id": 3456789, "type": "private" }
  ],
  "channels": [
    { "index": 1, "name": "Fleet",   "rx_hz": 446006250, "tx_hz": 446006250, "mode": "digital", "color_code": 1,
      "timeslot": 1, "contact": 1, "power_w": 0.5, "encrypt": 1, "bandwidth_25k": false },
    { "index": 2, "name": "Fleet 2", "rx_hz": 446031250, "tx_hz": 446031250, "mode": "digital", "color_code": 1,
      "timeslot": 1, "contact": 1, "power_w": 10,  "encrypt": 1, "bandwidth_25k": false },
    { "index": 3, "name": "Open",    "rx_hz": 446093750, "tx_hz": 446093750, "mode": "analog",  "power_w": 0.5,
      "bandwidth_25k": false, "rx_tone": null, "tx_tone": null }
  ],
  "zones": [ { "index": 0, "name": "Fleet", "channels": [1, 2, 3] } ],
  "home": { "zone": 0, "channel": 1, "idle_return_minutes": 5 },
  "message_presets": [ "All well", "Need help", "At home", "Where are you?", "On my way" ],
  "settings": { "privateCalls": 1, "backLightTimeout": 10, "ecoLevel": 1, "apo": 0, "autolockTimer": 0 }
}
```

* `fleet.pairing_key` and every `voice_keys[].key` start as `GENERATE`; step 4 replaces them with
  real 64-hex keys, kept only in this file (and in the radios).
* `fleet.masters` are the DMR IDs the handhelds accept provisioning from.
* `contacts[].index`, `channels[].index` are codeplug slots (1-based); `zones[].index` is 0-based.
  `channels[].contact` is the contact slot of the channel's talkgroup, `encrypt` the voice key
  slot (0 = none), `power_w` one of 0.05, 0.25, 0.5, 0.75, 1, 2, 3, 5, 10.
* `home.channel` is the codeplug channel index of the home channel (its position in the home
  zone is looked up on the radio); `idle_return_minutes` 0 = never, 1..240.
* `delete_contacts` lists contact slots to clear on radios that still hold older contacts (the GREEN list shows
  every private contact the radio has).
* `message_presets` are ASCII (the radio font has no diacritics), up to 10 of 47 characters; all
  ten slots are written, so dropping a line removes it from the radios.
* `settings` names are the whitelist in `PROVISIONING.md` §6; a value that is not a number (such
  as `"per-channel"` for `txPowerLevel`, when the channels carry their own power) is skipped.

## 2. Back up the stock codeplug

Before the first flash of a radio, read its TYT codeplug with the TYT CPS (Program > Read
From Radio, then File > Save, `private/<unit>-stock.rdt`). This is the only copy of the
factory calibration-adjacent data the TYT CPS can restore; keep it with the fleet file. After
flashing, the OpenGD77 CPS can back up the whole codeplug region as well
(`private/<unit>.codeplug.bin`).

## 3. Flash the fleet firmware

The **fleet** profile is `full` plus `ENABLE_REMOTE_PROVISION` and `ENABLE_SIMPLE_MODE`: the
same image on every radio of the fleet, masters and handhelds alike (the simple mode is a
settings bit, step 5 sets it per radio).

```sh
cd MDUV380_firmware && make -j$(nproc) PROFILE=fleet
./../flash.sh /path/to/MD9600-CSV2571V5-V26.45.bin fleet      # the AMBE donor, see BUILD.md
```

Power the radio on with the side buttons held for DFU first (BUILD.md, "Manual flash"). The
first boot runs on the TYT codeplug the radio had; nothing of the fleet is on it yet.

## 4. Identity, PIN and pairing key over USB

With the radio on and connected (it enumerates as the OpenGD77 CPS port):

```sh
cd MDUV380_firmware
# the first time only: make the fleet's keys and put them in private/fleet.json
python3 tools/provision.py pair --gen-key --master-id 1234567 --master-id 2345678   # prints the pairing key once
python3 -c "import os; print(os.urandom(32).hex())"                                   # a voice key for voice_keys[0]

# every radio: its own DMR ID and callsign, the menu PIN (handhelds), and the pairing key + masters
python3 tools/provision.py identity --from-json private/fleet.json --radio radio2 --pin 1234
python3 tools/provision.py pair --from-json private/fleet.json
python3 tools/provision.py show            # paired: yes, the two masters, counters 0
```

Then power-cycle the radio: the DMR ID and callsign are read at boot. The PIN is the stock
codeplug "boot password"; in simple mode it is **not** asked at power-on, it guards the menu
instead (GREEN held on the channel screen). A master gets no PIN unless you want one at boot.
Masters are paired too (same key; their `masters` entry lets the other master provision them).

## 5. The provisioning job

The job rides DMR data on the channel both radios are on, so the new radio must be able to hear
the master before it has the fleet channels: on the handheld, set VFO A to the fleet's channel 1
frequency, colour code and timeslot (digital) and leave it there; on the master, select channel
1. Then, from the bench PC connected to the **master**:

```sh
python3 tools/provision.py job --from-json private/fleet.json --radio radio2 --wait
```

`--radio radio2` picks that unit's identity and `simple_mode` from `radios`, the slave address
from its `dmr_id` (pass `--slave <id>` when the radio still has another ID), and sends the
common contacts, channels, zone, voice key, presets, settings and home as one transaction (a
job above one USB command is split into transactions sent one after the other, the identity in
the last). The tool prints every item and then the outcome: `outcome: done` and a STATUS line
with `result OK`, the codeplug CRC, the slave's git hash and battery. The slave writes the
codeplug on COMMIT, saves its settings (the simple-mode bit among them), answers, then takes its
new DMR ID, and, with the bit set, goes to the home channel on the next main-loop tick.

Order of a mixed job: the handhelds first, then each master provisioned by the other master
(the base by radio4, radio4 by the base; a radio never writes its own codeplug over the air).

**Over USB instead (the first provisioning, or a master).** The same job can be injected into the
radio on the bench PC without any RF: it goes through the slave's own receive path under one of its
configured master IDs, so no VFO bootstrap and no second radio are needed:

```sh
python3 tools/prov_packet.py replay --key <the pairing key> --master 2345678 --slave <this radio's current ID> \
        --seq 1 --txn 1 --from-json private/fleet.json --radio radio2 --commit
```

`--slave` is the ID the radio has *now* (it changes with the identity in the job; the next replay
uses the new one and a higher `--seq`), `--master` one of `fleet.masters`. The tool prints the slave's
counters after every PDU (`staged N`, then `commits 1`), and `provision.py show` has the committed
sequence number. The bench masters were set up this way (2026-10-30).

## 6. Verify

On the handheld (power-cycle it once; it should come up on the home channel):

| check | expected |
| --- | --- |
| boot | channel screen, home zone and channel, no PIN prompt |
| RED | nothing (or ends a private-call target); never the VFO |
| knob / keypad arrows | channels of the home zone only; SK2 + knob does nothing |
| GREEN | the private contacts; select one, PTT calls it; 60 s after the last over the target is the group again |
| GREEN held | PIN prompt; the right PIN opens the menu, RED cancels; back on the channel screen the PIN is asked again |
| SK1 | the preset list; GREEN, then "All: <group>" or a contact, GREEN sends; the base shows "SMS: ..." |
| SK1 held | the inbox |
| idle | on another channel, no key or PTT for `idle_return_minutes`: back to the home channel |
| `provision.py show` over USB | `paired: yes`, lastSeq = the COMMIT's seq, `commits 1` |

From the base: a group call on channel 1 is heard on the handheld; a private call to it rings
with the accept box (PTT or GREEN accepts); a message from it arrives in the base's inbox.

## 7. The monthly STATUS job

From the base, for every handheld:

```sh
python3 tools/provision.py job --slave 3456789 --status --wait
```

Report the STATUS line: `result OK`, `lastSeq`, `battery` (volts) and `git` (the firmware the
handheld runs). A radio that does not answer three tries is off or out of range; a `BAD_SEQ`
or silent radio after a master was restored from an old image needs re-pairing (§4 of
`PROVISIONING.md`).

## 8. Changes and incidents

* **New presets, channels or contacts:** edit the fleet file, run the job again for each radio
  (the same items are rewritten; `txn` may stay 1, the sequence numbers make the job new).
* **Switching a handheld back to a full radio:** a job with `"simple_mode": false`, or over USB
  `provision.py job ... --setting simpleMode 0` from a master, or clear the bit with the CPS.
* **A lost or stolen radio** holds the pairing key and the voice key: generate both anew, `pair`
  every remaining radio with the new pairing key and push the new voice key with a job.
* **A master changes its DMR ID:** the handhelds accept only the IDs in their `masters` list, so
  pair them again with the new list; the master's own counter carries on.
* Printed rule card for the handhelds (which channel, when to switch, what the two keys do) is
  part of the private fleet file, not of this repository.
