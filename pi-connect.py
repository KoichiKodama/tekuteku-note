#!/usr/bin/env python
import sys
import shlex
import subprocess
import json

class connection_t:
	def __init__(self,name,status):
		self.name = name
		self.status = status	# 0:未接続 1:接続中 9:無

known_connections = ['AUEWLAN','kodama-420','eth0','sim']
connections_wifi = []
connections_ethernet = []

def print_responce(status,message) :
	print('Content-Type: text/plain\n\n')
	print(json.dumps({"status":status,"message":message})) # status : 0/失敗 1/成功

ssid = ""
if len(sys.argv) != 2 :
	print_responce(0,"ssid-not-given")
	sys.exit()

ssid = ""
r = subprocess.run(shlex.split('sudo nmcli con'),stdout=subprocess.PIPE,encoding='utf-8',check=True)
l = r.stdout.splitlines()
for s in l[1:len(l)] :
	a = s.split()
	if a[0] == sys.argv[1] :
		ssid = a[0]
		if a[3] != '--' : 
			print_responce(1,"already connected")
			sys.exit()
		break

if ssid == "" :
	print_responce(0,"no-such-connection({0})".format(sys.argv[1]))
	sys.exit()

r = subprocess.run(shlex.split('sudo nmcli con up '+ssid),stdout=subprocess.PIPE,encoding='utf-8')
if r.returncode != 0 :
	print_responce(0,"failed-in-con-up")
	sys.exit()

r = subprocess.run(shlex.split('sudo nmcli con'),stdout=subprocess.PIPE,encoding='utf-8',check=True)
l = r.stdout.splitlines()
status = 9
for s in l[1:len(l)] :
	a = s.split()
	if a[0] == ssid and a[3] != "--" :
		print_responce(1,"connected({0})".format(ssid))
		sys.exit()

print_responce(0,"internal-error")
