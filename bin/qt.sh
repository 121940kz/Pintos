#! /bin/bash
# Description: qt.sh is a quick test to make sure nothing is wrong 
# Notes: To run this script, type . qt.sh in the terminal 
cd ~/CIS520/Pintos/pintos/src/threads
make clean
make
cd build
pintos run alarm-multiple
