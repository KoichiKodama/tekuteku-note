#!/usr/bin/env python
import sys
import shlex
import subprocess
import json

class connection_t:
	def __init__(self,name,device,status,addr):
		self.name = name
		self.device = device
		self.status = status	# 0:未接続 1:接続中 9:利用不可
		self.addr = addr

wifi_dev = 'wlan0'
known_connections = ['AUEWLAN','auewlan@sien','kodama-420','eth0','sim','sim-nttpc']
connections_wifi = []
connections_ethernet = []

def print_responce(status,message) :
	print('Content-Type: text/plain\n\n')
	print(json.dumps({"status":status,"message":message})) # status : 0/失敗 1/成功

if len(sys.argv) < 2 :
	print_responce(0,"ssid-not-given")
	sys.exit()

job = sys.argv[1]
ssid = sys.argv[2] if len(sys.argv) == 3 else ''
match job :
	case 'shutdown' :
		print_responce(1,"done")
		r = subprocess.run(shlex.split('sudo shutdown now'))
		sys.exit()

	case 'rescan' :
#		r = subprocess.run(shlex.split('sudo nmcli -f SSID,SIGNAL dev wifi list ifname {0}'.format(wifi_dev)),stdout=subprocess.PIPE,encoding='utf-8')
		r = subprocess.run(shlex.split('sudo nmcli -f SSID,SIGNAL dev wifi list ifname {0} -rescan yes'.format(wifi_dev)))
		print_responce(1,"done")
		sys.exit()

	case 'connect' :
		r = subprocess.run(shlex.split('sudo nmcli con'),stdout=subprocess.PIPE,encoding='utf-8',check=True)
		l = r.stdout.splitlines()
		for s in l[1:len(l)] :
			a = s.split()
			if a[0] == ssid :
				if a[3] != '--' : 
					print_responce(1,"already connected")
					sys.exit()
				else :
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
				sys.exit()
		print_responce(0,"no-such-connection({0})".format(ssid))
		sys.exit()

	case 'disconnect' :
		r = subprocess.run(shlex.split('sudo nmcli con'),stdout=subprocess.PIPE,encoding='utf-8',check=True)
		l = r.stdout.splitlines()
		for s in l[1:len(l)] :
			a = s.split()
			if a[0] == ssid :
				if a[3] == '--' : 
					print_responce(1,"already disconnected")
					sys.exit()
				else :
					r = subprocess.run(shlex.split('sudo nmcli con down '+ssid),stdout=subprocess.PIPE,encoding='utf-8')
					if r.returncode != 0 :
						print_responce(0,"failed-in-con-down")
						sys.exit()
					r = subprocess.run(shlex.split('sudo nmcli con'),stdout=subprocess.PIPE,encoding='utf-8',check=True)
					l = r.stdout.splitlines()
					status = 9
					for s in l[1:len(l)] :
						a = s.split()
						if a[0] == ssid and a[3] == "--" :
							print_responce(1,"disconnected({0})".format(ssid))
							sys.exit()
				print_responce(0,"internal-error")
				sys.exit()
		print_responce(0,"no-such-connection({0})".format(ssid))
		sys.exit()
print_responce(0,"internal-error")
