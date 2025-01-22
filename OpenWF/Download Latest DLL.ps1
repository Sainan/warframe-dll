Write-Host "Fetching latest DLL version..."
$version = Invoke-RestMethod -Uri "https://openwf.io/supplementals/client%20drop-in/latest.txt" -Method Get
Write-Host "Downloading OpenWF Bootstrapper v$version..."
Invoke-WebRequest -Uri "https://openwf.io/supplementals/client%20drop-in/$version/dwmapi.dll" -OutFile "../dwmapi.dll"