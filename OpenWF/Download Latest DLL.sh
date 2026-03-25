#!/usr/bin/env bash

echo "Fetching latest version..."
latest_version=$(curl -s "https://openwf.io/supplementals/client%20drop-in/latest.txt")
latest_hotfix=$(curl -s "https://openwf.io/supplementals/client%20drop-in/latest_hotfix.txt")
latest_sha256=$(curl -s "https://openwf.io/supplementals/client%20drop-in/sha256.txt")
latest_hotfix_sha256=$(curl -s "https://openwf.io/supplementals/client%20drop-in/sha256_hotfix.txt")

dll_path="../wtsapi32.dll"
if [[ -f "../dwmapi.dll" ]]; then
	dll_path="../dwmapi.dll"
elif [[ -f "../version.dll" ]]; then
	dll_path="../version.dll"
elif [[ ! -f "../Launch with OpenWF.sh" ]]; then
	echo "WINEDLLOVERRIDES=\"wtsapi32.dll=n,b\" wine Warframe.x64.exe" > "../Launch with OpenWF.sh"
	chmod +x "../Launch with OpenWF.sh"
fi

sha256=""
hotfix_sha256=""
if [[ -f "$dll_path" ]]; then
	sha256=$(sha256sum "$dll_path" | awk '{print $1}')
fi
if [[ -f "Hotfix.owf" ]]; then
	hotfix_sha256=$(sha256sum "Hotfix.owf" | awk '{print $1}')
fi

if [[ "$sha256" != "$latest_sha256" || "$hotfix_sha256" != "$latest_hotfix_sha256" ]]; then
	if [[ -n "$latest_hotfix" ]]; then
		echo "Downloading OpenWF Bootstrapper v$latest_version $latest_hotfix..."
	else
		echo "Downloading OpenWF Bootstrapper v$latest_version..."
	fi

	if [[ "$sha256" != "$latest_sha256" ]]; then
		curl -L "https://openwf.io/supplementals/client%20drop-in/$latest_version/dwmapi.dll" -o "$dll_path"
	fi

	if [[ "$hotfix_sha256" != "$latest_hotfix_sha256" ]]; then
		if [[ -n "$latest_hotfix" ]]; then
			curl -L "https://openwf.io/supplementals/client%20drop-in/$latest_version/${latest_hotfix// /%20}/Hotfix.owf" -o "Hotfix.owf"
		else
			rm -f "Hotfix.owf"
		fi
	fi
else
	echo "Your OpenWF Bootstrapper is up-to-date."
fi
