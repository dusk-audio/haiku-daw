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

# Dumb HTTP transport serves static files: without this metadata a clone gets a
# 404 on the refs and fails in a way that reads like a network problem. If it
# cannot be written there is nothing worth serving, so stop here.
if ! git update-server-info; then
    echo "ERROR: git update-server-info failed - refusing to serve stale metadata" >&2
    exit 1
fi

# Which address to advertise.
#
# `hostname -I | awk '{print $1}'` was wrong: this host has several addresses
# (LAN, libvirt bridge, tailscale) and their order is not fixed, so the first
# one is frequently NOT the one the Haiku machine can reach. The VM reaches the
# host on the libvirt bridge, real hardware on the LAN -- there is no single
# right answer to guess, so allow an explicit choice and otherwise show every
# candidate instead of silently picking one.
#
#   SERVE_ADDR=192.168.122.1 sh scripts/serve.sh
if [ -n "$SERVE_ADDR" ]; then
    IP="$SERVE_ADDR"
else
    # Drop loopback and link-local; they can never be the answer.
    CANDIDATES=$(hostname -I 2>/dev/null | tr ' ' '\n' \
                 | grep -E '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' \
                 | grep -vE '^(127\.|169\.254\.)' )
    IP=$(printf '%s\n' "$CANDIDATES" | head -1)
fi

if [ -z "$IP" ]; then
    echo "ERROR: no usable IPv4 address found." >&2
    echo "  Set one explicitly:  SERVE_ADDR=<addr> sh scripts/serve.sh [port]" >&2
    exit 1
fi

echo "Serving $(pwd)"
echo "  clone with: git clone http://${IP}:${PORT}/.git haiku-daw"
if [ -z "$SERVE_ADDR" ]; then
    OTHERS=$(printf '%s\n' "$CANDIDATES" | tail -n +2)
    if [ -n "$OTHERS" ]; then
        echo "  other addresses on this host (use SERVE_ADDR if the clone hangs):"
        printf '%s\n' "$OTHERS" | sed 's/^/    /'
    fi
fi
echo "  (Ctrl-C to stop)"
python3 -m http.server "$PORT" --bind 0.0.0.0
