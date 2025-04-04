#!/bin/bash
#
sh rp1 cat usb0.out | rematch 'slept ([0-9]+) .* difference ([-0-9]+)' | gnuplot -e "set logscale xy; plot '-' u 1:(abs(\$2)); pause 111;"

