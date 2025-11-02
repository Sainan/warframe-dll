<?php
require "tools/u64_dyn.php";

function joaat(string $str): int
{
	return hexdec(hash("joaat", $str));
}

$target_version = substr(trim(explode("\n", file_get_contents("main.cpp"))[0]), 28, -1);

chdir("tools");
passthru("pluto archive.pluto");
chdir("..");

function wrap_archive($uncompressed)
{
	$bin_str = pack_u64_dyn_bp(time());
	$bin_str .= pack_u64_dyn_bp(strlen($uncompressed));
	$bin_str .= gzcompress($uncompressed, 9);
	return $bin_str;
}

$bin_str = wrap_archive(file_get_contents("archive_all.tmp"));
unlink("archive_all.tmp");

file_put_contents("owf_archive_data.inc", "static const char compressed_archive_data[] = { '\\x".join("', '\\x", array_map("dechex", array_map("ord", str_split($bin_str))))."' };");
touch("owf_repo.cpp");

if (file_exists("archive_changed.tmp"))
{
	echo ">>> Hotfix.owf only contains files changed since tag ".substr($target_version, strlen("OpenWF Bootstrapper v"))."\n";
	$bin_str = wrap_archive(file_get_contents("archive_changed.tmp"));
	unlink("archive_changed.tmp");
}

file_put_contents("Hotfix.owf", pack("V", joaat($target_version)).$bin_str);
