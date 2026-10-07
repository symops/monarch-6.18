# Building the MCH/MCHD (WD My Cloud Home / My Cloud Home Duo) kernel from a clean Debian 13

This describes building **Monarch** (MCH, single-bay, RTD1295, repo `symops/monarch-6.18`) or **Duo** (MCHD, dual-bay, RTD1296, repo `symops/pelican-6.18`) from scratch on a fresh Debian 13 ("trixie") machine. Both ports share the same workflow; differences are called out where they occur.

## 1. Install the toolchain and build dependencies

```sh
sudo apt update
sudo apt install -y \
    git make bc bison flex \
    gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
    libssl-dev libelf-dev \
    python3 cpio gzip pigz kmod rsync
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
gzip -9 -f -k -c /tmp/rescue.root.cpio > /tmp/rescue.root.cpio.gz
python3 -c "
data = open('/tmp/rescue.root.cpio.gz', 'rb').read()
target = 4194304
assert len(data) <= target, f'{len(data)} > {target}'
open('usb-payload/rescue.root.<emmc|sata>.cpio.gz_pad.img', 'wb').write(
    data + b'\0' * (target - len(data)))
"
```

The bootloader's rescue path reads this file as a **fixed 4,194,304-byte block**, regardless of real payload size — the zero-padding to that exact length is mandatory, not cosmetic.

## 9. Install and package the modules

```sh
rm -rf /tmp/modinstall && mkdir -p /tmp/modinstall
make -j"$(nproc)" LOCALVERSION= modules_install INSTALL_MOD_PATH=/tmp/modinstall
( cd /tmp/modinstall/lib/modules && tar -cJf usb-payload/modules.tar.xz . )
cp .config usb-payload/.config
```

Archiving from *inside* `lib/modules/` (not with `lib/modules/` as a prefix) matters: it makes the tar root `./<kernelrelease>/...`, so `tar -C /lib/modules -xf modules.tar.xz` on the target lands the version directory at the correct path directly.

## 10. Deploy for testing

Put `emmc.uImage`/`sata.uImage`, `rescue.emmc.dtb`/`rescue.sata.dtb`, and `rescue.root.*.cpio.gz_pad.img` with those **exact filenames** on a GPT/FAT-formatted USB stick. Triggering the bootloader's rescue-boot path (physical USB-Install button, or automatically if no factory partition is found) boots entirely from the stick — nothing on the unit's own storage is touched. `modules.tar.xz`/`.config` aren't read by the rescue loader itself; they're for deploying the same build onto a full installed OS afterward.

## 11. Flashing for a normal (non-rescue) boot: updating the firmware checksum table

Section 10's USB rescue-boot path never touches the unit's own storage — it's the
safe way to test a build. To make a build be the one the bootloader loads on a normal,
no-USB-stick boot, the kernel/dtb/rootfs images have to be written to their actual
partitions on the unit's internal storage (eMMC or SATA), **and** the bootloader's
firmware table — a small on-disk structure holding, per slot, a type, size, load
address and **checksum** — has to be updated to match. The bootloader checksums
each slot against this table before loading it; a plain `dd` of a new kernel onto
its partition without also updating the table gets rejected (or the old image
silently keeps booting). Two different vendor command-line tools do this update,
one per board family — both are tiny, closed-source, prebuilt binaries (not
built from anything in this tree), kept at `tools/monarch/fwtablectl` (Monarch)
and `tools/monarch/fwtutil` (Duo) for convenience.

**This writes directly to raw partitions / the raw eMMC device on the unit's own
storage.** A wrong device node, wrong partition number, or wrong firmware-type
name silently corrupts or bricks that slot — there is no confirmation prompt.
Always prove a build boots via the section 10 USB rescue path first, and
double-check the target device/partition numbers against the actual unit before
running any of this.

### Monarch (`fwtablectl`)

Monarch keeps the firmware table on its own partition (`/dev/sdb1` below),
separate from the raw data partitions each slot's image lives on. Updating a
slot is therefore two steps: `dd` the new image onto every partition that slot
occupies, then tell `fwtablectl` (operating on the table partition) to recompute
and store that slot's new size/checksum from the same file:

```sh
dd if=/root/sata.uImage of=/dev/sdb2
dd if=/root/sata.uImage of=/dev/sdb8
dd if=/root/rescue.sata.dtb of=/dev/sdb5
dd if=/root/rescue.sata.dtb of=/dev/sdb6
dd if=/root/rescue.root.sata.cpio.gz_pad.img of=/dev/sdb3
dd if=/root/rescue.root.sata.cpio.gz_pad.img of=/dev/sdb4

/root/fwtablectl firmware update /dev/sdb1 Kernel /root/sata.uImage
/root/fwtablectl firmware update /dev/sdb1 RescueKernel /root/sata.uImage
/root/fwtablectl firmware update /dev/sdb1 KernelRootFS /root/rescue.root.sata.cpio.gz_pad.img
/root/fwtablectl firmware update /dev/sdb1 RescueRootFS /root/rescue.root.sata.cpio.gz_pad.img
/root/fwtablectl firmware update /dev/sdb1 KernelDeviceTree /root/rescue.sata.dtb
/root/fwtablectl firmware update /dev/sdb1 RescueDeviceTree /root/rescue.sata.dtb
```

`sata.uImage` here is this guide's patched `Image` (packaged by section 7),
`rescue.sata.dtb` is section 7's device tree, and
`rescue.root.sata.cpio.gz_pad.img` is section 8's padded rescue initrd — each is
written to *two* partitions (the normal-boot slot and the rescue-mode slot) and
the table is updated for both corresponding firmware types.
`fwtablectl firmware types` lists every type name the table accepts;
`fwtablectl show /dev/sdb1` dumps the current table for inspection before/after.

### Duo / Pelican (`fwtutil`)

Duo keeps the table and the firmware data together on the single eMMC device
(`/dev/mmcblk0`), addressed by numeric slot index rather than by type name, and
`-r` writes the file into that slot *and* updates its table entry (checksum
included) in one step — no separate `dd` needed:

```sh
/root/fwtutil -r 5:/root/emmc.uImage -r 12:/root/emmc.uImage \
        -r 8:/root/rescue.root.emmc.cpio.gz_pad.img -r 9:/root/rescue.root.emmc.cpio.gz_pad.img \
        -r 6:/root/rescue.emmc.dtb -r 7:/root/rescue.emmc.dtb
```

Indices 5/12 are the normal-boot and rescue kernel slots (`emmc.uImage` from
section 7), 8/9 the normal-boot and rescue rootfs slots
(`rescue.root.emmc.cpio.gz_pad.img`, section 8's padded rescue initrd), and 6/7
the normal-boot and rescue device-tree slots (`rescue.emmc.dtb` from section 7).
`fwtutil -l` lists the firmware table, `fwtutil -p` lists partitions, `-x n:file`
extracts a slot's current content back out to a file (useful for taking a
backup of a slot before overwriting it).
