@echo off
if "%1" == "debug" goto :debug
if "%1" == "ssl" goto :ssl
if "%1" == "ssl-debug" goto :ssl-debug

echo release
setlocal
set CXXFLAGS= -nologo -MD -Ox -EHac -bigobj -std:c++17
set CXXINCLUDES= -I. -IC:\mnt\disk_z\usr\boost\boost_1_84_0
set CXXDEFS= -DNDEBUG -D_WINDOWS
set LDFLAGS= -link -subsystem:windows -entry:mainCRTStartup
set LIBS= -libpath:C:\mnt\disk_z\usr\boost\boost_1_84_0/stage/lib
cl %CXXFLAGS% %CXXINCLUDES% %CXXDEFS% tekuteku_server.cpp %LDFLAGS% %LIBS%
mt -manifest utf-8.manifest -outputresource:tekuteku_server.exe;#1
copy tekuteku_server.exe てくてくノートサーバ.exe /y
del tekuteku_server.exe
del tekuteku_server.obj
endlocal
exit /b

:ssl
setlocal
echo release ssl
set CXXFLAGS= -nologo -MD -Ox -EHac -bigobj -std:c++17
set CXXINCLUDES= -I. -IC:\mnt\disk_z\usr\boost\boost_1_84_0 -I"C:\Program Files\OpenSSL\include"
set CXXDEFS= -DUSE_SSL -DNDEBUG -D_WINDOWS
set LDFLAGS= -link -subsystem:windows -entry:mainCRTStartup
set LIBS= libssl.lib libcrypto.lib -libpath:C:\mnt\disk_z\usr\boost\boost_1_84_0/stage/lib -libpath:"C:\Program Files\OpenSSL\lib"
cl %CXXFLAGS% %CXXINCLUDES% %CXXDEFS% tekuteku_server.cpp %LDFLAGS% %LIBS%
mt -manifest utf-8.manifest -outputresource:tekuteku_server.exe;#1
del tekuteku_server.obj
endlocal
exit /b

:debug
setlocal
echo debug
set CXXFLAGS= -nologo -MD -Ox -EHac -bigobj -std:c++17
set CXXINCLUDES= -I. -IC:\mnt\disk_z\usr\boost\boost_1_84_0
set CXXDEFS= -DNDEBUG -D_WINDOWS
set LDFLAGS= -link -subsystem:windows -entry:mainCRTStartup -debug -incremental:no -pdb:tekuteku_server_exe.pdb -opt:ref,noicf
set LIBS= -libpath:C:\mnt\disk_z\usr\boost\boost_1_84_0/stage/lib
cl %CXXFLAGS% %CXXINCLUDES% %CXXDEFS% tekuteku_server.cpp %LDFLAGS% %LIBS%
mt -manifest utf-8.manifest -outputresource:tekuteku_server.exe;#1
del tekuteku_server.obj
endlocal
exit /b

:ssl-debug
setlocal
echo debug ssl
set CXXFLAGS= -nologo -MD -Ox -EHac -bigobj -std:c++17
set CXXINCLUDES= -I. -IC:\mnt\disk_z\usr\boost\boost_1_84_0 -I"C:\Program Files\openssl\include"
set CXXDEFS= -DUSE_SSL -DNDEBUG -D_WINDOWS
set LDFLAGS= -link -subsystem:windows -entry:mainCRTStartup -debug -incremental:no -pdb:tekuteku_server_exe.pdb -opt:ref,noicf
set LIBS= libssl.lib libcrypto.lib -libpath:C:\mnt\disk_z\usr\boost\boost_1_84_0/stage/lib -libpath:C:\mnt\disk_z\usr\boost\boost_1_84_0\stage\lib -libpath:"C:\Program Files\openssl\lib"
cl %CXXFLAGS% %CXXINCLUDES% %CXXDEFS% tekuteku_server.cpp %LDFLAGS% %LIBS%
mt -manifest utf-8.manifest -outputresource:tekuteku_server.exe;#1
del tekuteku_server.obj
endlocal
exit /b

rem # libboost_context-vc141-mt-gd-x64-1_84.lib
