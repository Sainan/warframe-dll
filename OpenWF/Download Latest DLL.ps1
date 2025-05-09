Write-Host "Fetching latest version..."
$latest = Invoke-RestMethod -Uri "https://openwf.io/supplementals/client%20drop-in/meta" -Method Get

$dll_path = "../dwmapi.dll"
if (Test-Path "../wtsapi32.dll") {
	$dll_path = "../wtsapi32.dll"
}
if (Test-Path "../version.dll") {
	$dll_path = "../version.dll"
}

$sha256 = ""
if (Test-Path $dll_path) {
	$sha256 = (Get-FileHash $dll_path -Algorithm SHA256).Hash.ToLower()
}
$hotfix_sha256 = ""
if (Test-Path "Hotfix.bin") {
	$hotfix_sha256 = (Get-FileHash "Hotfix.bin" -Algorithm SHA256).Hash.ToLower()
}

if ($sha256 -ne $latest.sha256 -or $hotfix_sha256 -ne $latest.hotfix_sha256) {
	if ($latest.hotfix -ne "") {
		Write-Host "Downloading OpenWF Bootstrapper v$($latest.version) $($latest.hotfix)..."
	}
	else {
		Write-Host "Downloading OpenWF Bootstrapper v$($latest.version)..."
	}

	if ($sha256 -ne $latest.sha256) {
		Invoke-WebRequest -Uri "https://openwf.io/supplementals/client%20drop-in/$($latest.version)/dwmapi.dll" -OutFile $dll_path
	}
	if ($hotfix_sha256 -ne $latest.hotfix_sha256) {
		if ($latest.hotfix -ne "") {
			Invoke-WebRequest -Uri "https://openwf.io/supplementals/client%20drop-in/$($latest.version)/$($latest.hotfix)/Hotfix.bin" -OutFile "Hotfix.bin"
		}
		else {
			Remove-Item "Hotfix.bin"
		}
	}
}
else {
	Write-Host "Your OpenWF Bootstrapper is up-to-date."
	Start-Sleep -Seconds 2
}