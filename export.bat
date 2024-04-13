@echo off
if exist てくてくノート.zip del てくてくノート.zip
7z a てくてくノート.zip tools index.html tekuteku-icon.png tekuteku-icon.ico てくてくノートサーバ.exe -x!tools\token.json 1>NUL
