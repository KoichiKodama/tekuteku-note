@echo off
if exist tekuteku-note.zip del tekuteku-note.zip
call 7z a tekuteku-note.zip index.html tekuteku.ico emoji-smile.svg てくてくノートサーバ.exe tools 1>NUL
