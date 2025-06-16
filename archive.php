<?php
// Do a translation update so all target dicts have all source strings
{
	$ogdir = getcwd();
	chdir("modules/openwf-translations");
	require "modules/openwf-translations/update.php";
	chdir($ogdir);
}

$uncompressed = "";

function joaat(string $str): int
{
	return hexdec(hash("joaat", $str));
}

function add_file_to_archive($path, $archive_path)
{
	global $uncompressed;
	echo $path." -> ".$archive_path;
	$cont = file_get_contents($path);
	if ($path == "OpenWF/runtime.pluto")
	{
		echo " [compiled]";
		passthru("tools\plutoc ".escapeshellarg($path));
		$cont = file_get_contents("plutoc.out") or die("Failed to comile $path");
		unlink("plutoc.out");
	}
	else if (substr($path, -5) == ".json" && $path != "OpenWF/Hotkeys.json")
	{
		echo " [minified]";
		passthru("tools\pluto tools/json_minify.pluto ".escapeshellarg($path)." min.json");
		$cont = file_get_contents("min.json") or die("Failed to minify $path");
		unlink("min.json");
	}
	echo "\n";
	$uncompressed .= pack("V", joaat($archive_path));
	$uncompressed .= pack("V", strlen($cont));
	$uncompressed .= $cont;
}

function add_folder_to_archive($base, $archive_base)
{
	foreach (scandir($base) as $file)
	{
		$path = $base.$file;
		if (is_file($path))
		{
			$archive_path = $archive_base.$file;
			add_file_to_archive($path, $archive_path);
		}
	}
}

function pack_u64_dyn_v2($val)
{
	return base64_decode(exec("tools\pluto tools\pack_u64_dyn_v2.pluto ".$val));
}

add_folder_to_archive("OpenWF/", "OpenWF/");
add_folder_to_archive("OpenWF/helpers/", "OpenWF/helpers/");
add_folder_to_archive("OpenWF/samples/", "OpenWF/samples/");
add_folder_to_archive("modules/openwf-translations/bootstrapper/", "OpenWF/translations/");

$bin_str = pack_u64_dyn_v2(strlen($uncompressed));
strlen($bin_str) == 3 or die();
$bin_str .= pack_u64_dyn_v2(time());
strlen($bin_str) == 8 or die();
$bin_str .= gzcompress($uncompressed, 9);
file_put_contents("owf_archive_data.hpp", "static const char compressed_archive_data[] = { '\\x".join("', '\\x", array_map("dechex", array_map("ord", str_split($bin_str))))."' };");
touch("owf_archive.cpp");

$target_version = substr(trim(explode("\n", file_get_contents("main.cpp"))[0]), 28, -1);
file_put_contents("Hotfix.owf", pack("V", joaat($target_version)).$bin_str);
