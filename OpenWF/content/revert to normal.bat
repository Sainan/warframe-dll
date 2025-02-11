@echo off
if exist H.Cache.bin!D_---------------------w (
	if exist H.Cache.bin!D_---------------------w.modded (
		del H.Cache.bin!D_---------------------w.modded
	)
	ren H.Cache.bin!D_---------------------w H.Cache.bin!D_---------------------w.modded
)

echo Done. However, please don't delete any files in this folder.
pause > nul
