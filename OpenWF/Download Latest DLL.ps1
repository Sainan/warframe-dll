Write-Host "Fetching latest version..."
$latest = Invoke-RestMethod -Uri "https://openwf.io/supplementals/client%20drop-in/meta" -Method Get

$dll_path = "../wtsapi32.dll"
if (Test-Path "../dwmapi.dll") {
	$dll_path = "../dwmapi.dll"
}
if (Test-Path "../version.dll") {
	$dll_path = "../version.dll"
}

$sha256 = ""
if (Test-Path $dll_path) {
	$sha256 = (Get-FileHash $dll_path -Algorithm SHA256).Hash.ToLower()
}
$hotfix_sha256 = ""
if (Test-Path "Hotfix.owf") {
	$hotfix_sha256 = (Get-FileHash "Hotfix.owf" -Algorithm SHA256).Hash.ToLower()
}

$sideloadify_sha256 = ""
if (Test-Path "sideloadify-cli.cache") {
	$sideloadify_sha256 = (Get-FileHash "sideloadify-cli.cache" -Algorithm SHA256).Hash
}
if ($sideloadify_sha256 -ne "EE990FF8366E49DE5AF5A80DB845A9BFE9285092365E3A887FDFC76EB20C61AF") {
	Invoke-WebRequest -Uri "https://github.com/Sainan/Sideloadify/releases/download/1.1.0/sideloadify-cli.exe" -OutFile "sideloadify-cli.cache"
}

$should_sleep = $false

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
			Invoke-WebRequest -Uri "https://openwf.io/supplementals/client%20drop-in/$($latest.version)/$($latest.hotfix)/Hotfix.owf" -OutFile "Hotfix.owf"
		}
		else {
			Remove-Item "Hotfix.owf"
		}
	}
}
else {
	Write-Host "Your OpenWF Bootstrapper is up-to-date."
	$should_sleep = $true
}

Rename-Item -Path "sideloadify-cli.cache" -NewName "sideloadify-cli.exe"
$sideloadified = (./sideloadify-cli.exe ../Warframe.x64.exe).Contains(" has successfully been sideloadified.")
Rename-Item -Path "sideloadify-cli.exe" -NewName "sideloadify-cli.cache"
if ($sideloadified) {
	Write-Host "Warframe.x64.exe has successfully been sideloadified."
	$should_sleep = $true
}
if ($should_sleep) {
	Start-Sleep -Seconds 2
}
