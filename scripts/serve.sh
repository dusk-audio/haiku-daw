#!/bin/sh
# Serve this repo over HTTP so a Haiku machine can clone/pull it. The repo has no
# git remote, so this is how code gets across.
#
# Run on the HOST from the repo root:  sh scripts/serve.sh [port]
#
# On the Haiku machine:
#   git clone http://192.168.1.230:9090/.git haiku-daw   # first time
#   git pull                                             # thereafter
#
# Leave this running while iterating. Ctrl-C to stop.
#
# PORT 9090, NOT 8000. The host runs firewalld with the LAN interface in the
# `public` zone, which allows only ssh, cockpit and dhcpv6-client. Port 8000 is
# dropped, so a clone from another machine HANGS and then times out -- it never
# gets refused, which makes it look like a problem on the Haiku end. 9090 is
# cockpit's port, and cockpit is not running, so serving there goes through an
# existing hole and needs no sudo.
#
# Check before blaming the client:
#   firewall-cmd --zone=public --list-all
# To use a different port you have to open it yourself (runtime only, reverts on
# reboot):
#   sudo firewall-cmd --add-port=8000/tcp
#
# Testing with curl FROM THIS MACHINE proves nothing: loopback never crosses the
# firewall. Only a fetch from another host tells you it works.

PORT="${1:-9090}"

cd "$(dirname "$0")/.." || exit 1
git update-server-info          # refresh dumb-HTTP metadata for pulls

IP=$(hostname -I 2>/dev/null | awk '{print $1}')
echo "Serving $(pwd)"
echo "  clone with: git clone http://${IP}:${PORT}/.git haiku-daw"
echo "  (Ctrl-C to stop)"
python3 -m http.server "$PORT" --bind 0.0.0.0
