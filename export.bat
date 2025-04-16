@echo off
if exist てくてくノート.zip del てくてくノート.zip
call 7z a -x!tools\token.json てくてくノート.zip index.html tekuteku.ico てくてくノートサーバ.exe tools 1>NUL
