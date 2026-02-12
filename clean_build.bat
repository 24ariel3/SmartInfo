@echo off
rmdir /s /q build
del sdkconfig
call S:\Programs\esp\esp-idf\export.bat
python %IDF_PATH%\tools\idf.py build
