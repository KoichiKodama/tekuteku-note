#!/bin/sh
ps -e | grep tekuteku_server | gawk '{print $1; system(sprintf("sudo kill %s\n",$1));}END{print "done";}'
