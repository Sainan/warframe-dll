tools\pluto tools\make_version_rc.pluto
llvm-rc version.rc
del version.rc

php archive.php

sun _release
del dwmapi.exp
del dwmapi.lib
del version.res

tools\pluto tools\embed_checksum.pluto
