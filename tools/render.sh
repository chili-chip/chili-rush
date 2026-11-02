#!/bin/sh
# Renders the Chili Rush audio to tools/out/*.wav and prints level stats.
set -e
cd "$(dirname "$0")/.."
SDK=${PATH_32BLIT_SDK:-../32blit-sdk}
mkdir -p tools/out
g++ -std=c++20 -O2 -Wall -Wextra -Wdouble-promotion -Wno-unused-parameter \
    -I"$SDK/32blit" -I. tools/render_audio.cpp audio.cpp "$SDK/32blit/audio/audio.cpp" -o tools/out/render_audio
./tools/out/render_audio tools/out
