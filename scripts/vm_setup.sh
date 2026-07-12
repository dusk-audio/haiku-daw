#!/bin/sh
# One-shot VM setup so the host can SSH in and drive the build.
# Idempotent: safe to run more than once.
#
#   cd ~/haiku-daw && git pull && sh scripts/vm_setup.sh
#
# The embedded key is a PUBLIC key — safe to commit.

KEY='ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAII2R3DhxZMAnyw6E+flpkSxczFrYqbJj7qefcDq/wQQq claude-haiku-vm'

echo "== installing authorized_keys =="
mkdir -p ~/.ssh
chmod 700 ~/.ssh
touch ~/.ssh/authorized_keys
if grep -qF "$KEY" ~/.ssh/authorized_keys; then
    echo "  key already present"
else
    echo "$KEY" >> ~/.ssh/authorized_keys
    echo "  key added"
fi
chmod 600 ~/.ssh/authorized_keys

echo
echo "== sshd status =="
if ps | grep -v grep | grep -q sshd; then
    echo "  sshd RUNNING"
else
    echo "  sshd NOT running -> installing openssh (reboot after if needed)"
    pkgman install -y openssh
    echo "  installed. If sshd still not running, reboot the VM."
fi

echo
echo "== VM IP address (host will SSH to the 192.168.x.x one) =="
ifconfig | grep -i inet | grep -v 127.0.0.1 | grep -v '::1'

echo
echo "== done. Tell the host: the 192.168.x.x IP above + whether sshd is RUNNING =="
