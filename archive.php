<?php
require "tools/u64_dyn.php";

function joaat(string $str): int
{
	return hexdec(hash("joaat", $str));
}

chdir("tools");
passthru("pluto archive.pluto");
chdir("..");
$uncompressed = file_get_contents("archive.tmp");
unlink("archive.tmp");

$bin_str = pack_u64_dyn_bp(strlen($uncompressed));
strlen($bin_str) == 3 or die();
$bin_str .= pack_u64_dyn_bp(time());
strlen($bin_str) == 8 or die();
$bin_str .= gzcompress($uncompressed, 9);
file_put_contents("owf_archive_data.inc", "static const char compressed_archive_data[] = { '\\x".join("', '\\x", array_map("dechex", array_map("ord", str_split($bin_str))))."' };");
touch("owf_archive.cpp");

$target_version = substr(trim(explode("\n", file_get_contents("main.cpp"))[0]), 28, -1);
file_put_contents("Hotfix.owf", pack("V", joaat($target_version)).$bin_str);
