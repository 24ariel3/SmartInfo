@echo off
call S:\Programs\esp\esp-idf\export.bat
python %IDF_PATH%\tools\idf.py build
