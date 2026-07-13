#!/bin/sh
# Probe what audio-INPUT capability this (plugin-less) Haiku image actually
# exposes, before writing any M5 recording code. Same discipline as M2: the
# image lacks media reader plugins, so never assume an API works — test it.
#
# Run inside the desktop session (needs media_server + app_server):
#   cd ~/haiku-daw && git pull && sh scripts/record_probe.sh 2>&1 | tail -80
#
# Compiles two tiny probes with the real Media Kit and runs them.

TMP=$(mktemp -d /tmp/recprobe.XXXXXX)
echo "== 0. Haiku version =="
uname -a
echo
echo "== 1. is media_server running? =="
ps 2>/dev/null | grep -i media_server | grep -v grep || echo "  media_server NOT in ps"

echo
echo "== 2. relevant Media Kit headers present? =="
for h in MediaRoster.h MediaNode.h MediaRecorder.h SoundRecorder.h BufferConsumer.h; do
    f="/boot/system/develop/headers/os/media/$h"
    if [ -f "$f" ]; then echo "  yes  $h"; else echo "  NO   $h"; fi
done

# --- Probe A: roster, GetAudioInput, physical input producer nodes ---------
cat > "$TMP/probe_input.cpp" <<'EOF'
#include <MediaRoster.h>
#include <MediaNode.h>
#include <MediaDefs.h>
#include <cstdio>
#include <cstring>

int main() {
    BMediaRoster* r = BMediaRoster::Roster();
    if (!r) { printf("NO media roster (media_server down?)\n"); return 1; }

    media_node in;
    status_t s = r->GetAudioInput(&in);
    printf("GetAudioInput: %s\n", strerror(s));
    if (s == B_OK) {
        printf("  node id=%d kind=0x%llx name=%s\n",
               (int)in.node, (unsigned long long)in.kind, in.name);
        r->ReleaseNode(in);
    }

    media_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = B_MEDIA_RAW_AUDIO;

    const int32 kMax = 32;
    live_node_info list[kMax];
    int32 count = kMax;
    s = r->GetLiveNodes(list, &count, NULL, &fmt, NULL,
                        B_BUFFER_PRODUCER | B_PHYSICAL_INPUT);
    printf("GetLiveNodes(physical raw-audio input producers): %s count=%d\n",
           strerror(s), (int)count);
    for (int32 i = 0; i < count; i++)
        printf("  [%d] %s (node %d)\n", (int)i, list[i].name,
               (int)list[i].node.node);
    return 0;
}
EOF

echo
echo "== 3. compile + run input-node probe =="
if g++ "$TMP/probe_input.cpp" -o "$TMP/probe_input" -lbe -lmedia 2>"$TMP/errA"; then
    "$TMP/probe_input"
else
    echo "  COMPILE FAILED:"; cat "$TMP/errA"
fi

# --- Probe B: does BMediaRecorder exist and init? --------------------------
cat > "$TMP/probe_recorder.cpp" <<'EOF'
#include <MediaRecorder.h>
#include <MediaDefs.h>
#include <cstdio>
#include <cstring>

int main() {
    BMediaRecorder rec("probe", B_MEDIA_RAW_AUDIO);
    status_t s = rec.InitCheck();
    printf("BMediaRecorder InitCheck: %s\n", strerror(s));
    // Try connecting it to the system audio input.
    media_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = B_MEDIA_RAW_AUDIO;
    status_t c = rec.Connect(fmt);
    printf("BMediaRecorder::Connect(raw audio wildcard): %s\n", strerror(c));
    return 0;
}
EOF

echo
echo "== 4. compile + run BMediaRecorder probe =="
if g++ "$TMP/probe_recorder.cpp" -o "$TMP/probe_recorder" -lbe -lmedia 2>"$TMP/errB"; then
    "$TMP/probe_recorder"
else
    echo "  COMPILE FAILED (BMediaRecorder likely absent on this image):"
    cat "$TMP/errB"
fi

echo
echo "== 5. verdict hint =="
echo "  - GetAudioInput B_OK + >=1 physical input producer -> BMediaNode"
echo "    capture path is viable (BBufferConsumer on that producer)."
echo "  - BMediaRecorder InitCheck/Connect B_OK -> simpler high-level path."
echo "  - If both fail like M2's decode: capture unavailable in this VM;"
echo "    develop M5 model/file-writer host-side, test capture on real HW."

rm -rf "$TMP"
