@ECHO OFF
mode con cols=71 lines=60
COLOR 0F
TITLE Orange Tool命令行

:CMD
CLS
ECHOC {0E}=--------------------------------------------------------------------={0F}{\n}
ECHO.
ECHOC {0C}                          Orange Tool命令行                                                           {0F}{\n}
ECHOC {0E}=--------------------------------------------------------------------={0F}{\n}
ECHO.
ECHO.
ECHOC {0A} ADB命令{0F}   adb devices                   查看设备{\n}
ECHOC            adb reboot recovery               重启到recovery模式{\n}
ECHOC            adb reboot bootloader             重启到fastboot模式{\n} 
ECHOC {0E}=-------------------------------------------------------------------={0F}{\n}
ECHOC {0A} FASTBOOT{0F}  fastboot reboot                   重启{\n}
ECHOC            fastboot devices              查看设备{\n}
ECHOC            fastboot oem edl                 进入9008模式{\n}
ECHOC            fastboot erase data             清除data分区(双清data){\n}
ECHOC            fastboot flash boot          刷入boot分区{\n}
ECHOC            fastboot set_active a/b       切换ab分区{\n}
ECHOC            fastboot flash modeam_ab      刷入基带NV{\n}
ECHOC            fastboot flash init_boot     刷入init_boot分区{\n}
ECHOC            fastboot flash recovery      刷入TWRP{\n}
ECHOC            fastboot reboot recovery          重启到recovery模式{\n}
ECHOC            fastboot reboot bootloader        重启到fastboot模式{\n}
ECHOC            fastboot oem lks                   BL1解锁命令（0为解锁）{\n}
ECHOC            fastboot oem device-info       查看BL锁false解锁，true为锁定{\n}
ECHOC {0B}=--------------------------------------------------------------------={0F}{\n}
ECHO. 
:CMD-CONTINUE
ECHOC {0B}=--------------------------------------------------------------------={0F}{\n}
ECHOC {0E}[请输入命令]{0F}
set /p cmd=
if "%cmd%"=="v" (
    tasklist | find "mmc.exe" 1>nul 2>nul && ECHOC {0A}                                                 [设备管理器已打开]{0F}{\n}&& goto CMD-CONTINUE
    start %windir%\system32\devmgmt.msc & goto CMD-CONTINUE)
if "%cmd%"=="" (
    ECHOC {0C}                                                        [命令不能为空]{0F}{\n}
    goto CMD-CONTINUE
)
if "%cmd%"=="cls" goto CMD
if "%cmd%"=="CLS" goto CMD
if "%cmd%"=="exit" exit
if "%cmd%"=="EXIT" exit
rem 这里可以添加cc命令的处理逻辑
if "%cmd%"=="cc" goto CMD-CONTINUE

rem 执行用户输入的命令
%cmd%
if %ERRORLEVEL% EQU 0 (
    ECHOC {0A}                                                      [命令执行成功]{0F}{\n}
) else (
    ECHOC {0C}                                                      [命令执行失败]{0F}{\n}
)
goto CMD-CONTINUE