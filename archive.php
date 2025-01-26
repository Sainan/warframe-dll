<?php
$uncompressed = "";

function joaat(string $str): int
{
	return hexdec(hash("joaat", $str));
}

function add_file_to_archive($path)
{
	global $uncompressed;
	echo $path."\n";
	$cont = file_get_contents($path);
	$uncompressed .= pack("V", joaat($path));
	$uncompressed .= pack("V", strlen($cont));
	$uncompressed .= $cont;
}

function add_folder_to_archive($base)
{
	foreach (scandir($base) as $file)
	{
		$path = $base.$file;
		if (is_file($path))
		{
			add_file_to_archive($path);
		}
	}

}

add_folder_to_archive("OpenWF/");
add_folder_to_archive("OpenWF/samples/");
add_folder_to_archive("OpenWF/webui_dicts/");

$bin_str = gzcompress($uncompressed);
file_put_contents("owf_archive_data.hpp", "static const char compressed_archive_data[] = { '\\x".join("', '\\x", array_map("dechex", array_map("ord", str_split($bin_str))))."' };");
touch("owf_archive.cpp");

$target_version = substr(trim(explode("\n", file_get_contents("main.cpp"))[0]), 28, -1);
file_put_contents("Hotfix.bin", pack("V", joaat($target_version)).$bin_str);
