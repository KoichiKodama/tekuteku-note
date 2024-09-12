#!/usr/bin/env python
import sys
import shlex
import subprocess

class connection_t:
	def __init__(self,name,device,status,addr):
		self.name = name
		self.device = device
		self.status = status	# 0:未接続 1:接続中 9:利用不可
		self.addr = addr

known_connections = ['AUEWLAN','kodama-420','eth0','sim','kodama-home']
connections_wifi = []
connections_ethernet = []

r = subprocess.run(shlex.split('sudo nmcli con'),stdout=subprocess.PIPE,encoding='utf-8')
l = r.stdout.splitlines()
for s in l[1:len(l)] :
	a = s.split()
	name = a[0]
	kind = a[2]
	device = a[3]
	if name in known_connections :
		status = ( 0 if device == '--' else 1 )
		addr = ''
		if status == 1 :
			cmd = 'ifconfig {0} | awk \'$1=="inet"{{print $2;}}\''.format(device)
			r = subprocess.run(cmd,shell=True,stdout=subprocess.PIPE,encoding='utf-8')
			addr = r.stdout
		match kind :
			case 'wifi' :
				connections_wifi.append(connection_t(name,device,status,addr))
			case 'ethernet' :
				connections_ethernet.append(connection_t(name,device,status,addr))

wifi_found = {}
r = subprocess.run(shlex.split('sudo nmcli -f SSID,SIGNAL dev wifi list ifname wlan0 -rescan yes'),stdout=subprocess.PIPE,encoding='utf-8')
l = r.stdout.splitlines()
for s in l[1:len(l)] :
	a = s.split()
	if a[0] != '--' :
		wifi_found[a[0]] = a[1]
for c in connections_wifi :
	if c.name not in wifi_found :
		c.status = 9

connections_ethernet.sort(key=lambda x: x.name)
connections_wifi.sort(key=lambda x: x.name)

html = '''\
Content-Type: text/html

<!doctype html>
<html lang="ja" data-bs-theme="dark">
<head>
<meta charset="utf-8">
<title>tekuteku-pi control</title>
<link href='./tools/bootstrap-5.3.0/css/bootstrap.min.css' rel='stylesheet'>
<meta name='viewport' content='width=device-width,initial-scale=1,shrink-to-fit=no'>
</head>
<body>
<div class="container">
<div class="col-6 mx-auto justify-contents-center">
<div class="alert alert-primary m-2 mx-auto text-center">インターネット接続</div>
<table class="table table-bordered align-middle m-2 mx-auto bg-secondary text-white">
'''

fmt_connect_top = '''\
<tr><td rowspan="{2}">WiFi</td><td>{0}</td><td value="{0}">未接続</td><td>{1}</td>
<td><button class="btn btn-outline-success pi-connect" value="{0}">接続する</button></td></tr>
'''

fmt_connect = '''\
<tr><td>{0}</td><td value="{0}">未接続</td><td>{1}</td>
<td><button class="btn btn-outline-success pi-connect" value="{0}">接続する</button></td></tr>
'''

fmt_disconnect_top = '''\
<tr><td rowspan="{2}">WiFi</td><td class="text-warning">{0}</td><td class="text-warning" value="{0}">接続完了</td><td class="text-warning">{1}</td>
<td><button class="btn btn-outline-success pi-disconnect" value="{0}">切断する</button></td></tr>
'''

fmt_disconnect = '''\
<tr><td class="text-warning">{0}</td><td class="text-warning" value="{0}">接続完了</td><td class="text-warning">{1}</td>
<td><button class="btn btn-outline-success pi-disconnect" value="{0}">切断する</button></td></tr>
'''

for i,c in enumerate(connections_ethernet) :
	if i == 0 :
		n = len(connections_ethernet)
		match c.status :
			case 0 :
				html += '<tr><td rowspan="{2}">有線</td><td>{0}</td><td>未接続</td><td>{1}</td><td></td></tr>\n'.format(c.name,c.addr,n)
			case 1 :
				html += '<tr><td rowspan="{2}">有線</td><td class="text-warning">{0}</td><td class="text-warning">接続完了</td><td class="text-warning">{1}</td><td></td></tr>\n'.format(c.name,c.addr,n)
			case _ :
				html += '<tr><td rowspan="{2}">有線</td><td>{0}</td><td>利用不可</td><td>{1}</td><td></td></tr>\n'.format(c.name,c.addr,n)
	else :
		match c.status :
			case 0 :
				html += '<tr><td>{0}</td><td>未接続</td><td>{1}</td><td></td></tr>\n'.format(c.name,c.addr)
			case 1 :
				html += '<tr><td class="text-warning">{0}</td><td class="text-warning">接続完了</td><td class="text-warning">{1}</td><td></td></tr>\n'.format(c.name,c.addr)
			case _ :
				html += '<tr><td>{0}</td><td>利用不可</td><td>{1}</td><td></td></tr>\n'.format(c.name,c.addr)

for i,c in enumerate(connections_wifi) :
	if i == 0 :
		n = len(connections_wifi)
		match c.status :
			case 0 :
				html += fmt_connect_top.format(c.name,c.addr,n)
			case 1 :
				html += fmt_disconnect_top.format(c.name,c.addr,n)
			case 9 :
				html += '<tr><td rowspan="{2}">WiFi</td><td>{0}</td><td>利用不可</td><td>{1}</td><td></td></tr>\n'.format(c.name,c.addr,n)
	else :
		match c.status :
			case 0 :
				html += fmt_connect.format(c.name,c.addr,n)
			case 1 :
				html += fmt_disconnect.format(c.name,c.addr)
			case 9 :
				html += '<tr><td>{0}</td><td>利用不可</td><td>{1}</td><td></td></tr>\n'.format(c.name,c.addr)

html += '''\
</table>
<div class="alert alert-dark m-2 mx-auto text-center">WiFiの接続先を変えるとラズパイとの接続は切断されます。<br>その場合は再接続して下さい。</div>
<div class="alert alert-dark m-2 mx-auto text-center">この<a href="./ssl-keys/tekuteku-pi.cer">自己証明書</a>をインストールすると警告は出なくなります。</div>
</div>
</div>
<script src='./tools/jquery-3.6.0/jquery-3.6.0.min.js'></script>
<script src='./tools/bootstrap-5.3.0/js/bootstrap.bundle.min.js'></script>
<script src='./tools/sprintf-1.1.2/sprintf.min.js'></script>
<script type="text/javascript">
'use strict';
$(document).ready( function(){
	$('button.pi-connect').on('click',function(){
		let ssid = $(this).val();
		$.ajax("./pi-control.py?connect+"+ssid,{ method: 'get', async: true, dataType: 'json' })
			.done((data)=>{	if ( data.status == 0 ) { alert("接続エラーです"); } })
			.fail(()=>{ alert("通信エラーです"); })
			.always(()=>{ location.reload(); });
		$(`td[value="${ssid}"]`).html('接続中<span class="spinner-border spinner-border-sm ms-2" role="status" aria-hidden="true"></span>');
	});
	$('button.pi-disconnect').on('click',function(){
		let ssid = $(this).val();
		$.ajax("./pi-control.py?disconnect+"+ssid,{ method: 'get', async: true, dataType: 'json' })
			.done((data)=>{	if ( data.status == 0 ) { alert("接続エラーです"); } })
			.fail(()=>{ alert("通信エラーです"); })
			.always(()=>{ location.reload(); });
		$(`td[value="${ssid}"]`).html('切断中<span class="spinner-border spinner-border-sm ms-2" role="status" aria-hidden="true"></span>');
	});
});
</script>
</body>
</html>
'''

print(html)
