#!/bin/sh
# Find where Haiku's media add-ons actually live (the standard path is
# missing). Run: cd ~/haiku-daw && git pull && sh scripts/media_probe.sh 2>&1 | tail -60

echo "== A. does the media add-ons dir exist at all? =="
ls -la /boot/system/add-ons/media/ 2>&1

echo
echo "== B. every media add-ons subdir anywhere =="
find /boot -type d -path '*add-ons/media*' 2>/dev/null

echo
echo "== C. actual reader/decoder/writer plugin files =="
find /boot -path '*add-ons/media*' -name '*' -type f 2>/dev/null | head -60

echo
echo "== D. ffmpeg media-kit plugin present? =="
find /boot -name '*ffmpeg*' 2>/dev/null | grep -i -E 'media|add-on'

echo
echo "== E. media-ish packages actually installed on disk =="
ls /boot/system/packages/ 2>/dev/null | grep -iE 'haiku|media|ffmpeg|codec'

echo
echo "== F. Haiku version =="
uname -a
