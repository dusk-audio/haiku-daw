#!/bin/sh
# Diagnose + fix the missing media decoder plugins that make BMediaFile
# return "No handler". Run:  cd ~/haiku-daw && git pull && sh scripts/fix_media.sh

echo "== 1. what's under the media add-ons dir? =="
ls -la /boot/system/add-ons/media/ 2>&1
echo
echo "== 2. search for any reader/decoder plugins anywhere =="
find /boot -path '*media*plugin*' 2>/dev/null | head -40
find /boot -name '*reader*' 2>/dev/null | head -20
echo
echo "== 3. installed media-related packages =="
pkgman search -i ffmpeg 2>&1
pkgman search -i media 2>&1 | head -20
echo
echo "== 4. install the ffmpeg codec/reader plugin package =="
pkgman install -y ffmpeg
echo
echo "== 5. recheck plugins dir (should now list wav/ffmpeg readers) =="
ls -la /boot/system/add-ons/media/plugins/ 2>&1
echo
echo "== DONE. If step 5 now shows plugins, restart media services"
echo "   (Media preferences -> Restart Media Services) then rerun play_clip. =="
