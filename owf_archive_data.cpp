#include "owf_repo.hpp"

#include "owf_archive_data.inc"

void owfRepo::loadBuiltinArchive()
{
	hotfix = 0;
	return loadArchive(compressed_archive_data, sizeof(compressed_archive_data));
}
