#!/usr/bin/env python
import sys
import shlex
import subprocess
import json

def get_value(key,text):
	a = text.strip().split('=')
	if len(a) != 2:
		return "undefined"
	return a[1] if a[0] == key else "undefined"

class connection_t:
	def __init__(self,name,device,status,addr):
		self.name = name
		self.device = device
		self.status = status	# 0:未接続 1:接続中 9:利用不可
		self.addr = addr

class connection_t_encoder(json.JSONEncoder):
	def default(self, obj):
		if isinstance(obj,connection_t):
			return {"name":obj.name,"device":obj.device,"status":obj.status,"addr":obj.addr}
		else:
			return super().default(obj)

wifi_dev = 'wlan0'
known_connections = ['AUEWLAN','AUEWS','AUEWT','auewlan@sien','kodama-420','eth0','sim','sim-nttpc','kodama-home']
connections_wifi = []
connections_ethernet = []

def print_responce(responce):
	print('Content-Type: text/plain\n') # 空行を入れる
	print(json.dumps(responce,cls=connection_t_encoder))

def job_status(force_rescan):
	r = subprocess.run(shlex.split('sudo nmcli con'),stdout=subprocess.PIPE,encoding='utf-8')
	l = r.stdout.splitlines()
	for s in l[1:len(l)]:
		a = s.split()
		name = a[0]
		kind = a[2]
		device = a[3]
		if name in known_connections:
			status = ( 0 if device == '--' else 1 )
			addr = ''
			if status == 1:
				cmd = 'ifconfig {0} | awk \'$1=="inet"{{print $2;}}\''.format(device)
				r = subprocess.run(cmd,shell=True,stdout=subprocess.PIPE,encoding='utf-8')
				addr = r.stdout.strip('\n')
			match kind:
				case 'wifi':
					connections_wifi.append(connection_t(name,device,status,addr))
				case 'ethernet'|'gsm':
					connections_ethernet.append(connection_t(name,device,status,addr))

	wifi_found = {}
	o = "-rescan yes" if force_rescan == 1 else ""
	r = subprocess.run(shlex.split('sudo nmcli -f SSID,SIGNAL dev wifi list ifname {0} {1}'.format(wifi_dev,o)),stdout=subprocess.PIPE,encoding='utf-8')
	l = r.stdout.splitlines()
	for s in l[1:len(l)]:
		a = s.split()
		if a[0] != '--':
			wifi_found[a[0]] = a[1]
	for c in connections_wifi:
		if c.name not in wifi_found:
			c.status = 9

	connections_ethernet.sort(key=lambda x: x.name)
	connections_wifi.sort(key=lambda x: x.name)
	return { "status":1, "ethernet":connections_ethernet, "wifi":connections_wifi }

def job_connect(ssid):
	r = subprocess.run(shlex.split('sudo nmcli con'),stdout=subprocess.PIPE,encoding='utf-8',check=True)
	l = r.stdout.splitlines()
	for s in l[1:len(l)]:
		a = s.split()
		if a[0] == ssid:
			if a[3] != '--':
				return {"status":1,"message":"already-connected({0})".format(ssid)}
			else:
				r = subprocess.run(shlex.split('sudo nmcli con up '+ssid),stdout=subprocess.PIPE,encoding='utf-8')
				if r.returncode != 0:
					return {"status":0,"message":"failed-in-con-up({0})".format(ssid)}
				r = subprocess.run(shlex.split('sudo nmcli con'),stdout=subprocess.PIPE,encoding='utf-8',check=True)
				l = r.stdout.splitlines()
				for s in l[1:len(l)]:
					a = s.split()
					if a[0] == ssid:
						return ( {"status":1,"message":"connected({0})".format(ssid)} if a[3] != "--" else {"status":0,"message":"error({0})".format(ssid)} )
	return {"status":0,"message":"no-such-connection({0})".format(ssid)}

def job_disconnect(ssid):
	r = subprocess.run(shlex.split('sudo nmcli con'),stdout=subprocess.PIPE,encoding='utf-8',check=True)
	l = r.stdout.splitlines()
	for s in l[1:len(l)]:
		a = s.split()
		if a[0] == ssid:
			if a[3] == '--':
				return {"status":1,"message":"already-disconnected({0})".format(ssid)}
			else:
				r = subprocess.run(shlex.split('sudo nmcli con down '+ssid),stdout=subprocess.PIPE,encoding='utf-8')
				if r.returncode != 0:
					return {"status":0,"message":"failed-in-con-down({0})".format(ssid)}
				r = subprocess.run(shlex.split('sudo nmcli con'),stdout=subprocess.PIPE,encoding='utf-8',check=True)
				l = r.stdout.splitlines()
				for s in l[1:len(l)]:
					a = s.split()
					if a[0] == ssid:
						return ( {"status":1,"message":"connected({0})".format(ssid)} if a[3] == "--" else {"status":0,"message":"error({0})".format(ssid)} )
	return {"status":1,"message":"no-such-connection({0})".format(ssid)}

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
		r = subprocess.run(shlex.split('sudo shutdown now'))
	case 'rescan':
		print_responce(job_status(1))
	case 'connect':
		print_responce(job_connect(ssid))
	case 'disconnect':
		print_responce(job_disconnect(ssid))
	case _:
		print_responce({"status":0,"message":"unknown job={0}".format(job)})
