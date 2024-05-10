#!/bin/sh
(
cd /home/sien/tekuteku-note
x=`/etc/apache2/ssl.key/pass_ssl.sh`
nohup ./tekuteku_server --port 8080 --ssl /etc/apache2/ssl.key/sien.key /etc/apache2/ssl.crt/sien.aichi-edu.ac.jp.cer /etc/apache2/ssl.crt/nii-odca4g7rsa.cer $x 1>/dev/null 2>&1 &
nohup ./tekuteku_server --port 8081 --ssl /etc/apache2/ssl.key/sien.key /etc/apache2/ssl.crt/sien.aichi-edu.ac.jp.cer /etc/apache2/ssl.crt/nii-odca4g7rsa.cer $x 1>/dev/null 2>&1 &
nohup ./tekuteku_server --port 8082 --ssl /etc/apache2/ssl.key/sien.key /etc/apache2/ssl.crt/sien.aichi-edu.ac.jp.cer /etc/apache2/ssl.crt/nii-odca4g7rsa.cer $x 1>/dev/null 2>&1 &
nohup ./tekuteku_server --port 8083 --ssl /etc/apache2/ssl.key/sien.key /etc/apache2/ssl.crt/sien.aichi-edu.ac.jp.cer /etc/apache2/ssl.crt/nii-odca4g7rsa.cer $x 1>/dev/null 2>&1 &
nohup ./tekuteku_server --port 8084 --ssl /etc/apache2/ssl.key/sien.key /etc/apache2/ssl.crt/sien.aichi-edu.ac.jp.cer /etc/apache2/ssl.crt/nii-odca4g7rsa.cer $x 1>/dev/null 2>&1 &
)
ps -e | grep tekuteku_server

