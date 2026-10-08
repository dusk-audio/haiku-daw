#!/bin/sh
# Drive the Haiku VM from the host over SSH. Since the Virt Manager migration the
# VM has its own libvirt-NAT IP (no more passt 2222 port-forward).
# Prereq once: in the VM run `sh scripts/fix_ssh.sh` (starts sshd, installs key).
#
#   sh scripts/vm.sh ssh   [cmd...]   # run a command in the VM (default: shell info)
#   sh scripts/vm.sh sync             # push host `master` into the VM via git bundle
#   sh scripts/vm.sh build            # sync + configure + build on the VM
#   sh scripts/vm.sh test             # sync + build + ctest on the VM
#
# The VM checkout at ~/haiku-daw is hard-reset to the host's master each sync
# (host is authoritative; the VM never commits).
set -e
KEY=~/.ssh/haiku_vm
VM=${HAIKU_VM_IP:-192.168.122.48}   # override with HAIKU_VM_IP if it changes
OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=10"
SSH="ssh -i $KEY $OPTS user@$VM"
SCP="scp -i $KEY $OPTS"

cmd="${1:-ssh}"; shift 2>/dev/null || true

do_sync() {
    git bundle create /tmp/haiku.bundle master
    $SCP /tmp/haiku.bundle user@$VM:/tmp/haiku.bundle
    $SSH 'cd ~/haiku-daw && git fetch /tmp/haiku.bundle master && git reset --hard FETCH_HEAD && git log --oneline -1'
}

case "$cmd" in
  ssh)   $SSH "${@:-uname -a; pwd}";;
  sync)  do_sync;;
  build) do_sync; $SSH 'cd ~/haiku-daw && cmake -B build >/tmp/cm.log 2>&1 && cmake --build build -j4 2>&1 | tail -3';;
  test)  do_sync; $SSH 'cd ~/haiku-daw && cmake -B build >/tmp/cm.log 2>&1 && cmake --build build -j4 >/tmp/b.log 2>&1 && ctest --test-dir build 2>&1 | tail -3';;
  *)     echo "usage: sh scripts/vm.sh {ssh|sync|build|test} [cmd...]"; exit 1;;
esac
