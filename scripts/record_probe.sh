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
        printf("  node id=%d kind=0x%llx\n",
               (int)in.node, (unsigned long long)in.kind);
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

# --- Probe B: BMediaRecorder end-to-end capture (the M5 recipe) -------------
cat > "$TMP/probe_recorder.cpp" <<'EOF'
#include <MediaRecorder.h>
#include <MediaRoster.h>
#include <MediaNode.h>
#include <MediaDefs.h>
#include <OS.h>
#include <cstdio>
#include <cstring>
#include <atomic>

static std::atomic<size_t> g_bytes{0};
static std::atomic<int>    g_calls{0};
static media_format        g_negotiated;

static void RecordHook(void*, bigtime_t, void* /*data*/, size_t size,
                       const media_format& fmt) {
    g_bytes.fetch_add(size);
    if (g_calls.fetch_add(1) == 0)
        g_negotiated = fmt;   // capture the format the input actually gave us
}

int main() {
    BMediaRecorder rec("probe", B_MEDIA_RAW_AUDIO);
    printf("InitCheck: %s\n", strerror(rec.InitCheck()));

    // Ask for float stereo but leave rate wildcard so the device picks native.
    media_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = B_MEDIA_RAW_AUDIO;
    fmt.u.raw_audio = media_raw_audio_format::wildcard;
    fmt.u.raw_audio.format     = media_raw_audio_format::B_AUDIO_FLOAT;
    fmt.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;

    rec.SetHooks(RecordHook, NULL, NULL);

    // The source-less Connect(format) returns "Bad source" on this image, so
    // connect to the explicit physical input node (GetAudioInput / node 3).
    BMediaRoster* roster = BMediaRoster::Roster();
    media_node inNode;
    status_t gi = roster ? roster->GetAudioInput(&inNode) : B_ERROR;
    printf("GetAudioInput for connect: %s\n", strerror(gi));

    status_t c = B_ERROR;
    if (gi == B_OK) {
        c = rec.Connect(inNode, NULL, &fmt);
        printf("Connect(inputNode, NULL, &fmt): %s\n", strerror(c));
    }
    // Fall back to the source-less form just to record its status too.
    if (c != B_OK) {
        status_t c2 = rec.Connect(fmt);
        printf("Connect(format only): %s\n", strerror(c2));
        c = c2;
    }
    if (c != B_OK) return 1;

    printf("Start: %s\n", strerror(rec.Start()));
    snooze(500000);            // capture ~0.5 s
    rec.Stop();

    const media_raw_audio_format& n = g_negotiated.u.raw_audio;
    printf("captured: %d callbacks, %lu bytes\n",
           g_calls.load(), (unsigned long)g_bytes.load());
    printf("negotiated: rate=%.0f ch=%u fmt=0x%x buf=%u bytes\n",
           n.frame_rate, n.channel_count, n.format, n.buffer_size);
    return 0;
}
EOF

echo
echo "== 4. compile + run BMediaRecorder capture probe =="
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
