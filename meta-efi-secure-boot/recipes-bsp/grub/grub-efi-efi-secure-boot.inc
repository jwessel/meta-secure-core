DEPENDS += "openssl-native"
FILESEXTRAPATHS_prepend := "${THISDIR}/grub-efi:"

GRUB_SIGN_VERIFY_STRICT ?= "1"

EXTRA_SRC_URI = "\
    ${@'file://efi-secure-boot.inc file://password.inc' if d.getVar('UEFI_SB', True) == '1' else ''} \
"

GRUB_MOKVERIFY_PATCH = " \
   file://mok2verify-support-to-verify-non-PE-file-with-PKCS-7.patch \
   file://mok2verify-multiboot.patch"

SRC_URI += "\
    file://0001-pe32.h-add-header-structures-for-TE-and-DOS-executab.patch \
    file://0002-shim-add-needed-data-structures.patch \
    file://0003-efi-chainloader-implement-an-UEFI-Exit-service-for-s.patch \
    file://0004-efi-chainloader-port-shim-to-grub.patch \
    file://0005-efi-chainloader-use-shim-to-load-and-verify-an-image.patch \
    file://0006-efi-chainloader-boot-the-image-using-shim.patch \
    file://0007-efi-chainloader-take-care-of-unload-undershim.patch \
    file://chainloader-handle-the-unauthenticated-image-by-shim.patch \
    file://chainloader-Don-t-check-empty-section-in-file-like-..patch \
    file://chainloader-Actually-find-the-relocations-correctly-.patch \
    file://efi-chainloader-implemented-for-32-bit.patch \
    file://Grub-get-and-set-efi-variables.patch \
    file://uefi_verify.patch \
    file://0001-grub-verify-Add-strict_security-variable.patch \
    file://grub-efi.cfg \
    file://boot-menu.inc \
    ${@d.getVar('GRUB_MOKVERIFY_PATCH', True) if d.getVar('UEFI_SELOADER', True) == '1' else ''} \
    ${EXTRA_SRC_URI} \
"

# functions efi_call_foo and efi_shim_exit are not implemented for arm64 yet
COMPATIBLE_HOST_aarch64 = 'null'

EFI_BOOT_PATH = "/boot/efi/EFI/BOOT"

GRUB_SIGNING_MODULES += "${@'pgp gcry_rsa gcry_sha256 gcry_sha512 --pubkey %s ' % d.getVar('GRUB_PUB_KEY', True) if d.getVar('GRUB_SIGN_VERIFY', True) == '1' else ''}"

GRUB_BUILDIN_append += "tftp reboot ${GRUB_SIGNING_MODULES} chain \
  ${@'efivar password_pbkdf2 ' if d.getVar('UEFI_SB', True) == '1' else ''} \
  ${@'mok2verify ' if d.getVar('UEFI_SELOADER', True) == '1' else ''}"

# For efi_call_foo and efi_shim_exit
CFLAGS_append = " -fno-toplevel-reorder"

# Set a default root specifier.
inherit user-key-store

python __anonymous () {
    if d.getVar('UEFI_SB', True) != "1":
        return

    # Override the default filename if efi-secure-boot enabled.
    # grub-efi must be renamed as grub${arch}.efi for working with shim
    # or SELoader.
    import re

    target = d.getVar('TARGET_ARCH', True)
    if target == "x86_64":
        grubimage = "grubx64.efi"
    elif re.match('i.86', target):
        grubimage = "grubia32.efi"
    else:
        raise bb.parse.SkipPackage("grub-efi is incompatible with target %s" % target)

    d.setVar("GRUB_IMAGE", grubimage)
}

do_compile_append() {
	if [ "${GRUB_SIGN_VERIFY}" = "1" -a "${GRUB_SIGN_VERIFY_STRICT}" = "1" ] ; then
		cat<<EOF>${WORKDIR}/cfg
insmod verify
set strict_security=1
search.file (\$cmdpath)/EFI/BOOT/grub.cfg root
set prefix=(\$root)/EFI/BOOT
EOF
	else
		cat<<EOF>${WORKDIR}/cfg
search.file (\$cmdpath)/EFI/BOOT/grub.cfg root
set prefix=(\$root)/EFI/BOOT
EOF
	fi
}

do_compile_append_class-native() {
    make grub-editenv
}

do_install_append_class-native() {
    install -m 0755 grub-editenv "${D}${bindir}"
}

do_install_append_class-target() {
    local menu="${WORKDIR}/boot-menu.inc"

    # Enable the default IMA rules if IMA is enabled and luks is disabled.
    # This is because unseal operation will fail when any PCR is extended
    # due to updating the aggregate integrity value by the default IMA rules.
    [ x"${IMA}" = x"1" -a x"${@bb.utils.contains('DISTRO_FEATURES', 'luks', '1', '0', d)}" != x"1" ] && {
        ! grep -q "ima_policy=tcb" "$menu" &&
            sed -i 's/^\s*linux\s\+.*bzImage.*/& ima_policy=tcb/g' "$menu"
    }

    # Replace the root parameter in boot command line with BOOT_CMD_ROOT,
    # which can be configured. It is helpful when secure boot is enabled.
    [ -n "${BOOT_CMD_ROOT}" ] && {
        sed -i "s,root=/dev/hda2,root=${BOOT_CMD_ROOT},g" "$menu"
    }

    # Install the stacked grub configs.
    install -d "${D}${EFI_BOOT_PATH}"
    install -m 0600 "${WORKDIR}/grub-efi.cfg" "${D}${EFI_BOOT_PATH}/grub.cfg"
    install -m 0600 "$menu" "${D}${EFI_BOOT_PATH}"
    [ x"${UEFI_SB}" = x"1" ] && {
        install -m 0600 "${WORKDIR}/efi-secure-boot.inc" "${D}${EFI_BOOT_PATH}"
        install -m 0600 "${WORKDIR}/password.inc" "${D}${EFI_BOOT_PATH}"
    }

    # Create the initial environment block with empty item.
    grub-editenv "${D}${EFI_BOOT_PATH}/grubenv" create

    install -d "${D}${EFI_BOOT_PATH}/${GRUB_TARGET}-efi"
    grub-mkimage -c ../cfg -p /EFI/BOOT -d "./grub-core" \
        -O "${GRUB_TARGET}-efi" -o "${B}/${GRUB_IMAGE}" \
        ${GRUB_BUILDIN}

    install -m 0644 "${B}/${GRUB_IMAGE}" "${D}${EFI_BOOT_PATH}/${GRUB_IMAGE}"

    # Install the modules to grub-efi's search path
    make -C grub-core install DESTDIR="${D}${EFI_BOOT_PATH}" pkglibdir=""

    # Remove .module
    rm -f ${D}${EFI_BOOT_PATH}/${GRUB_TARGET}-efi/*.module
}

fakeroot python do_sign_class-target() {
    image_dir = d.getVar('D', True)
    efi_boot_path = d.getVar('EFI_BOOT_PATH', True)
    grub_image = d.getVar('GRUB_IMAGE', True)
    dir = image_dir + efi_boot_path + '/'

    sb_sign(dir + grub_image, dir + grub_image, d)
    uks_sel_sign(dir + 'grub.cfg', d)
    uks_sel_sign(dir + 'boot-menu.inc', d)
    uks_boot_sign(dir + 'grub.cfg', d)
    uks_boot_sign(dir + 'boot-menu.inc', d)

    if d.getVar('UEFI_SB', True) == "1":
        uks_sel_sign(dir + 'efi-secure-boot.inc', d)
        uks_sel_sign(dir + 'password.inc', d)
        uks_boot_sign(dir + 'efi-secure-boot.inc', d)
        uks_boot_sign(dir + 'password.inc', d)
}

python do_sign() {
}
addtask sign after do_install before do_deploy do_package
do_sign[prefuncs] += "check_deploy_keys"
do_sign[prefuncs] += "${@'check_boot_public_key' if d.getVar('GRUB_SIGN_VERIFY', True) == '1' else ''}"

fakeroot do_chownp7b() {
    if [ -e "${D}${EFI_BOOT_PATH}/grub.cfg.p7b" ] ; then
        chown root:root -R "${D}${EFI_BOOT_PATH}/grub.cfg.p7b"
        chown root:root -R "${D}${EFI_BOOT_PATH}/boot-menu.inc.p7b"
        [ x"${UEFI_SB}" = x"1" ] && {
            chown root:root -R "${D}${EFI_BOOT_PATH}/efi-secure-boot.inc.p7b"
            chown root:root -R "${D}${EFI_BOOT_PATH}/password.inc.p7b"
        }
    fi
    if [ -e "${D}${EFI_BOOT_PATH}/grub.cfg.sig" ] ; then
        chown root:root -R "${D}${EFI_BOOT_PATH}/grub.cfg.sig"
        chown root:root -R "${D}${EFI_BOOT_PATH}/boot-menu.inc.sig"
        [ x"${UEFI_SB}" = x"1" ] && {
            chown root:root -R "${D}${EFI_BOOT_PATH}/efi-secure-boot.inc.sig"
            chown root:root -R "${D}${EFI_BOOT_PATH}/password.inc.sig"
        }
   fi        
}
addtask chownp7b after do_deploy before do_package

# Override the do_deploy() in oe-core.
do_deploy_class-target() {
    install -m 0644 "${D}${EFI_BOOT_PATH}/${GRUB_IMAGE}" "${DEPLOYDIR}"

    install -d "${DEPLOYDIR}/efi-unsigned"
    install -m 0644 "${B}/${GRUB_IMAGE}" "${DEPLOYDIR}/efi-unsigned"
    cp -af "${D}${EFI_BOOT_PATH}/${GRUB_TARGET}-efi" "${DEPLOYDIR}/efi-unsigned"
}

FILES_${PN} += "/boot/efi"

CONFFILES_${PN} += "\
    ${EFI_BOOT_PATH}/grub.cfg \
    ${EFI_BOOT_PATH}/grubenv \
    ${EFI_BOOT_PATH}/boot-menu.inc \
    ${EFI_BOOT_PATH}/efi-secure-boot.inc \
"
