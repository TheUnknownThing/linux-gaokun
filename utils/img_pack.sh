#!/bin/bash
set -euo pipefail
# =============================================================================
# Filename: img_pack.sh
# Purpose: To be filled
# Usage: sudo ./img_pack.sh
# =============================================================================

# libarchive-tools for bsdtar, qemu-user-static for emulation
# parted for partion raw img, arch-install-scripts for arch-chroot, genfstab
apt update && apt install -y zstd curl libarchive-tools qemu-user-static parted arch-install-scripts

# handle binfmt_misc, https://access.redhat.com/solutions/1985633
if grep -q 'binfmt_misc' /proc/mounts; then
    echo "binfmt_misc mounted"
else
    mount binfmt_misc -t binfmt_misc /proc/sys/fs/binfmt_misc
fi

if [[ -f /proc/sys/fs/binfmt_misc/status ]] && grep -qx 'disabled' /proc/sys/fs/binfmt_misc/status; then
    echo 1 > /proc/sys/fs/binfmt_misc/status
fi

if [[ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ]]; then
    echo "qemu-aarch64 binfmt already registered"
else
    echo ':qemu-aarch64:M::\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/bin/qemu-aarch64-static:FP' > /proc/sys/fs/binfmt_misc/register
fi


# Pick a free loop device instead of assuming a fixed one exists.
LOOP_DEV="$(losetup -f)"
IMAGE_SIZE="${IMAGE_SIZE:-8G}"

# container setup
truncate -s "${IMAGE_SIZE}" archlinuxarm.img
losetup -P ${LOOP_DEV} archlinuxarm.img
parted ${LOOP_DEV} --script mklabel gpt
parted ${LOOP_DEV} --script mkpart EFI fat32 1MiB 301MiB
parted ${LOOP_DEV} --script set 1 boot on
parted ${LOOP_DEV} --script mkpart ALARM ext4 301MiB 100%
mkfs.fat -F32 ${LOOP_DEV}p1
mkfs.ext4 ${LOOP_DEV}p2

# mount, extract rootfs and generate a mount table
CHROOT_DIR='alarm-chroot'
mkdir -p ${CHROOT_DIR}
mount ${LOOP_DEV}p2 ${CHROOT_DIR}
MIRROR_URL='http://fl.us.mirror.archlinuxarm.org'
curl -fsSL "$MIRROR_URL/os/ArchLinuxARM-aarch64-latest.tar.gz" -o alarm.tar.gz
bsdtar -xpf alarm.tar.gz -C ${CHROOT_DIR}
mkdir -p ${CHROOT_DIR}/boot/efi
mount ${LOOP_DEV}p1 ${CHROOT_DIR}/boot/efi

###### dirty insert ######

genfstab -U ${CHROOT_DIR} >> ${CHROOT_DIR}/etc/fstab

# github action runner has a swap, we don't need it.
sed -i '/^.*swap.*$/d' ${CHROOT_DIR}/etc/fstab

# many tutorials sugget this
cp /usr/bin/qemu-aarch64-static  ${CHROOT_DIR}/usr/bin/qemu-aarch64-static

# enable ParallelDownloads
sed -i 's/#ParallelDownloads = 5/ParallelDownloads = 4/' ${CHROOT_DIR}/etc/pacman.conf
sed -i 's/^DownloadUser = .*/DownloadUser = root/' ${CHROOT_DIR}/etc/pacman.conf
sed -i 's/^#DisableSandboxFilesystem/DisableSandboxFilesystem/' ${CHROOT_DIR}/etc/pacman.conf
sed -i 's/^#DisableSandboxSyscalls/DisableSandboxSyscalls/' ${CHROOT_DIR}/etc/pacman.conf
echo "Server = $MIRROR_URL"'/$arch/$repo' >> ${CHROOT_DIR}/etc/pacman.d/mirrorlist

# add my repo to install kernel and firmware
echo '[nuvole-arch]' >> ${CHROOT_DIR}/etc/pacman.conf
echo "Server = https://github.com/right-0903/my_arch_auto_pack/releases/download/packages" >> ${CHROOT_DIR}/etc/pacman.conf

# set console font
cat << EOF >> ${CHROOT_DIR}/etc/vconsole.conf
KEYMAP=us
FONT=solar24x32
EOF

# disable all kinds of sleep for now
cat << EOF >> ${CHROOT_DIR}/etc/systemd/sleep.conf
AllowSuspend=no
AllowHibernation=no
AllowSuspendThenHibernate=no
AllowHybridSleep=no
EOF

# initialize the pacman keyring and populate the Arch Linux ARM package signing keys
# https://archlinuxarm.org/platforms/armv8/generic
arch-chroot ${CHROOT_DIR} sh -c 'pacman-key --init && pacman-key --populate archlinuxarm'

# trust my key for my repo
curl https://raw.githubusercontent.com/right-0903/my_arch_auto_pack/refs/heads/main/keys/CA909D46CD1890BE.asc -o ${CHROOT_DIR}/root/CA909D46CD1890BE.asc
arch-chroot ${CHROOT_DIR} sh -c 'pacman-key --add /root/CA909D46CD1890BE.asc && pacman-key --lsign-key CA909D46CD1890BE'

# Keep the stock kernel installed until the replacement kernel transaction succeeds.
arch-chroot ${CHROOT_DIR} sh -c 'pacman -Syu efibootmgr grub wireless-regdb linux-gaokun3 linux-gaokun3-headers iwd btrfs-progs --noconfirm'

# The custom firmware package overlaps with upstream qcom/atheros firmware files.
arch-chroot ${CHROOT_DIR} sh -c 'pacman -Rdd --noconfirm linux-firmware-atheros linux-firmware-qcom || true'
arch-chroot ${CHROOT_DIR} sh -c "pacman -S linux-firmware-gaokun3 --noconfirm --overwrite '/usr/lib/firmware/ath11k/WCN6855/*,/usr/lib/firmware/qca/*,/usr/lib/firmware/qcom/a660_*'"

# make a copy for this repo
mv ${CHROOT_DIR}/var/cache/pacman/pkg/*.pkg.tar.zst .
rm -f ${CHROOT_DIR}/var/cache/pacman/pkg/*

# use early KMS for debugging, this would give us log in the initramfs stage.
if [[ -f ${CHROOT_DIR}/etc/mkinitcpio-gaokun3.conf ]]; then
    sed -i 's/^\(MODULES=(\)/\1\nsimpledrm\nphy-qcom-snps-femto-v2/' ${CHROOT_DIR}/etc/mkinitcpio-gaokun3.conf
elif [[ -f ${CHROOT_DIR}/etc/mkinitcpio.conf ]]; then
    sed -i 's/^\(MODULES=(\)/\1\nsimpledrm\nphy-qcom-snps-femto-v2/' ${CHROOT_DIR}/etc/mkinitcpio.conf
fi
arch-chroot ${CHROOT_DIR} sh -c 'mkinitcpio -P'

# install grub
arch-chroot ${CHROOT_DIR} sh -c 'grub-install --target=arm64-efi --efi-directory=/boot/efi --bootloader-id=arch --no-nvram --recheck'

# fix efi loading
arch-chroot ${CHROOT_DIR} sh -c 'mkdir -p /boot/efi/EFI/Boot && cp /boot/efi/EFI/arch/grubaa64.efi /boot/efi/EFI/Boot/BOOTAA64.EFI'

# set kernel commandline parameters
sed -i 's/GRUB_CMDLINE_LINUX=""/GRUB_CMDLINE_LINUX="clk_ignore_unused pd_ignore_unused arm64.nopauth iommu.passthrough=0 iommu.strict=0 pcie_aspm.policy=powersupersave efi=noruntime modprobe.blacklist=msm"/' ${CHROOT_DIR}/etc/default/grub
sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT="loglevel=3 quiet"/GRUB_CMDLINE_LINUX_DEFAULT="fbcon=rotate:1 loglevel=3"/' ${CHROOT_DIR}/etc/default/grub

# generate the grub config
arch-chroot ${CHROOT_DIR} sh -c 'grub-mkconfig -o /boot/grub/grub.cfg'

# do clean
rm ${CHROOT_DIR}/usr/bin/qemu-aarch64-static ${CHROOT_DIR}/root/*

# umount
umount -R ${CHROOT_DIR} && losetup -d ${LOOP_DEV}

# compress, github release is limited to 2GB
xz archlinuxarm.img
