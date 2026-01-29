@echo off
::start /B build/WorkProject.exe --help
start /B build/Debug/WorkProject.exe --source input1_initial.txt --destination output4.txt --memory mem_shared
start /B build/Debug/WorkProject.exe --source input1.txt --destination output1.txt --memory mem_shared
start /B build/Debug/WorkProject.exe --source input1.txt --destination output2.txt --memory mem_shared
start /B build/Debug/WorkProject.exe --source input1.txt --destination output1.txt --memory mem_shared
start /B build/Debug/WorkProject.exe --source input1_initial.txt --destination output4.txt --memory mem_shared
start /B build/Debug/WorkProject.exe --source input1.txt --destination output2.txt --memory mem_shared
start /B build/Debug/WorkProject.exe --source input1.txt --destination output2.txt --memory mem_shared
start /B build/Debug/WorkProject.exe --source input1.txt --destination output3.txt --memory mem_shared
pause