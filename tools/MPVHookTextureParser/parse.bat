@ECHO OFF

SET TEXTURE_DATA=__Resolved.txt

python resolveTexture.py %1 %TEXTURE_DATA%

IF %ERRORLEVEL% NEQ 0 (
    ECHO resolveTexture.py ʧ��
    PAUSE
    EXIT 1
)

MPVHookTextureParser %TEXTURE_DATA% %2

IF %ERRORLEVEL% NEQ 0 (
    ECHO ���� DDS ʧ��
    PAUSE
    EXIT 1
)

DEL %TEXTURE_DATA%
