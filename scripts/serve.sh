#!/bin/sh
# Serve this repo to the Haiku VM over HTTP so it can `git pull`.
# Run on the HOST from the repo root:  sh scripts/serve.sh
#
# In the VM, clone/pull with:
#   git clone http://192.168.1.230:8000/.git haiku-daw   # first time
#   git pull                                             # thereafter
#
# Leave this running while iterating. Ctrl-C to stop.

cd "$(dirname "$0")/.." || exit 1
git update-server-info          # refresh dumb-HTTP metadata for pulls
echo "Serving $(pwd) at http://$(hostname -I 2>/dev/null | awk '{print $1}'):8000/.git"
echo "VM pulls from: http://192.168.1.230:8000/.git   (Ctrl-C to stop)"
python3 -m http.server 8000 --bind 0.0.0.0
