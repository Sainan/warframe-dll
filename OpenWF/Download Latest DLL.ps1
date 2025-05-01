Write-Host "Fetching latest version..."
$latest = Invoke-RestMethod -Uri "https://openwf.io/supplementals/client%20drop-in/meta" -Method Get

$sha256 = ""
if (Test-Path "../wtsapi32.dll") {
	$sha256 = (Get-FileHash "../wtsapi32.dll" -Algorithm SHA256).Hash.ToLower()
}
$hotfix_sha256 = ""
if (Test-Path "Hotfix.bin") {
	$hotfix_sha256 = (Get-FileHash "Hotfix.bin" -Algorithm SHA256).Hash.ToLower()
}

if (Test-Path "../dwmapi.dll") {
	Remove-Item "../dwmapi.dll"
}

if ($sha256 -ne $latest.sha256 -or $hotfix_sha256 -ne $latest.hotfix_sha256) {
	if ($latest.hotfix -ne "") {
		Write-Host "Downloading OpenWF Bootstrapper v$($latest.version) $($latest.hotfix)..."
	}
	else {
		Write-Host "Downloading OpenWF Bootstrapper v$($latest.version)..."
	}

	if ($sha256 -ne $latest.sha256) {
		Invoke-WebRequest -Uri "https://openwf.io/supplementals/client%20drop-in/$($latest.version)/dwmapi.dll" -OutFile "../wtsapi32.dll"
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