@echo off
if exist てくてくノート.zip del てくてくノート.zip
if exist export.work rmdir /s /q export.work
mkdir export.work
pushd export.work
copy ..\index.html . 1>NUL
copy ..\tekuteku.ico . 1>NUL
copy ..\emoji-smile.svg . 1>NUL
copy ..\てくてくノートサーバ.exe . 1>NUL
copy ..\config-yy.json .\config.json 1>NUL
copy ..\yy-service.exe . 1>NUL
copy ..\yy.ico . 1>NUL
robocopy ..\tools .\tools /s 1>NUL
call 7z a -scsWIN ..\てくてくノート.zip * 1>NUL
popd
rmdir /s /q export.work
