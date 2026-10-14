# Building the MCH/MCHD (WD My Cloud Home / My Cloud Home Duo) kernel from a clean Debian 13

This describes building **Monarch** (MCH, single-bay, RTD1295, repo `symops/monarch-6.18`) or **Duo** (MCHD, dual-bay, RTD1296, repo `symops/pelican-6.18`) from scratch on a fresh Debian 13 ("trixie") machine. Both ports share the same workflow; differences are called out where they occur.

## 1. Install the toolchain and build dependencies

```sh
sudo apt update
sudo apt install -y \
    git make bc bison flex \
    gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
    libssl-dev libelf-dev \
    python3 cpio gzip pigz xz-utils kmod rsync
```

No `u-boot-tools`/`mkimage` is needed — despite the `.uImage` filename, this board's bootloader does **not** use the mkimage/FIT format; it's a raw patched `Image`. Device-tree compiler is also not required system-wide: the kernel build tree compiles its own `scripts/dtc` from source.

## 2. Clone the repository

```sh
git clone git@github.com:symops/monarch-6.18.git   # or pelican-6.18.git for Duo
cd monarch-6.18
```

## 3. Get a `.config`

`.config` is **not tracked in git** (board config is build output, not source). Download `default.config` from the latest GitHub release and rename it:

```sh
gh release download --repo symops/monarch-6.18 --pattern 'default.config' -O .config
# or just fetch the raw asset URL with curl/wget if you don't have gh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
```

`olddefconfig` resolves any new Kconfig symbols introduced since that release against their defaults — always run it after fetching an older `.config`.

### Duo: required `.config` settings

These matter for a Duo (Pelican) build; the release `default.config` plus the following is the verified recipe:

| Symbol | Value | Why |
|---|---|---|
| `CONFIG_PINCTRL_SINGLE` | `m` | I2C0 pinmux for the PMIC (DTS `pinmux@310`) |
| `CONFIG_MFD_G2227_I2C` | `m` | G2227 PMIC core |
| `CONFIG_REGULATOR_G2227` | `m` | PMIC regulators: DVFS (`cpu-supply`) and the power-off rail shutdown |
| `CONFIG_I2C_RTK` | `m` | I2C0 controller |
| `CONFIG_MQ_IOSCHED_DEADLINE` | `y` | otherwise block devices come up with scheduler `none` |
| `CONFIG_RT_GROUP_SCHED` | not set | systemd wants it off |

**The three PMIC drivers must be modules, not built in.** Built in (about +16 KB in `vmlinux`) they bring back the CPU0 hang (`port 0 is not capable of FBS` followed by an RCU stall with CPU0 stuck in `do_idle()`), because the idle-loop nop workaround (`.rept 100` in `kernel/sched/idle.c`) is layout-sensitive and only happens to work for the module layout. Bisected on real hardware: PR #7 alone boots, the four drivers built in hang, PMIC/OPP DTS with the drivers as modules boots. Changing the nop count (100, 1000, 10000) did not help. Do not flip these to `=y` without re-testing a full boot.

Consequences of the modules: the PMIC, DVFS (`cpufreq-dt` stays in deferred probe until `g2227-regulator` loads, about 37 s into boot with udev) and the real power-off (the g2227 driver's coldboot sleep states cut the rails at `shutdown`) only work once the modules are loaded. The rescue initrd does not load them, so `cpufreq-dt: deferred probe pending` there is expected.

**Always pass `ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-` to every `make`, including `olddefconfig`.** A `make` run without `ARCH` configures against the host (x86) Kconfig and silently strips the board options from `.config` (`ARCH_REALTEK`, `IRQ_RTD129X_MUX`, `CMDLINE`, ...): no UART console, `dw-apb-uart: IRQ index 0 not found`, `/init` exits at once. If a boot looks like that, check `grep -E 'ARCH_REALTEK|IRQ_RTD129X_MUX|^CONFIG_CMDLINE=' .config` and re-fetch `default.config`.

## 4. Build the kernel, modules, and device trees

```sh
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-

make -j"$(nproc)" LOCALVERSION= Image modules dtbs
```

`LOCALVERSION=` is explicit and required — without it, `scripts/setlocalversion` appends a stray `+` to the kernel release string (from the lack of a clean git tag match), which breaks module vermagic matching against the packaged `modules.tar.xz`.

Check the resulting version string:
```sh
cat include/config/kernel.release
```

## 5. Re-sync the initramfs's hand-loaded storage modules

The minimal rescue initramfs `insmod`s three modules by hand (`phy-rtk-sata.ko`, `usb-storage.ko`, `uas.ko`). Since `CONFIG_INITRAMFS_SOURCE=""` (the initrd is built and packaged separately from `initramfs/`, not embedded into `Image` — see step 8), these `.ko`s live under `initramfs/lib/modules/` in the source tree and must be refreshed from the modules just built in step 4, or their vermagic will mismatch what the rescue initrd actually loads:

```sh
tools/monarch/sync-storage-modules.sh
```

(Yes, this path is literally `tools/monarch/...` in **both** repos — a historical naming artifact, not a typo.) This only needs to happen before the `initramfs/` directory gets packaged into a `cpio` in step 8 — it has no effect on `Image` itself, since `Image` never embeds `initramfs/`. If you later change anything under `kernel/`/`drivers/` and re-run `make modules`, re-run this script before repackaging the initrd.

## 6. Patch the Image header

```sh
python3 tools/monarch/patch-header.py arch/arm64/boot/Image
```

This board's U-Boot (2015.07, 2016 vintage) copies the loaded Image to the physical address given by the image header's own `text_offset` field. A normal relocatable arm64 `Image` has `text_offset=0` — i.e. "copy on top of itself at address 0" — which is silent, total, undebuggable boot failure (no console output at all). The script rewrites `code0`, `text_offset` (to `0x200000`), and `pe_offset` in the Image header in place. **Run this on every rebuilt Image, before packaging it.**

## 7. Package the boot image

The packaged files go into `usb-payload/` (steps 7-9), which is not in the repository: create it first.

```sh
mkdir -p usb-payload
```

**Duo:**
```sh
cp arch/arm64/boot/Image arch/arm64/boot/Image.noinitramfs
pigz -11 -k -f -c arch/arm64/boot/Image.noinitramfs > usb-payload/emmc.uImage
cp arch/arm64/boot/dts/realtek/rtd1296-wd-mycloud-home-duo.dtb usb-payload/rescue.emmc.dtb
```

**Monarch:**
```sh
pigz -11 -k -f -c arch/arm64/boot/Image > usb-payload/sata.uImage
cp arch/arm64/boot/dts/realtek/rtd1295-wd-mycloud-home.dtb usb-payload/rescue.sata.dtb
```

Despite the `.uImage` name, this is just the patched, gzip'd raw `Image` — no FIT/mkimage wrapping. Always gzip the *patched* Image, never the unpatched one.

## 8. Build the separate rescue initrd

`CONFIG_INITRAMFS_SOURCE=""` — the initrd is **not** baked into `Image`; it's packaged separately from the `initramfs/` directory in the repo (a minimal static-`busybox`/`mdadm` rescue userspace):

```sh
( cd initramfs && find . | cpio -o -H newc > /tmp/rescue.root.cpio )
pigz -11 -f -k -c /tmp/rescue.root.cpio > /tmp/rescue.root.cpio.gz
python3 -c "
data = open('/tmp/rescue.root.cpio.gz', 'rb').read()
target = 4194304
assert len(data) <= target, f'{len(data)} > {target}'
open('usb-payload/rescue.root.<emmc|sata>.cpio.gz_pad.img', 'wb').write(
    data + b'\0' * (target - len(data)))
"
```

`initramfs/bin/` carries a symlink to `/bin/busybox` for every applet of the bundled static busybox (`busybox --list`, ~400 of them). The init script and the rescue shell rely on them (`insmod`, `grep`, `sed`, `ip`, `udhcpc`, `telnetd`, ...): this busybox is not built with `FEATURE_SH_STANDALONE`, so without the links they fail with `not found`. After replacing `bin/busybox` with a build that has a different applet set, re-create the links with:

```sh
cd initramfs/bin && for a in $(qemu-aarch64 ./busybox --list); do [ -e "$a" ] || [ "$a" = busybox ] || [ "$a" = init ] || ln -s /bin/busybox "$a"; done
```

(use `./busybox --list` directly on an aarch64 host).

In the `open(...)` line replace `<emmc|sata>` with `emmc` for a Duo or `sata` for a Monarch (the file name must be exactly `rescue.root.emmc.cpio.gz_pad.img` or `rescue.root.sata.cpio.gz_pad.img`).

The bootloader's rescue path reads this file as a **fixed 4,194,304-byte block**, regardless of real payload size — the zero-padding to that exact length is mandatory, not cosmetic.

## 9. Install and package the modules

```sh
rm -rf /tmp/modinstall && mkdir -p /tmp/modinstall
make -j"$(nproc)" LOCALVERSION= modules_install INSTALL_MOD_PATH=/tmp/modinstall
KDIR="$PWD"
( cd /tmp/modinstall/lib/modules && tar -cJf "$KDIR/usb-payload/modules.tar.xz" . )
cp .config usb-payload/.config
```

`KDIR` keeps the kernel tree's path, because the `tar` runs from another directory and a relative `usb-payload/...` destination would not exist there. Archiving from *inside* `lib/modules/` (not with `lib/modules/` as a prefix) matters: it makes the tar root `./<kernelrelease>/...`, so `tar -C /lib/modules -xf modules.tar.xz` on the target lands the version directory at the correct path directly.

## 10. Deploy for testing

Put `emmc.uImage`/`sata.uImage`, `rescue.emmc.dtb`/`rescue.sata.dtb`, and `rescue.root.*.cpio.gz_pad.img` with those **exact filenames** on a GPT/FAT-formatted USB stick. Triggering the bootloader's rescue-boot path (physical USB-Install button, or automatically if no factory partition is found) boots entirely from the stick — nothing on the unit's own storage is touched. `modules.tar.xz`/`.config` aren't read by the rescue loader itself; they're for deploying the same build onto a full installed OS afterward.

## 11. Flashing for a normal (non-rescue) boot: updating the firmware checksum table

Section 10's USB rescue-boot path never touches the unit's own storage — it's the
safe way to test a build. To make a build be the one the bootloader loads on a normal,
no-USB-stick boot, the kernel/dtb/rootfs images have to be written to their actual
slots on the unit's internal storage (eMMC or SATA), **and** the bootloader's
firmware table — a small on-disk structure holding, per slot, a type, size, load
address and **checksum** — has to be updated to match. The bootloader checksums
each slot against this table before loading it; a plain `dd` of a new kernel onto
its partition without also updating the table gets rejected (or the old image
silently keeps booting).

Both boards use the same table format, and one tool handles both:
**[verona-tool](https://github.com/symops/verona-tool)**. It writes a slot's data,
recalculates the checksum, rewrites the table and reads everything back to verify it,
replacing the two vendor tools (`fwtablectl` for Monarch, `fwtutil` for Duo) that used
to be kept in this tree. Download the `verona-tool` binary (linux/arm64, dynamically
linked against glibc only, so it runs in the rescue environment and on a full Debian)
from the [latest release](https://github.com/symops/verona-tool/releases/latest), copy
it to the unit and `chmod +x` it. To build it yourself see that repository's
`BUILDING.md`; its `README.md` has the full command-line reference.

**This writes directly to the unit's own storage.** A wrong `--device`, a wrong platform
or a wrong file silently corrupts or bricks that slot — there is no confirmation prompt.
Always prove a build boots via the section 10 USB rescue path first, and double-check
the target device against the actual unit before running any of this. Take a backup of
the table first (`--backup=FILE`) and rehearse the exact command with `--dry-run`, which
prints what would be written and writes nothing.

### Monarch

On Monarch pass the **whole disk** as `--device` (`/dev/sdX` — the unit's SATA disk, check
with `lsblk`; it is required for this board): `verona-tool` reads the GPT, finds the
firmware-table partition itself and updates each slot in place.

```sh
verona-tool --platform monarch --device /dev/sdX --write --kernel=/mnt/sata.uImage --write --rootfs=/mnt/linux/rootfs1.bin --write --dtb=/mnt/rescue.sata.dtb
```

### Duo / Pelican

On Duo the table and the firmware data live together on the single eMMC device, so
`--device` can be left out (it defaults to `/dev/mmcblk0`):

```sh
verona-tool --platform pelican --write --kernel=/mnt/emmc.uImage --write --rootfs=/mnt/linux/rootfs2.bin --write --dtb=/mnt/rescue.emmc.dtb
```

### What the options do

`sata.uImage`/`emmc.uImage` is section 7's patched, gzip'd `Image` and `rescue.sata.dtb`/
`rescue.emmc.dtb` is section 7's device tree; the root filesystem image is `rootfs1.bin`
(Monarch) or `rootfs2.bin` (Duo) in the layout used above. `--kernel`, `--rootfs` and
`--dtb` are shortcuts for a *pair* of table slots (the normal-boot slot and its rescue
copy): the same file is written into both, and only if it fits both — otherwise neither
is touched. Each shortcut needs its own `--write` in front of it, and the file is
attached with `=` (`--kernel=FILE`).

| Shortcut | Slots (normal / rescue) |
|---|---|
| `--kernel` (`-k`) | `5` Kernel / `12` RescueKernel |
| `--dtb` (`-t`) | `7` KernelDeviceTree / `6` RescueDeviceTree |
| `--rootfs` (`-f`) | `9` KernelRootFS / `8` RescueRootFS |

Useful before and after (add `--device /dev/sdX` on Monarch):

```sh
verona-tool --platform pelican --list                        # print the firmware table
verona-tool --platform pelican --backup=duo-table.img \
    --read 5:kernel-backup.img                               # back up the table and one slot first
verona-tool --platform pelican --dry-run --write --kernel=/mnt/emmc.uImage   # rehearse
verona-tool --platform pelican --restore duo-table.img       # put a saved table back
```

`--read SLOT:FILE` also extracts any slot's current content, which is the way to take a
backup of a slot before overwriting it.
