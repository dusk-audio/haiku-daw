#!/bin/sh
# One-shot setup for a FRESH Haiku install (real hardware), so the Linux host can
# SSH in and drive builds the same way it drives the VM.
#
# Run this ON THE HAIKU MACHINE. It needs the repo, which you get from the host
# (which serves it over HTTP with scripts/serve.sh):
#
#   pkgman install -y openssh cmake git lilv lilv_devel lv2
#   git clone http://192.168.1.230:8000/.git ~/haiku-daw
#   cd ~/haiku-daw && sh scripts/hw_setup.sh
#
# Idempotent: safe to run again. The embedded key is a PUBLIC key.
#
# vm_setup.sh does the SSH half of this for the VM; this adds the build
# dependencies and the authorized_keys path that Haiku's sshd actually reads,
# which is the part that silently breaks pubkey auth (see fix_ssh.sh).

KEY='ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAII2R3DhxZMAnyw6E+flpkSxczFrYqbJj7qefcDq/wQQq claude-haiku-vm'

echo "== packages =="
# gcc and make ship with Haiku; the rest do not. lilv/lv2 are what enable the
# LV2 hosting path -- without them the build still succeeds, with LV2 compiled
# out (DAW_LV2 auto-disables), so this is not fatal if a package is unavailable.
pkgman install -y openssh cmake git lilv lilv_devel lv2 || \
    echo "  (some packages failed - build will still work, LV2 may be disabled)"

echo
echo "== authorized_keys =="
# BOTH paths on purpose. Haiku's sshd_config defaults AuthorizedKeysFile to
# config/settings/ssh/authorized_keys, so writing only ~/.ssh looks correct and
# then silently fails to authenticate.
for AK in "$HOME/.ssh/authorized_keys" \
          "$HOME/config/settings/ssh/authorized_keys"; do
    mkdir -p "$(dirname "$AK")"
    if grep -qF "$KEY" "$AK" 2>/dev/null; then
        echo "  already present -> $AK"
    else
        echo "$KEY" >> "$AK"
        echo "  key added -> $AK"
    fi
    chmod 600 "$AK"
done
# sshd StrictModes refuses keys when $HOME or the .ssh dir are group/world
# writable, which is the other common silent failure.
chmod 755 "$HOME"
chmod 700 "$HOME/.ssh" "$HOME/config/settings/ssh" 2>/dev/null

echo
echo "== host keys =="
ssh-keygen -A 2>&1 | head -3 || echo "  (ssh-keygen -A unavailable)"

echo
echo "== sshd =="
if ps | grep -v grep | grep -q sshd; then
    echo "  sshd RUNNING"
else
    echo "  sshd not running - trying to start it"
    SSHD="$(which sshd 2>/dev/null)"
    [ -n "$SSHD" ] || SSHD=/boot/system/servers/sshd
    if [ -x "$SSHD" ]; then
        "$SSHD" 2>&1 | head -3
        sleep 1
        ps | grep -v grep | grep -q sshd \
            && echo "  sshd RUNNING" \
            || echo "  still not running -> REBOOT, Haiku starts it at boot"
    else
        echo "  sshd binary not found -> REBOOT after the package install"
    fi
fi

echo
echo "== IP address (tell the host this) =="
ifconfig 2>/dev/null | grep -i inet | grep -v 127.0.0.1 | grep -v '::1'

echo
echo "== toolchain =="
for c in gcc make cmake git pkg-config; do
    printf "  %-11s %s\n" "$c" "$(which $c 2>/dev/null || echo MISSING)"
done
printf "  %-11s %s\n" "lilv" \
    "$(pkg-config --modversion lilv-0 2>/dev/null || echo 'MISSING (LV2 disabled)')"

echo
echo "== done. Tell the host: the 192.168.x.x IP above, and whether sshd is RUNNING =="
