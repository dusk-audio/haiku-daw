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
# Required: without these the host cannot get in or build at all, so a failure
# here has to stop the script rather than be reported and walked past.
if ! pkgman install -y openssh cmake git; then
    echo "  ERROR: required packages (openssh cmake git) failed to install" >&2
    exit 1
fi
# Optional: lilv/lv2 only enable the LV2 hosting path. Without them the build
# still succeeds with LV2 compiled out (DAW_LV2 auto-disables), so a failure is
# a warning, not a stop.
pkgman install -y lilv lilv_devel lv2 || \
    echo "  (LV2 packages failed - build still works, LV2 compiled out)"

echo
echo "== authorized_keys =="
# BOTH paths on purpose. Haiku's sshd_config defaults AuthorizedKeysFile to
# config/settings/ssh/authorized_keys, so writing only ~/.ssh looks correct and
# then silently fails to authenticate.
for AK in "$HOME/.ssh/authorized_keys" \
          "$HOME/config/settings/ssh/authorized_keys"; do
    AKDIR="$(dirname "$AK")"
    if ! mkdir -p "$AKDIR"; then
        echo "  ERROR: cannot create $AKDIR" >&2
        exit 1
    fi

    ADDED=0
    if ! grep -qF "$KEY" "$AK" 2>/dev/null; then
        if ! printf '%s\n' "$KEY" >> "$AK"; then
            echo "  ERROR: cannot write the key to $AK" >&2
            exit 1
        fi
        ADDED=1
    fi

    # Permissions are part of the install, not a follow-up: sshd ignores a
    # readable-by-others authorized_keys, so a chmod failure means the key is
    # NOT usable and must not be reported as added.
    if ! chmod 600 "$AK"; then
        echo "  ERROR: cannot chmod 600 $AK" >&2
        exit 1
    fi

    if [ "$ADDED" -eq 1 ]; then
        echo "  key added -> $AK"
    else
        echo "  already present -> $AK"
    fi
done
# sshd StrictModes refuses keys when $HOME or the .ssh dir are group/world
# WRITABLE, which is the other common silent failure. Drop just those write
# bits: `chmod 755` would satisfy StrictModes while also handing every local
# user read and traverse access to the home directory, which is a wider grant
# than the problem needs.
chmod go-w "$HOME"
chmod 700 "$HOME/.ssh" "$HOME/config/settings/ssh" 2>/dev/null

echo
echo "== host keys =="
# `ssh-keygen -A | head` would report the exit status of `head`, which succeeds
# almost unconditionally, so a real failure to generate host keys looked fine.
# Capture the status first, then trim the output for readability.
if command -v ssh-keygen >/dev/null 2>&1; then
    KEYGEN_OUT="$(ssh-keygen -A 2>&1)"
    KEYGEN_ST=$?
    [ -n "$KEYGEN_OUT" ] && printf '%s\n' "$KEYGEN_OUT" | head -3
    if [ "$KEYGEN_ST" -ne 0 ]; then
        echo "  (ssh-keygen -A FAILED, status $KEYGEN_ST - sshd may not start)"
    fi
else
    echo "  (ssh-keygen unavailable - install openssh)"
fi

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
