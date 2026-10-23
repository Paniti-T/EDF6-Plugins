@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist obj mkdir obj
cl /nologo /O2 /MT /EHa /W4 /std:c++17 /DUNICODE /D_UNICODE /Foobj\ CorpseCleanup.cpp ^
   /link /DLL /OUT:..\..\EDF6CorpseCleanup.dll /IMPLIB:obj\EDF6CorpseCleanup.lib /PDB:obj\EDF6CorpseCleanup.pdb /DEBUG /OPT:REF /OPT:ICF kernel32.lib
