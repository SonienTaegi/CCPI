#!/bin/sh
raspivid -t 5000 -w 320 -h 240 -fps 15 -o fifo
avconv -r 15 -i fifo -pass 1 -coder 0 -bf 0 -weightp 0 -f h264 -b 10000 http://127.0.0.1:8082/kovcam/320/240
#raspivid -t 1000 -w 320 -h 240 -fps 15 -b 100000 -o - | ffmpeg -r 15 -i - -pass 1 -coder 0 -bf 0 -flags -loop -wpredp 0 -f h264 http://127.0.0.1:8082/kovcam/320/240

