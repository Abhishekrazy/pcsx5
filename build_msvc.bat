@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -B I:\Personal\Windows\pcsx5\build -S I:\Personal\Windows\pcsx5