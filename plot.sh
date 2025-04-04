#!/bin/bash
#
#ssh rp1 cat usb0.out | rematch 'slept ([0-9]+) .* difference ([-0-9]+)' | gnuplot -e "set logscale xy; plot '-' u 1:(abs(\$2)); pause 111;"
ssh rp1 cat usb0.out | rematch 'slept ([0-9]+) .* late \(([-0-9.]+.)' | gnuplot -e "
set logscale x;
set term qt size 1000,600;
plot [*:*][0:1] '-' u 1:(abs(\$2)); pause 111;"

