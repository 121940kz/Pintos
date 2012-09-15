#! /bin/bash
# qt.sh = a "Quick Test" to be sure nothing is broken; just runs alarm-multiple
#
# To run this script, type ". qt.sh" in the putty terminal window.
# If it doesn't work, this file may not be in your path or your folder
# capitalization is different (e.g. "Pintos" instead of "pintos" in the first line).
#
# Recommendation:
#   Add ~/cis520/Pintos/bin to your path 
#
cd ~/cis520/Pintos/pintos/src/threads
make clean
make
cd build
pintos run alarm-multiple