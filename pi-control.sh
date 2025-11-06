#!/usr/bin/env python
import sys
import shlex
import subprocess
import json
import time

version = '2025-11-06'
wifi_dev = 'wlan1'
known_connections = ['AUEWS','AUEWT','auewlan@sien','sim','sim-nttpc','eth0','kodama-420','kodama-home']

def get_value(key,text):
	a = text.strip().split('=')
	if len(a) != 2:
		return "undefined"
	return a[1] if a[0] == key else "undefined"

class connection_t:
	def __init__(self,name,device="",status=9,addr="",kind=""):
		self.name = name
		self.device = device
		self.status = status	# 0:未接続 1:接続中 9:利用不可
		self.addr = addr
		self.kind = kind

class connection_t_encoder(json.JSONEncoder):
	def default(self, obj):
		if isinstance(obj,connection_t):
			return {"name":obj.name,"device":obj.device,"status":obj.status,"addr":obj.addr,"kind":obj.kind}
		else:
			return super().default(obj)

connections = {}
for c in known_connections:
	connections[c] = connection_t(c)

def print_responce(responce):
	print('Content-Type: text/plain\n') # 空行を入れる
	print(json.dumps(responce,cls=connection_t_encoder))

def job_status(force_rescan):
	r = subprocess.run(shlex.split('nmcli con'),stdout=subprocess.PIPE,encoding='utf-8')
	l = r.stdout.splitlines()
	for s in l[1:len(l)]:
		a = s.split()
		name = a[0]
		kind = a[2]
		device = a[3]
		if name in connections.keys():
			status = ( 1 if device != '--' else 9 )
			addr = ''
			if status == 1:
				cmd = 'nmcli con show {0} | awk \'$1=="IP4.ADDRESS[1]:"{{sub(/\/[0-9]+$/,"",$2); print $2;}}\''.format(name)
				r = subprocess.run(cmd,shell=True,stdout=subprocess.PIPE,encoding='utf-8')
				addr = r.stdout.strip('\n')
			connections[name] = connection_t(name,device,status,addr,kind)

	# signal >= 60 のみを利用可とする。
	wifi = []
	o = "-rescan yes" if force_rescan == 1 else ""
	r = subprocess.run(shlex.split('nmcli -g SSID,SIGNAL dev wifi list ifname {0} {1}'.format(wifi_dev,o)),stdout=subprocess.PIPE,encoding='utf-8')
	l = r.stdout.splitlines()
	for s in l[1:len(l)]:
		ss = s.split(':')
		ssid = ss[0]
		signal = int(ss[1])
		if signal >= 60: wifi.append(ssid)

	for c in connections.values():
		if c.kind == "wifi" and c.status == 9 and c.name in wifi: c.status = 0

	return { "status":1, "connections":list(connections.values()) }

def job_connect(ssid):
	r = subprocess.run(shlex.split('nmcli con'),stdout=subprocess.PIPE,encoding='utf-8',check=True)
	l = r.stdout.splitlines()
	for s in l[1:len(l)]:
		a = s.split()
		if a[0] == ssid:
			if a[3] != '--':
				return {"status":1,"message":"already-connected {0}".format(ssid)}
			else:
				r = subprocess.run(shlex.split('nmcli con up '+ssid),stdout=subprocess.PIPE,encoding='utf-8')
				if r.returncode == 0: return {"status":1,"message":"connected {0}".format(ssid)}
				return {"status":0,"message":"failed {0} exit-code = {1}".format(ssid,r.returncode)}
#				r = subprocess.run(shlex.split('nmcli con'),stdout=subprocess.PIPE,encoding='utf-8',check=True)
#				l = r.stdout.splitlines()
#				for s in l[1:len(l)]:
#					a = s.split()
#					if a[0] == ssid:
#						return ( {"status":1,"message":"connected({0})".format(ssid)} if a[3] != "--" else {"status":0,"message":"error({0})".format(ssid)} )
	return {"status":0,"message":"no-connection {0}".format(ssid)}

def job_disconnect(ssid):
	r = subprocess.run(shlex.split('nmcli con'),stdout=subprocess.PIPE,encoding='utf-8',check=True)
	l = r.stdout.splitlines()
	for s in l[1:len(l)]:
		a = s.split()
		if a[0] == ssid:
			if a[3] == '--':
				return {"status":1,"message":"already-disconnected {0}".format(ssid)}
			else:
				r = subprocess.run(shlex.split('nmcli con down '+ssid),stdout=subprocess.PIPE,encoding='utf-8')
				if r.returncode == 0: return {"status":1,"message":"connected {0}".format(ssid)}
				return {"status":0,"message":"failed {0} exit-code = {1}".format(ssid,r.returncode)}
#				r = subprocess.run(shlex.split('nmcli con'),stdout=subprocess.PIPE,encoding='utf-8',check=True)
#				l = r.stdout.splitlines()
#				for s in l[1:len(l)]:
#					a = s.split()
#					if a[0] == ssid:
#						return ( {"status":1,"message":"connected({0})".format(ssid)} if a[3] == "--" else {"status":0,"message":"error({0})".format(ssid)} )
	return {"status":1,"message":"no-connection {0}".format(ssid)}

def job_update():
	r0 = subprocess.run('sudo apt-get update && sudo apt-get -y upgrade && sudo apt-get -y autoremove',encoding='utf-8',shell=True)
	if r0.returncode == 0 :
		r1 = subprocess.run('/home/sien/update-tekuteku-note.sh 1>/dev/null 2>&1',encoding='utf-8',shell=True)
		if r1.returncode == 0 : return {"status":1,"message":"done"}
		return {"status":0,"message":"error in update-tekuteku-note.sh"}
	return {"status":0,"message":"error in apt"}

if len(sys.argv) < 2 :
	print_responce({"status":0,"message":"no-job-specified"})
	sys.exit(1)

job = get_value("job",sys.argv[1])
ssid = "" if len(sys.argv) != 3 else get_value("ssid",sys.argv[2])
match job:
	case 'status':
		print_responce(job_status(0))
	case 'shutdown':
		print_responce({"status":1,"message":"shutting-down"})
		r = subprocess.run(shlex.split('shutdown now'))
	case 'rescan':
		print_responce(job_status(1))
	case 'connect':
		print_responce(job_connect(ssid))
	case 'disconnect':
		print_responce(job_disconnect(ssid))
	case 'update':
		print_responce(job_update())
	case _:
		print_responce({"status":0,"message":"unknown job={0}".format(job)})
