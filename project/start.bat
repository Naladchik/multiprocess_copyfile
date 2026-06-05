@echo off
rem Open the server in its own window so it has a separate console.
start "Server" build\Release\WorkProject.exe -r server

rem Wait 1 second to give the server time to start listening before
rem the client tries to connect.
timeout /t 1 /nobreak > nul

rem Open the client in its own window so the user can type in it.
start "Client" build\Release\WorkProject.exe -r client
