# Linux 6.18 for WD My Cloud Home (Realtek RTD1295 "Monarch")

This tree is vanilla `v6.18.51` (rebased from the initial `v6.18` this port
started on via a 3-way merge, `v6.18` as merge base — see "Updating the base
version" below) plus a board port for the WD My Cloud Home
(single-bay), a NAS built around Realtek's RTD1295 SoC. It replaces the
vendor's stock kernel, `linux-4.9.330`, which shipped as GPL source at
[symops/monarch-4.9.330](https://github.com/symops/monarch-4.9.330) — that
tree is this port's primary hardware reference throughout.

Mainline already carries generic RTD1295/RTD129x SoC support (maintained by
Andreas Färber): GIC, ARM timer, three UARTs, and not much else. Every
peripheral this board actually needs — SATA, USB, Ethernet, RTC, PWM,
cpufreq, SMP bring-up — had no mainline driver at all before this port, and
had to be forward-ported from the vendor's 4.9-era Realtek BSP.

## Status

Verified end-to-end on real hardware: SMP (4 cores), SATA (real disk,
6.0 Gbps link, RAID assembly, root mount), USB 2.0/3.0 (host, mass
storage), Ethernet, RTC (flash-backed, survives reboot), PWM/LED,
cpufreq (all 3 board OPPs), watchdog, thermal.

Not done: GPIO/pinctrl (no mainline `gpio-rtd129x`/`pinctrl-rtd129x`
driver exists yet — see below), I2C PMIC regulator stack (built but
doesn't probe, see below), full CRT clock controller (only the one CPU
clock needed for cpufreq is ported, not the whole vendor
`clk_pll_div`/mux/gate framework).

## Where this code came from

Three sources, in order of how much of each thing is used:

1. **This port's own new code**, written directly against the vendor
   4.9.330 driver as a functional/register-level reference, targeting
   modern kernel APIs from the start rather than adapting someone else's
   forward-port. This is the SATA glue driver, the SCPU (CPU) clock
   driver, the PWM controller driver, the flash-backed RTC + SPI-NOR
   controller, and the IRQ-affinity fix.
2. **[Fireblossom/wd-mch-kernel](https://github.com/Fireblossom/wd-mch-kernel)**,
   an existing community 4.9→6.18 port for this exact device
   (`docs/PORTING_GUIDE_4.9_to_6.18.md` there documents its own
   per-file reasoning). Adopted largely as-is for SMP bring-up, GMAC
   Ethernet, USB dwc3/PHY, watchdog, thermal, IRQ mux, I2C, and the
   MFD/regulator stack, plus the board DTS skeleton.
3. **The vendor 4.9.330 tree itself**, as the ground-truth register
   reference for everything above, and as the direct source for the
   parts of (1) that needed genuinely new forward-porting.

## Acknowledgments

Thanks to [Fireblossom](https://github.com/Fireblossom) for
[wd-mch-kernel](https://github.com/Fireblossom/wd-mch-kernel) — this
port would have taken considerably longer without that existing 4.9→6.18
groundwork to build on for SMP bring-up, GMAC Ethernet, USB dwc3/PHY,
watchdog, thermal, IRQ mux, I2C, the MFD/regulator stack, the board DTS
skeleton, and the rescue-userspace `init` script this port's own
storage load-order fixes sit on top of.

## Change summary (relative to vanilla v6.18)

### Board

- `arch/arm64/boot/dts/realtek/rtd1295-wd-mycloud-home.dts` (new, ~730
  lines) — SoC/board description forward-ported from the vendor's
  `rtd-1295-monarch-1GB.dts`, including board-specific SATA PHY
  calibration tables (`phy-param`/`tx-driving-tbl`/`rx-sense-tbl` —
  without these the mainline PHY driver falls back to generic defaults
  for a *different* reference board and the disk never links, see
  "SATA" below).
- `arch/arm64/kernel/smp_spin_table.c` — this SoC's secondary CPUs spin
  on a device register, not a normal RAM address; small patch to the
  generic spin-table code to support that.

### Storage (SATA)

- `drivers/ata/ahci_rtd1295.c` (new) — platform driver on top of the
  standard `ahci_platform`/`libahci_platform` framework, not
  `generic-ahci`: this SoC's AHCI block needs several vendor-specific
  register pokes generic-ahci knows nothing about (MMIO offsets
  `0xf20`/`0xc`), a drive-bay power GPIO raised by raw MMIO poke (no
  mainline `gpio-rtd129x` driver/binding exists to do this properly
  yet), and a deferred host-init delay matching the vendor driver's
  default. Most importantly: an explicit deassert of the
  `SATA_PHY_POW_0` reset bit (CRT offset `0x00`, bit 10) — the SATA
  analog PHY power macro is left in hardware reset by the boot loader
  on a cold boot path (reproducible via the physical USB-Install
  button, which skips U-Boot's own SATA scan), and every *other*
  register write that looks plausible (MDIO calibration, SB2 gate
  bits, the AHCI vendor registers above) silently no-ops against it
  because they live in unrelated reset/clock domains. This one bit was
  the actual root cause after several other SATA-link-down leads
  (calibration tables, GPIO, timing) turned out to be red herrings.
  **Deliberately left source-divergent from symops/pelican-6.18's
  version of this file** (audited 2026-08-25, alongside every other
  driver shared between the two ports -- everything else came back
  byte-for-byte identical). Duo's copy was rewritten into a table-driven
  `ahci_rtd1295_port_quirk[]` structure that also enables the SATA
  clock-gate (CRT+0x0C) under a shared `rtd129x_crt_lock` and deasserts
  `SATA_n`/`SATA_PHY_n` (not just `SATA_PHY_POW_n`) per port -- none of
  which this board's driver does. Not a missed fix: this board's
  `SATA_n`/`SATA_PHY_n`/clock-gate bits are already correct by the time
  Linux boots (confirmed via `devmem` -- see git history), unlike Duo's
  cold eMMC-boot path where they aren't. Both versions are independently
  proven on their own real hardware; rewriting this one to match would
  touch a boot-critical driver with no bug driving the change, so it's
  being kept as-is rather than unified for source-parity's own sake.
- `drivers/phy/realtek/phy-rtk-sata.c` — SATA PHY, already written
  against the modern generic PHY framework (`devm_phy_create()`) in
  the vendor tree, so this forward-port only needed API-surface
  updates, not a rewrite. Also opens the SB2 bus gate at the top of
  `.init()` (via `phy_rtk_sata_sb2_gate_open()`), not just in
  `.power_on()` — a no-op here in practice (this board's bootloader
  already has the gate open by the time Linux runs), backported from
  symops/pelican-6.18 for byte-for-byte `phy-rtk-sata.ko` parity
  between the two ports (confirmed: identical size and md5) after a
  real source diff turned up the difference. See that repo's
  README.md for why Duo actually needs this call (its
  bootloader leaves the gate closed on a cold boot, unlike this
  board's). **Confirmed on real hardware, no regression**: clean boot,
  `init phy0 OK` with no `mdio busy` stalls, `ata1: SATA link up 3.0
  Gbps`, drive identified/partitioned, full boot to shell — as
  expected, since the gate was already open here.
- `drivers/mtd/spi-nor/controllers/rtk-sfc.c` (new) — the boot SPI-NOR
  controller, forward-ported to `spi_nor_controller_ops`. Its RDID
  register read is the one genuinely hardware-quirky part: the whole
  (up to 4-byte) JEDEC ID response is latched by a single pulse of
  `SFC_CTL` into one 32-bit word at a fixed IO address, not one byte
  per read/pulse as a naive port of the vendor's `memcpy`-based
  `read_reg()` would suggest — confirmed by hand via `devmem` after
  three different byte-wise read strategies all came back identical
  and wrong (`ef ef ef ef ef ef` instead of the real `ef 40 14`).

### RTC

- `drivers/rtc/rtc-wd-mch-flash.c` (new) — this board has no
  battery-backed hardware RTC. The vendor firmware persists the clock
  as 8 bytes (seconds + inverted-checksum) in a dedicated area of the
  boot SPI-NOR instead; this driver reads/writes the same on-flash
  format as the vendor's `rtc-rtk.c`, so the timestamp stays valid
  across a dual-boot back to stock firmware. Registers under a new
  `wd,mycloud-home-rtc` compatible rather than binding to
  `realtek,rtd1295-rtc`: the real hardware RTC counter at that
  register offset free-runs from an arbitrary reset value with no
  battery backup and is useless for keeping actual time (this is why
  mainline's `rtc-rtd119x.c`, which drives that counter directly,
  always reports 2014-01-01 on this board) — the vendor firmware
  never uses it either. Periodic write-back to flash needs no extra
  code: it's the kernel's ordinary `CONFIG_RTC_SYSTOHC` (NTP → RTC
  sync, ~every 11 minutes once synced), already enabled and pointed at
  `rtc0`.

### PWM / front LED

- `drivers/pwm/pwm-rtd129x.c` (new) — forward-ported from the vendor's
  `pwm-rtk.c` to `pwm_ops.apply()`/`.get_state()`. The vendor driver's
  ~700 lines of custom per-channel sysfs attributes are dropped —
  mainline already exposes the same controls generically under
  `/sys/class/pwm/` — keeping only the register math and probe core.
  `get_state()` reads live hardware registers rather than mirroring a
  software-shadow array, because `leds-pwm`'s `default-state = "keep"`
  calls `get_state()` once at probe to seed the LED's initial
  brightness from whatever is already running.
- Board DTS front SYS LED default: `"keep"` turned out to be the wrong
  choice for this channel. The raw pre-driver register state is reset/
  BootROM leftovers (~1.6 Hz, visibly flickering), not anything the
  boot loader meaningfully configured — the vendor driver
  unconditionally reprograms all 4 channels at probe regardless of
  what it finds there. Its logged defaults for this channel
  (`clksrc_div=1 clkout_div=255 duty_rate=10`, ~26367 Hz) are the real
  known-good operating point, so the DTS now uses
  `default-state = "on"` with an explicit `default-brightness`
  reproducing that duty cycle.

### CPU clock / cpufreq

- `drivers/clk/realtek/clk-rtd129x-scpu.c` (new) — a single-purpose
  driver for just the one clock cpufreq needs (the CPU PLL), rather
  than a port of the vendor's generic `clk_pll_div`/mux/gate framework
  for the SoC's whole CRT clock tree (~1600 lines across the vendor's
  `clk-pll.{c,h}` + `common.{c,h}`, none of it needed for this one
  clock). Both lookup tables (PLL N/F "SSC1" register values,
  post-divider values) are exact copies of the vendor's; the
  anti-glitch divider/PLL write ordering is kept; the vendor's
  `val==1` workaround is dropped as dead code for this specific
  divider table (never has a value of 1); there is no
  `.enable`/`.disable`/`.is_enabled` because the vendor's own gating
  hooks are no-ops for this particular clock (no `pow_loc`
  configured) — it is the CPU's own clock, `CLK_IGNORE_UNUSED` in the
  vendor tree too.
- Board DTS `scpu_clk` node has no `reg`: the divider register (CRT
  offset `0x030`) and the PLL/SSC1 registers (`0x500+`) sit far apart
  in the same block, so the driver reaches both through `&crt`'s
  syscon regmap instead of a private MMIO sub-window that could only
  cover one of them.
- Three board OPPs in the DTS (300375/600750/1100000 kHz) match the
  vendor's boot-time PLL setting and its double/half.

### IRQ mux

- `drivers/irqchip/irq-rtd129x.c` — this SoC's has more interrupt
  sources than GIC lines, multiplexed through a vendor IRQ-mux block
  (adopted from the community port). One additional fix on top: its
  `set_affinity` delegated to the parent GIC IRQ but never recorded
  the result on the muxed (virtual) `irq_data` itself, so genirq
  warned `"did not update eff. affinity mask"` for every muxed IRQ
  (e.g. the UART's) whenever affinity was set. Fixed by recording the
  parent's effective affinity on the muxed IRQ too, since all IRQs on
  one mux instance share that single parent GIC line anyway.

### Everything else from the community port, adopted as-is

SMP bring-up, GMAC Ethernet (`r8169soc.c` — note the driver name; the
matching Kconfig symbol is gated behind `ARCH_RTD129x`, a Kconfig trap
worth knowing about if it ever looks unselectable), USB dwc3 + PHY,
watchdog, thermal, I2C, and the MFD/regulator stack for the board's
I2C PMIC. The I2C/regulator stack builds and is wired into the DTS but
does not currently probe successfully — not investigated further since
nothing on this board depends on it (the vendor boot log shows it
initializing, but the board runs fine without it).

## Progress log

### Cosmetic poweroff driver (`drivers/power/reset/wdmc-poweroff.c`) — LED, HDD, USB VBUS

Neither mainline nor the vendor 4.9.330 source implements a real
hardware power-off for this board (no board-level 12V power-hold GPIO
exists in either board's vendor DTS), so `halt`/`poweroff` used to just
park the CPU with the board still fully powered: SYS LED lit, USB VBUS
live, disks still spinning. The old board DTS carried a
`realtek,rtd129x-coolboot-poweroff` node left over from the community
port, but that compatible string matches no driver anywhere, mainline
or vendor — confirmed by grepping both GPL source drops directly. New
`wdmc-poweroff.c` driver (shared verbatim with symops/pelican-6.18)
quiets everything actually under this SoC's control at shutdown time:

- **SYS LED**: turns off the PWM channel's OCD register directly (same
  effect as `pwm_disable()`, reached by a raw `devm_ioremap()` poke
  since the `pwm@d0` block is already exclusively owned by the real
  `pwm-rtd129x.c` driver).
- **HDD spin-down**: already worked via the existing SCSI
  `manage_shutdown` sysfs attribute (enabled via udev at boot) — not
  reimplemented, just confirmed working alongside the new driver.
- **USB VBUS**: the hard part. A misc-gpio raw-MMIO poke matching what
  a rescue initramfs's own boot-time VBUS-enable step does was tried
  first and *exhaustively disproven* on real Duo hardware (every bit of
  misc-gpio, both 32-bit banks, and every `rtk_iso_gpio` line swept
  with zero effect) — until it turned out a brief 1-second hold on a
  candidate line shows no effect at all even on a genuinely correct
  line; the actual working duration is several seconds. The real fix
  only came from reading the vendor's own `rtk_usb_manager.c` driver
  against a captured **stock-firmware boot log from this exact
  physical unit** (`mch-debian-4.2.2.log`) rather than the generic
  reference-board DTS, which lists a different, incomplete GPIO set
  than retail firmware actually uses. Confirmed assignment for this
  board: `misc-gpio` bit 19 (port0, raw MMIO — misc-gpio has no
  mainline gpiolib controller in this port) and `rtk_iso_gpio` line 1
  (port1+port2, sharing one physical USB port, a normal gpiod
  consumer). **Confirmed on real hardware.**

**Status: confirmed working, both boards** (Duo's own two ports are on
different `rtk_iso_gpio` lines entirely — see symops/pelican-6.18's
README for that board's assignment and the full discovery
methodology).

### Base version bump: v6.18.46 → v6.18.51

Rebased onto the latest upstream stable point release following this
file's "Updating the base version" procedure (single-parent
`commit-tree`, upstream tags fetched under non-colliding aliases). The
`v6.18.50 → v6.18.51` merge was clean — no conflicts, none of this
port's own files touched by the upstream delta. Rebuilt
`Image`/`dtbs`/`modules` with `LOCALVERSION=` and repackaged; **confirmed
booting and working on real hardware** (alongside the poweroff driver
above, same test).

### Base version bump: v6.18.51 → v6.18.52, and the CPU0-hang workaround update

Rebased onto v6.18.52 the same way — clean merge, no conflicts. On
Duo, this bump made the shared CPU0-interrupt-loss hang (see
symops/pelican-6.18's README.md, "Base version bump: v6.18.45 →
v6.18.46") reproduce on every boot instead of intermittently. A full
bisection there (see pelican-6.18's README.md, "Base version bump:
v6.18.51 → v6.18.52, and the CPU0-hang regression") traced this to an
upstream commit whose only effect on either board is an incidental
~4 KiB shift in kernel image layout — the underlying race itself
predates this bump and was never root-caused, on either board.

The previous delay+counter workaround (`usleep_range()` in
`kernel/dma/direct.c`'s `dma_direct_alloc()`, plus a counter in
`kernel/sched/core.c`'s `__resched_curr()`) stopped being effective at
v6.18.52's layout on Duo. Replaced with 100 no-op instructions right
before `do_idle()`'s idle loop (`kernel/sched/idle.c`) — same
empirical, layout-perturbation-only nature as what it replaces, not an
understood fix. Ported here identically and, unlike the delay+counter
workaround this port carried for months without ever seeing the bug
directly, **this time confirmed working on real Monarch hardware**
(back-to-back clean boots), not just applied as a defensive port.
Negligible cost: ~100 cycles once per `do_idle()` entry, idle path
only.

## Building and booting

This board's U-Boot (`2015.07`, `Realtek QA Board`, 2016 build) has two
requirements that are very easy to miss and produce total, silent boot
failure — no console output, nothing — if skipped:

1. **`arch/arm64/boot/Image`'s header must be patched** before
   packaging. This loader copies the Image to the address given by its
   own header `text_offset` field; a normal relocatable Image has
   `text_offset=0`, i.e. "copy the kernel on top of itself at physical
   address 0". Run `tools/monarch/patch-header.py arch/arm64/boot/Image`
   on every build before flashing/testing. (This was mistaken for a
   kernel-image-size limit for a long time before the real cause was
   found — several rounds of shrinking the kernel "fixed" it only by
   accident, by changing unrelated build parameters alongside it.)
2. **Package the patched Image with gzip (`pigz -11`).** This was
   avoided for a long time on the theory that the loader's built-in
   gzip decompression was unreliable above a few MB (`inflate()`
   buffer errors, or silent hangs) — but that theory has since been
   confirmed wrong on real hardware at the sizes this port actually
   produces, and this is now the standard packaging step (see
   `tools/monarch/patch-header.py`'s docstring).

The safe way to test any of this without touching the board's internal
storage: this loader has a `boot_rescue_from_usb` path (triggered by
the physical USB-Install button, or automatically when no factory
partition is found) that reads fixed filenames from a GPT/FAT USB
stick — `rescue.sata.dtb`, `sata.uImage` (despite the name, no
mkimage/FIT wrapping — just the raw patched `Image` + padding, see
above), and either a combined `CONFIG_INITRAMFS_SOURCE`-baked initramfs
or a separate `rescue.root.sata.cpio.gz_pad.img` (gzip'd cpio,
zero-padded to exactly 4194304 bytes regardless of real payload size —
the loader reads that fixed block size unconditionally). Nothing on
the board's own flash/disk is touched by this path.

The `usb-payload/` directory packaged for deployment also carries two
files the rescue loader itself never reads, kept alongside for
convenience when deploying the same build onto the full installed OS:
`modules.tar.xz` (tarred from *inside* `INSTALL_MOD_PATH/lib/modules/`,
i.e. `cd .../lib/modules && tar -cJf modules.tar.xz .`, so the archive
root is `./6.18.4X+/...` — extracting with `tar -C /lib/modules -xf
modules.tar.xz` lands the version directory directly at the right
path) and `.config` (the exact `.config` this build was made from).

`initramfs/` (the `CONFIG_INITRAMFS_SOURCE` this tree's `.config` points
at) is this rescue userspace: a minimal set of prebuilt static
aarch64 binaries (`busybox`, `mdadm`, `libc`/`ld-linux`) plus the
`init` script, originally from `Fireblossom/wd-mch-kernel`'s own
initramfs (adopted like the rest of that community port, see above),
with this port's own storage-module load-order/timing fixes on top
(see below). `initramfs/lib/modules/*.ko` (the three modules `init`
`insmod`s by hand) are **not** committed — they're kernel-version-tied
build output, not source; run `tools/monarch/sync-storage-modules.sh`
after `make modules` and before `make Image` to (re)populate them from
the just-built tree, every time, including after a base-version rebase
(see "Updating the base version" below).

Three of this board's drivers — `phy-rtk-sata`, `usb-storage`, `uas` — are
built as modules rather than built-in (see "Config" below), so the
initramfs `init` script `insmod`s them explicitly by path before touching
any `/dev/sd*` device; there's no `modprobe`/`depmod` in this minimal
initramfs, so load order matters (`phy-rtk-sata` before `usb-storage`
before `uas`, since `uas` depends on `usb-storage`). Because the SoC's
`ahci_rtd1295` glue driver only finishes probing (and SATA link-up
negotiation) after `phy-rtk-sata` is inserted, and this board's drive
reliably takes several seconds to link, the `init` script also waits (up
to 12s, polling `/sys/bus/scsi/devices/{1,2}:0:0:0` — `ahci_rtd1295`'s
fixed SCSI host numbers, not tied to which `/dev/sd?` letter the kernel
assigns) before scanning for a root filesystem; without this wait every
boot fell through to Network Rescue Mode before the disk was even
attached. **Never `strip -s` (strip-all) a `.ko` that needs to actually
load** — it removes the symbol table the kernel's module loader parses to
resolve relocations, so `insmod` fails with `EINVAL`/"invalid module
format" even though the file looks fine otherwise; `strip --strip-debug`
only drops debug sections and keeps modules loadable.

## Config

Compared to a bare `defconfig`, this port's `.config` also mirrors
`monarch-4.9.330`'s module/built-in choices where applicable (matching
features enabled there, and building as modules whatever it built as
modules), plus a broad pass converting non-driver, non-boot-critical
`=y` features to `=m` to shrink the built-in `Image`. A few things must
stay built-in no matter what that pass's heuristics say, because the
initramfs needs them before any module could be loaded: `EXT4_FS` (+
`JBD2`, `FS_MBCACHE`), `VFAT_FS`, `NLS*`, the `DECOMPRESS_*`/`ZLIB_*`
codecs, `CRC16`/`CRC32`, `PSTORE*`, `XZ_DEC` — and, learned the hard way,
`BINFMT_SCRIPT` (needed to exec `/init`'s own `#!/bin/sh` line — without
it the kernel panics with `Failed to execute /init (error -8)`) and
`PACKET` (`AF_PACKET`, needed by `udhcpc` to send a `DHCPDISCOVER` at
all — without it the initramfs's own network-rescue DHCP client fails
immediately with `EAFNOSUPPORT`, before any module could be inserted to
provide it).

**Docker/container support**: checked against upstream Docker's own
`contrib/check-config.sh` requirements list. The baseline config
already covered essentially everything in the "required" tier
(namespaces, cgroups v1 controllers, veth/bridge/netfilter, NAT,
POSIX_MQUEUE, etc.) and most of the nftables family already too
(`NF_TABLES`, `NF_TABLES_INET`, `NFT_NAT`/`MASQ`/`REDIR`/`COMPAT`/
`REJECT*`/`CT`/`LOG`/`LIMIT`/`HASH`/`NUMGEN`, all `=m`) — only a
handful of genuinely-missing, genuinely-available options needed
adding: `CGROUP_PERF` (bool, `=y`), `BTRFS_FS_POSIX_ACL` (bool, `=y`,
`BTRFS_FS` was already `=m`), and as modules matching the existing
`NF_TABLES=m` pattern: `NFT_FIB_IPV4`, `NFT_FIB_IPV6`, `NFT_FIB_INET`,
`NFT_QUOTA`, `NFT_CONNLIMIT`, `IP_SCTP`. (A few other items the script
checks -- `SECURITY_SELINUX`/`SECURITY_APPARMOR`, `DEVPTS_MULTIPLE_
INSTANCES`, `IOSCHED_CFQ`, `NETPRIO_CGROUP` -- are either genuinely out
of scope for this minimal embedded config or no longer exist as
separate options on this kernel version; `CGROUP_NET_PRIO` already
covers the modern equivalent of `NETPRIO_CGROUP`.) Not yet tested with
an actual Docker/container workload on real hardware.

## Updating the base version

To rebase this port onto a newer upstream point release (e.g.
`v6.18.46` → `v6.18.50`), do a 3-way merge with the port's most
recently merged-in point release as the explicit merge base — this
tree's initial commit is a content-squashed root with no real git
parent link into upstream history, so a plain `git merge`/`git rebase`
can't infer the right base on its own.

**Step 0 — fetch the real upstream tags under aliases, never bare.**
Both this repo and the sibling `symops/pelican-6.18` have, at various
points, moved their own local `vX.Y.Z` tag to point at a *release*
commit of their own (for GitHub Releases) — meaning the bare tag name
`v6.18.46` in this repo's local clone does **not** point at the real
upstream `v6.18.46` commit, it points at one of our own commits with
that same name reused. Trusting it as `--merge-base` silently produces
a nonsense diff (every file this port ever touched looks "deleted" in
the new upstream tag, because `git merge-tree` is diffing against our
own tree instead of upstream's). Always fetch upstream tags under a
name that cannot collide, and verify before using one:

```
git fetch --filter=blob:none \
    https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git \
    v6.18.46:refs/tags/upstream-v6.18.46
git cat-file -e upstream-v6.18.46:drivers/phy/realtek/phy-rtk-sata.c \
    && echo "BUG: this is OUR tag, not upstream's" \
    || echo "OK: genuine upstream tree, no board-port files"
```

(use `git.kernel.org`'s stable tree, not `github.com/gregkh/linux` —
the GitHub mirror lags behind by up to one point release and may not
have the newest tag yet.)

**Step 1 — compute and land the merge.**

```
git fetch --filter=blob:none \
    https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git \
    tag v6.18.50
TREE=$(git merge-tree --write-tree --merge-base=upstream-v6.18.46 HEAD v6.18.50^{commit})
git commit-tree -p HEAD -m "Merge upstream v6.18.50" $TREE
git reset --hard <resulting-commit>
```

Note the single `-p HEAD` — **do not** also pass `-p v6.18.50^{commit}`
as a second parent, even though that is what a normal `git merge`
commit would record and what an earlier version of this section
recommended. Actually attaching the real upstream commit as a git
parent links this repo's history into upstream's, and pushing that
choked outright on both this repo and pelican-6.18 with `remote: fatal:
did not receive expected object <sha>` / `index-pack failed` — traced
to a phantom object in the local (partial-clone) object store that
was reachable from no ref and present in no local pack, yet kept
getting pulled into the push by git's own pack-generation logic once
upstream's real ancestry was reachable from the commit being pushed.
`git backfill`, `--refetch`, `--no-thin`, and `verify-pack` all failed
to resolve or route around it. A single-parent commit produces the
exact same tree/content (this repo's own design already never carries
real upstream git history anyway — see `symops/MCG1-6.18`'s README,
"squashed baseline commit importing linux-stable" — so not literally
attaching the upstream commit as a parent is arguably *more*
consistent with that design, not less) and only ever needs to push the
actual content delta, never upstream's ancestry graph.

**Important for every rebase *after* the first**: `--merge-base` must
be the point release most recently merged in (e.g. `v6.18.46` when
rebasing onto `v6.18.50`, fetched under its own `upstream-v6.18.46`
alias per Step 0), not the port's original base tag again — reusing an
older base tag makes `git merge-tree` try to replay the entire
upstream delta since that point a second time against a tree that
already contains it, producing hundreds of bogus conflicts in files
this port never touches.

**Step 2 — rebuild and re-sync everything that bakes in the version
string.** A version bump always changes `UTS_RELEASE`, so anything
built before it (`Image`, modules, the vermagic strings inside them)
is now stale and must be rebuilt together, or `insmod` will reject the
old ones with a vermagic mismatch. Also pass `LOCALVERSION=` (empty,
but explicitly set) on every `make` invocation below: without it,
`scripts/setlocalversion` appends a bare `+` to the release string for
any commit that isn't itself exactly a tagged upstream release —
i.e. always, for this port — producing kernel/module strings like
`6.18.50+` instead of the clean `6.18.50` this project wants (see
`scripts/setlocalversion`'s own `LOCALVERSION` handling for why: it
only suppresses the `+` when the make variable is *set*, even to
empty, not when `CONFIG_LOCALVERSION_AUTO` happens to be off).

```
make LOCALVERSION= Image modules dtbs
tools/monarch/sync-storage-modules.sh
make LOCALVERSION= modules_install INSTALL_MOD_PATH=<staging dir>
```

`sync-storage-modules.sh` refreshes `initramfs/lib/modules/*.ko`
*before* the next `make Image` in this same invocation (order matters
— the initramfs is baked into `Image` at that step); the separate
`rescue.root.sata.cpio.gz_pad.img` initrd, if that path is also used,
needs re-packing from the same now-current `initramfs/` tree too.
