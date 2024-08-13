#!/usr/bin/env python
import sys
import shlex
import subprocess

class connection_t:
	def __init__(self,name,status):
		self.name = name
		self.status = status	# 0:未接続 1:接続中 9:利用不可

known_connections = ['AUEWLAN','kodama-420','eth0','sim']
connections_wifi = []
connections_ethernet = []
# ssids = {}

r = subprocess.run(shlex.split('sudo nmcli con'),stdout=subprocess.PIPE,encoding='utf-8')
l = r.stdout.splitlines()
for s in l[1:len(l)] :
	a = s.split()
	if a[0] in known_connections :
		match a[2] :
			case 'wifi' :
				connections_wifi.append(connection_t(a[0],( 0 if a[3] == '--' else 1 )))
			case 'ethernet' :
				connections_ethernet.append(connection_t(a[0],( 0 if a[3] == '--' else 1 )))

# r = subprocess.run(shlex.split('sudo nmcli -f SSID,SIGNAL dev wifi list ifname wlan0'),stdout=subprocess.PIPE,encoding='utf-8')
# l = r.stdout.splitlines()
# for s in l[1:len(l)] :
#	a = s.split()
#	if a[0] != '--' :
#		ssids[a[0]] = a[1]
#
# for c in connections_wifi :
#	if c.name in ssids and c.status == 9 :
#		c.status = 0

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
<div class="col-4 mx-auto justify-contents-center">
<div class="alert alert-primary m-2 mx-auto text-center">インターネット接続</div>
<table class="table table-bordered align-middle m-2 mx-auto bg-secondary text-white">
'''

fmt_w_btn_top = '''\
<tr><td rowspan="{1}">WiFi</td><td>{0}</td>
<td value="{0}">未接続<span class="spinner-border spinner-border-sm ms-2" role="status" aria-hidden="true" style="display:none;"></span></td>
<td><button class="btn btn-outline-success pi-connect" value="{0}">接続する</button></td></tr>
'''

fmt_w_btn = '''\
<tr><td>{0}</td>
<td value="{0}">未接続<span class="spinner-border spinner-border-sm ms-2" role="status" aria-hidden="true" style="display:none;"></span></td>
<td><button class="btn btn-outline-success pi-connect" value="{0}">接続する</button></td></tr>
'''

for i,c in enumerate(connections_ethernet) :
	if i == 0 :
		n = len(connections_ethernet)
		match c.status :
			case 0 :
				html += '<tr><td rowspan="{1}">有線</td><td>{0}</td><td>未接続</td><td></td></tr>\n'.format(c.name,n)
			case 1 :
				html += '<tr><td rowspan="{1}">有線</td><td class="text-warning">{0}</td><td class="text-warning">接続完了</td><td></td></tr>\n'.format(c.name,n)
			case _ :
				html += '<tr><td rowspan="{1}">有線</td><td>{0}</td><td>利用不可</td><td></td></tr>\n'.format(c.name,n)
	else :
		match c.status :
			case 0 :
				html += '<tr><td>{0}</td><td>未接続</td><td></td></tr>\n'.format(c.name)
			case 1 :
				html += '<tr><td class="text-warning">{0}</td><td class="text-warning">接続完了</td><td></td></tr>\n'.format(c.name)
			case _ :
				html += '<tr><td>{0}</td><td>利用不可</td><td></td></tr>\n'.format(c.name)

for i,c in enumerate(connections_wifi) :
	if i == 0 :
		n = len(connections_wifi)
		match c.status :
			case 0 :
				html += fmt_w_btn_top.format(c.name,n)
			case 1 :
				html += '<tr><td rowspan="{1}">WiFi</td><td class="text-warning">{0}</td><td class="text-warning">接続完了</td><td></td></tr>\n'.format(c.name,n)
			case 9 :
				html += '<tr><td rowspan="{1}">WiFi</td><td>{0}</td><td>利用不可</td><td></td></tr>\n'.format(c.name,n)
	else :
		match c.status :
			case 0 :
				html += fmt_w_btn.format(c.name,n)
			case 1 :
				html += '<tr><td class="text-warning">{0}</td><td class="text-warning">接続完了</td><td></td></tr>\n'.format(c.name)
			case 9 :
				html += '<tr><td>{0}</td><td>利用不可</td><td></td></tr>\n'.format(c.name)

html += '''\
</table>
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
		$.ajax("./pi-connect.py?"+ssid,{ method: 'get', async: true, dataType: 'json' })
			.done((data)=>{	if ( data.status == 0 ) { alert("接続エラーです"); } })
			.fail(()=>{ alert("通信エラーです"); })
			.always(()=>{ location.reload(); });
		$(`td[value="${ssid}"] span.spinner-border`).show();
	});
});
</script>
</body>
</html>
'''

print(html)
