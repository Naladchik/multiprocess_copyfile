@echo off
::start /B build/WorkProject.exe --help
start /B build/WorkProject.exe --source input1.txt --destination output1.txt --memory mem_shared
start /B build/WorkProject.exe --source input1.txt --destination output1.txt --memory mem_shared
pause