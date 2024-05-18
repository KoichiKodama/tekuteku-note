#!/bin/sh
(
cd /home/sien/tekuteku-note
x=/etc/apache2/
./tekuteku_server --port $1 --ssl $x/ssl.key/sien.key $x/ssl.crt/sien.aichi-edu.ac.jp.cer $x/ssl.crt/nii-odca4g7rsa.cer `$x/ssl.key/pass_ssl.sh` --magic tekuteku
)

