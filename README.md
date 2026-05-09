# tr8-bridge

A small ALSA daemon that fixes erratic timing on the **Roland TR-8** (and
likely other class-compliant USB-MIDI gear with the same firmware quirk) when
connected over USB to a Linux host.

## Purpose

If you own a Roland TR-8 and have ever plugged it into a Linux (or Windows)
machine over USB and watched it suddenly start stuttering, drifting, or
stalling mid-pattern, this daemon is for you. It transparently restores
solid timing while still letting your DAW talk to the device over standard
ALSA / PipeWire / JACK MIDI plumbing.

It exists because moving a hardware studio onto Linux shouldn't mean giving
up gear you already own.

## Requirements

* Linux with a recent enough kernel (`snd-usb-audio` and `snd-seq` are
  in-tree on basically every modern distro)
* `libasound2-dev` (Debian/Ubuntu) / `alsa-lib` (Arch) / `alsa-lib-devel`
  (Fedora) for the build
* `make`, `gcc` (or `clang`)
* `systemd` and `udev` (for auto-start on plug-in; not strictly required if
  you only want to run it by hand)
* PipeWire or JACK — to expose the bridged port to your DAW. ALSA seq alone
  also works for any seq-aware client.

## The problem

When a Roland TR-8 is plugged in over USB and no userspace client is reading
from its rawmidi node, the on-board sequencer's timing falls apart —
stutters, random speedups and slowdowns, multi-second stalls. Unplug USB,
or use 5-pin DIN MIDI instead, and timing is rock solid.

The cause is USB-MIDI 1.0 transport flow control:

* DIN MIDI is a fire-and-forget UART. The TR-8 blasts bytes onto the wire
  whether anyone's listening or not. There is no flow control.
* USB-MIDI 1.0 runs over a USB **bulk endpoint**. Bulk transfers have
  implicit flow control: if the host doesn't issue IN tokens (or its kernel
  buffer fills), the device's TX FIFO fills and stays full.
* The Linux `snd-usb-audio` driver only aggressively pulls bytes off the
  device while a userspace client has the rawmidi node open and is reading.
  Without one, the kernel buffer fills and stays full.
* The TR-8's firmware appears to use a blocking USB-TX path. With the FIFO
  full, the sequencer's tight loop ends up gated on USB drain timing instead
  of the device's own quartz, and audible timing chaos follows.

## The fix

`tr8-bridge` opens every substream of the TR-8's rawmidi node exclusively,
drains them continuously, and re-publishes each as a normal ALSA sequencer
port. The TR-8 exposes two MIDI ports, so you'll see two seq ports under a
single `tr8-bridge` client:

```
tr8-bridge: TR-8 MIDI 1 (bridged)
tr8-bridge: TR-8 MIDI 2 (bridged)
```

PipeWire and JACK pick these up as regular MIDI devices, so DAWs subscribe
to the bridged ports instead of fighting over the rawmidi node. The TR-8's
sequencer reverts to running off its own quartz, and clock is solid again.

Port names are derived automatically from the kernel rawmidi subdevice
info, so the daemon Just Works for other multi-port USB-MIDI gear with the
same back-pressure quirk.

## Caveats

* This eliminates the **back-pressure stall**. It does **not** eliminate
  the inherent ~1 ms USB-frame-aligned jitter of USB-MIDI 1.0. If you need
  tight clock sync, DIN MIDI is still the right answer.
* While the bridge is running, the underlying rawmidi node is held
  exclusive. Anything that tries to open the raw hardware MIDI port
  directly (rather than through ALSA seq / PipeWire / JACK) will get
  `EBUSY`. That is the trade-off — it is also the whole point.

## Build and install

Dependencies (Debian/Ubuntu): `build-essential libasound2-dev`.
Arch: `base-devel alsa-lib`. Fedora: `gcc make alsa-lib-devel`.

```sh
make
sudo make install
sudo udevadm control --reload
sudo systemctl daemon-reload
```

Re-plug the TR-8. The daemon should start automatically. Verify:

```sh
aconnect -l | grep -A2 tr8-bridge
systemctl status 'tr8-bridge@*'
journalctl -u 'tr8-bridge@*' -f
```

You should see one or more ports under the `tr8-bridge` client (one per
MIDI substream the device exposes). Connect your DAW to those instead of
the raw `TR-8` ports.

## Verifying or extending the device match

The shipped udev rule matches the original Roland TR-8 (USB ID `0582:017c`,
verified against a real unit). To verify or extend for other Roland gear:

```sh
lsusb | grep -i roland
```

Add the new VID:PID to `udev/71-tr8-bridge.rules` and re-install.

## Running manually (debugging)

```sh
# Find your TR-8's card
cat /proc/asound/cards

# Run the daemon directly (Ctrl-C to stop)
./tr8-bridge -d midiC1D0
```

The `-d` flag accepts:

* `hw:X,Y` or `midiCXDY` or `/dev/snd/midiCXDY` — bridge **all** substreams
  of card X, device Y (the typical case)
* `hw:X,Y,Z` — bridge only substream Z (rare; mostly for debugging)

If you see timing improve while the daemon is running and revert when it
stops, you've reproduced the fix.

Useful while debugging:

```sh
# Per-substream owner / overruns / drained byte counters
cat /proc/asound/card<N>/midi0
```

## How was this discovered?

Originally diagnosed via [ShowMIDI](https://github.com/gbevin/ShowMIDI), a
GUI MIDI activity monitor. Running ShowMIDI happens to drain the rawmidi
node as a side effect of subscribing to it for display, which incidentally
masks the TR-8's USB-TX stall. `tr8-bridge` does the same drain in a small
headless daemon, leaving the seq port available for cooperative use by any
PipeWire / JACK MIDI consumer.

## Contributing

Issues and pull requests welcome. If you have a different Roland (or other)
unit that exhibits the same USB-MIDI back-pressure stall and the bridge
fixes it for you, please open a PR adding the VID:PID to the udev rule (or
file an issue with the IDs and I'll add it).

## License

GPLv3. See [`LICENSE`](LICENSE).
