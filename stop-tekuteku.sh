#!/bin/sh
ps -e | gawk '$4=="tekuteku_server"{print $1; system(sprintf("sudo kill %s\n",$1));}END{print "done";}'
