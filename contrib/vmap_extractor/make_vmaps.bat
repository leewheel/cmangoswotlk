@echo off
chcp 65001 >nul
cls
echo.
echo                    vmap 提取与组装工具
echo.
echo 目录规则:
echo   ClientData\Buildings  vmap 原始数据
echo   ClientData\vmaps      vmap 组装结果
echo.
echo 磁盘空间提示:
echo   建议至少预留 2 GB 可用空间。
echo.
choice /c 10 /n /m "请输入 1 继续提取，输入 0 退出："
if errorlevel 2 (
echo 已取消提取。
pause
exit /b 0
)
cls
echo.
echo.
echo.
echo [1/2] 提取 vmap 原始数据到 ClientData\Buildings...
IF EXIST ClientData\Buildings\dir (ECHO 检测到旧的 ClientData\Buildings，按任意键清理并继续。
pause>nul
RMDIR /S /Q ClientData\Buildings)
vmap_extractor.exe
IF NOT %ERRORLEVEL% LEQ 1 (echo vmap_extractor.exe 执行失败。
echo 按任意键继续 . . .
pause>nul
exit /b 1)
IF NOT EXIST ClientData\Buildings\dir_bin (
echo [失败] 未生成 ClientData\Buildings\dir_bin。
echo 按任意键继续 . . .
pause>nul
exit /b 1
)
for /f %%C in ('dir /b /a-d "ClientData\Buildings" 2^>nul ^| find /c /v ""') do set BUILDING_COUNT=%%C
echo [完成] vmap 原始文件: %BUILDING_COUNT% 个
cls
echo.
echo.
echo.
echo [2/2] 组装 ClientData\Buildings 到 ClientData\vmaps...
echo 按任意键开始组装 vmaps . . .
pause>nul
IF EXIST ClientData\vmaps RMDIR /S /Q ClientData\vmaps
md ClientData\vmaps
vmap_assembler.exe ClientData\Buildings ClientData\vmaps
IF NOT %ERRORLEVEL% LEQ 1 (echo vmap_assembler.exe 执行失败。
echo 按任意键继续 . . .
pause>nul
exit /b 1)
IF NOT EXIST ClientData\vmaps (
echo [失败] 未生成 ClientData\vmaps。
echo 按任意键继续 . . .
pause>nul
exit /b 1
)
for /f %%C in ('dir /b /a-d "ClientData\vmaps\*.vmtree" 2^>nul ^| find /c /v ""') do set VMTREE_COUNT=%%C
for /f %%C in ('dir /b /a-d "ClientData\vmaps\*.vmtile" 2^>nul ^| find /c /v ""') do set VMTILE_COUNT=%%C
cls
echo.
echo.
echo.
echo 处理完成！请复制 ClientData\vmaps 到 MaNGOS 运行目录。
echo 统计:
echo   vmap 原始文件: %BUILDING_COUNT%
echo   vmtree 文件:   %VMTREE_COUNT%
echo   vmtile 文件:   %VMTILE_COUNT%
echo 按任意键退出 . . .
pause>nul
