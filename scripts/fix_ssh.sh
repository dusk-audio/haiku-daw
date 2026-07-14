#!/bin/sh
# Make pubkey SSH actually work. Fixes the common cause: sshd StrictModes
# ignores authorized_keys when $HOME or ~/.ssh perms are too open.
#   cd ~/haiku-daw && git pull && sh scripts/fix_ssh.sh

KEY='ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAII2R3DhxZMAnyw6E+flpkSxczFrYqbJj7qefcDq/wQQq claude-haiku-vm'

echo "== identity =="
whoami; echo "HOME=$HOME"; ls -ld "$HOME" "$HOME/.ssh" 2>&1

echo
echo "== current authorized_keys =="
cat "$HOME/.ssh/authorized_keys" 2>&1

echo
echo "== re-add key + tighten perms =="
mkdir -p "$HOME/.ssh"
# Install into BOTH the common ~/.ssh path AND the path Haiku's sshd_config
# actually uses by default: AuthorizedKeysFile config/settings/ssh/authorized_keys
# (relative to $HOME). Writing only ~/.ssh silently fails pubkey auth.
for AK in "$HOME/.ssh/authorized_keys" \
          "$HOME/config/settings/ssh/authorized_keys"; do
    mkdir -p "$(dirname "$AK")"
    if ! grep -qF "$KEY" "$AK" 2>/dev/null; then
        echo "$KEY" >> "$AK"
        echo "  key appended -> $AK"
    else
        echo "  key already present -> $AK"
    fi
    chmod 600 "$AK"
done
chmod 755 "$HOME"
chmod 700 "$HOME/.ssh" "$HOME/config/settings/ssh" 2>/dev/null
ls -l "$HOME/.ssh/authorized_keys" "$HOME/config/settings/ssh/authorized_keys" 2>&1

echo
echo "== sshd_config auth-relevant settings =="
for f in /system/settings/ssh/sshd_config /boot/system/settings/ssh/sshd_config \
         /boot/home/config/settings/ssh/sshd_config; do
    [ -f "$f" ] && { echo "-- $f"; grep -iE 'StrictModes|PubkeyAuth|AuthorizedKeysFile|PasswordAuth|PermitEmpty' "$f"; }
done

echo
echo "== sshd running? =="
ps | grep -v grep | grep sshd

echo
echo "== done. host will retry SSH. =="
