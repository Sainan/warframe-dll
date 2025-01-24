Write-Host "Fetching latest DLL version..."
$version = Invoke-RestMethod -Uri "https://openwf.io/supplementals/client%20drop-in/latest.txt" -Method Get
Write-Host "Downloading OpenWF Bootstrapper v$version..."
Invoke-WebRequest -Uri "https://openwf.io/supplementals/client%20drop-in/$version/dwmapi.dll" -OutFile "../dwmapi.dll"

Write-Host "Checking for hotfixes..."
$hotfix = Invoke-RestMethod -Uri "https://openwf.io/supplementals/client%20drop-in/latest_hotfix.txt" -Method Get
if ($hotfix -eq "") {
	Remove-Item "Hotfix.bin"
}
else {
	Write-Host "Downloading $version $hotfix..."
	Invoke-WebRequest -Uri "https://openwf.io/supplementals/client%20drop-in/$version/$hotfix/Hotfix.bin" -OutFile "Hotfix.bin"
}