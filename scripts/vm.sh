#!/bin/sh
# Drive the Haiku VM from the host over SSH. Since the Virt Manager migration the
# VM has its own libvirt-NAT IP (no more passt 2222 port-forward).
# Prereq once: in the VM run `sh scripts/fix_ssh.sh` (starts sshd, installs key).
#
#   sh scripts/vm.sh ssh   [cmd...]   # run a command in the VM (default: shell info)
#   sh scripts/vm.sh sync             # put the host's current branch on the VM
#   sh scripts/vm.sh build            # sync + configure + build on the VM
#   sh scripts/vm.sh test             # sync + build + ctest on the VM
#
# The VM checkout at ~/haiku-daw is hard-reset to the host's commit each sync
# (host is authoritative; the VM never commits). VM_REF=<ref> syncs another ref.
# A commit already on GitHub is fetched there by the VM (the repo is public);
# one that is not yet pushed goes over in a git bundle.
set -e
KEY=~/.ssh/haiku_vm
VM=${HAIKU_VM_IP:-192.168.122.48}   # override with HAIKU_VM_IP if it changes
OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=10"
SSH="ssh -i $KEY $OPTS user@$VM"
SCP="scp -i $KEY $OPTS"

cmd="${1:-ssh}"; shift 2>/dev/null || true

do_sync() {
    ref=${VM_REF:-$(git rev-parse --abbrev-ref HEAD)}
    sha=$(git rev-parse --verify "$ref^{commit}")
    git fetch -q origin
    if [ -n "$(git branch -r --contains "$sha")" ]; then
        $SSH "cd ~/haiku-daw && git fetch -q origin && git reset --hard $sha && git log --oneline -1"
    else
        git bundle create /tmp/haiku.bundle "$ref"
        $SCP /tmp/haiku.bundle user@$VM:/tmp/haiku.bundle
        $SSH "cd ~/haiku-daw && git fetch /tmp/haiku.bundle $ref && git reset --hard $sha && git log --oneline -1"
    fi
}

case "$cmd" in
  ssh)   $SSH "${@:-uname -a; pwd}";;
  sync)  do_sync;;
  # The build and the test run write to a log and are tailed AFTERWARDS: a
  # pipeline's status is its last command's, so `... | tail -3` reported SUCCESS
  # for a failed compile (or a failing ctest) and left the previous binaries in
  # place -- the exact trap the entry doc warns about ("check the build's exit
  # code, not only ctest"), defeated by this script. `exit $rc` propagates the
  # real status through ssh.
  build) do_sync; $SSH 'cd ~/haiku-daw && cmake -B build >/tmp/cm.log 2>&1 && cmake --build build -j2 >/tmp/daw-build.log 2>&1; rc=$?; tail -3 /tmp/daw-build.log; exit $rc';;
  test)  do_sync; $SSH 'cd ~/haiku-daw && cmake -B build >/tmp/cm.log 2>&1 && cmake --build build -j2 >/tmp/b.log 2>&1; rc=$?; if [ $rc -ne 0 ]; then tail -3 /tmp/b.log; exit $rc; fi; ctest --test-dir build >/tmp/daw-ctest.log 2>&1; rc=$?; tail -3 /tmp/daw-ctest.log; exit $rc';;
  *)     echo "usage: sh scripts/vm.sh {ssh|sync|build|test} [cmd...]"; exit 1;;
esac
