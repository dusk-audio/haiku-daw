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
if ! grep -qF "$KEY" "$HOME/.ssh/authorized_keys" 2>/dev/null; then
    echo "$KEY" >> "$HOME/.ssh/authorized_keys"
    echo "  key appended"
else
    echo "  key already present"
fi
chmod 755 "$HOME"
chmod 700 "$HOME/.ssh"
chmod 600 "$HOME/.ssh/authorized_keys"
ls -ld "$HOME" "$HOME/.ssh"; ls -l "$HOME/.ssh/authorized_keys"

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
