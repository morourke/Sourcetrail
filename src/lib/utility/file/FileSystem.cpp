#include "FileSystem.h"

#include <chrono>
#include <set>

#include <boost/date_time.hpp>
#include <boost/date_time/c_local_time_adjustor.hpp>

#include "utilityString.h"

std::vector<FilePath> FileSystem::getFilePathsFromDirectory(
	const FilePath& path, const std::vector<std::wstring>& extensions)
{
	std::set<std::wstring> ext(extensions.begin(), extensions.end());
	std::vector<FilePath> files;

	if (path.isDirectory())
	{
		std::filesystem::recursive_directory_iterator it(path.getPath());
		std::filesystem::recursive_directory_iterator endit;
		while (it != endit)
		{
			if (std::filesystem::is_symlink(*it))
			{
				// check for self-referencing symlinks
				std::filesystem::path p = std::filesystem::read_symlink(*it);
				if (p.filename() == p.string() && p.filename() == it->path().filename())
				{
					++it;
					continue;
				}
			}

			if (std::filesystem::is_regular_file(*it) &&
				(ext.empty() || ext.find(it->path().extension().wstring()) != ext.end()))
			{
				files.push_back(FilePath(it->path().generic_wstring()));
			}
			++it;
		}
	}
	return files;
}

FileInfo FileSystem::getFileInfoForPath(const FilePath& filePath)
{
	if (filePath.exists())
	{
		return FileInfo(filePath, getLastWriteTime(filePath));
	}
	return FileInfo();
}

std::vector<FileInfo> FileSystem::getFileInfosFromPaths(
	const std::vector<FilePath>& paths,
	const std::vector<std::wstring>& fileExtensions,
	bool followSymLinks)
{
	std::set<std::wstring> ext;
	for (const std::wstring& e: fileExtensions)
	{
		ext.insert(utility::toLowerCase(e));
	}

	std::set<std::filesystem::path> symlinkDirs;
	std::set<FilePath> filePaths;

	std::vector<FileInfo> files;

	for (const FilePath& path: paths)
	{
		if (path.isDirectory())
		{
			std::filesystem::recursive_directory_iterator it(
				path.getPath(), std::filesystem::directory_options::follow_directory_symlink);
			std::filesystem::recursive_directory_iterator endit;
			std::error_code ec;
			for (; it != endit; it.increment(ec))
			{
				if (std::filesystem::is_symlink(*it))
				{
					if (!followSymLinks)
					{
						it.disable_recursion_pending();
						continue;
					}

					// check for self-referencing symlinks
					std::filesystem::path p = std::filesystem::read_symlink(*it);
					if (p.filename() == p.string() && p.filename() == it->path().filename())
					{
						continue;
					}

					// check for duplicates when following directory symlinks
					if (std::filesystem::is_directory(*it))
					{
						std::filesystem::path absDir = std::filesystem::canonical(
							it->path().parent_path() / p);

						if (symlinkDirs.find(absDir) != symlinkDirs.end())
						{
							it.disable_recursion_pending();
							continue;
						}

						symlinkDirs.insert(absDir);
					}
				}

				if (std::filesystem::is_regular_file(*it) &&
					(ext.empty() ||
					 ext.find(utility::toLowerCase(it->path().extension().wstring())) != ext.end()))
				{
					const FilePath canonicalPath = FilePath(it->path().wstring()).getCanonical();
					if (filePaths.find(canonicalPath) != filePaths.end())
					{
						continue;
					}
					filePaths.insert(canonicalPath);
					files.push_back(getFileInfoForPath(canonicalPath));
				}
			}
		}
		else if (path.exists() && (ext.empty() || ext.find(utility::toLowerCase(path.extension())) != ext.end()))
		{
			const FilePath canonicalPath = path.getCanonical();
			if (filePaths.find(canonicalPath) != filePaths.end())
			{
				continue;
			}
			filePaths.insert(canonicalPath);
			files.push_back(getFileInfoForPath(canonicalPath));
		}
	}

	return files;
}

std::set<FilePath> FileSystem::getSymLinkedDirectories(const FilePath& path)
{
	return getSymLinkedDirectories(std::vector<FilePath> {path});
}

std::set<FilePath> FileSystem::getSymLinkedDirectories(const std::vector<FilePath>& paths)
{
	std::set<std::filesystem::path> symlinkDirs;

	for (const FilePath& path: paths)
	{
		if (path.isDirectory())
		{
			std::filesystem::recursive_directory_iterator it(
				path.getPath(), std::filesystem::directory_options::follow_directory_symlink);
			std::filesystem::recursive_directory_iterator endit;
			std::error_code ec;
			for (; it != endit; it.increment(ec))
			{
				if (std::filesystem::is_symlink(*it))
				{
					// check for self-referencing symlinks
					std::filesystem::path p = std::filesystem::read_symlink(*it);
					if (p.filename() == p.string() && p.filename() == it->path().filename())
					{
						continue;
					}

					// check for duplicates when following directory symlinks
					if (std::filesystem::is_directory(*it))
					{
						std::filesystem::path absDir = std::filesystem::canonical(
							it->path().parent_path() / p);

						if (symlinkDirs.find(absDir) != symlinkDirs.end())
						{
							it.disable_recursion_pending();
							continue;
						}

						symlinkDirs.insert(absDir);
					}
				}
			}
		}
	}

	std::set<FilePath> files;
	for (auto& p: symlinkDirs)
	{
		files.insert(FilePath(p.wstring()));
	}
	return files;
}

unsigned long long FileSystem::getFileByteSize(const FilePath& filePath)
{
	return std::filesystem::file_size(filePath.getPath());
}

TimeStamp FileSystem::getLastWriteTime(const FilePath& filePath)
{
	boost::posix_time::ptime lastWriteTime;
	if (filePath.exists())
	{
		auto ftime = std::filesystem::last_write_time(filePath.getPath());
		auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
			ftime - std::filesystem::file_time_type::clock::now()
			+ std::chrono::system_clock::now());
		std::time_t t = std::chrono::system_clock::to_time_t(sctp);
		lastWriteTime = boost::posix_time::from_time_t(t);
		lastWriteTime = boost::date_time::c_local_adjustor<boost::posix_time::ptime>::utc_to_local(
			lastWriteTime);
	}
	return TimeStamp(lastWriteTime);
}

bool FileSystem::remove(const FilePath& path)
{
	std::error_code ec;
	const bool ret = std::filesystem::remove(path.getPath(), ec);
	path.recheckExists();
	return ret;
}

bool FileSystem::rename(const FilePath& from, const FilePath& to)
{
	if (!from.recheckExists() || to.recheckExists())
	{
		return false;
	}

	std::filesystem::rename(from.getPath(), to.getPath());
	to.recheckExists();
	return true;
}

bool FileSystem::copyFile(const FilePath& from, const FilePath& to)
{
	if (!from.recheckExists() || to.recheckExists())
	{
		return false;
	}

	std::filesystem::copy_file(from.getPath(), to.getPath());
	to.recheckExists();
	return true;
}

bool FileSystem::copy_directory(const FilePath& from, const FilePath& to)
{
	if (!from.recheckExists() || to.recheckExists())
	{
		return false;
	}

	std::filesystem::copy(from.getPath(), to.getPath(), std::filesystem::copy_options::directories_only);
	to.recheckExists();
	return true;
}

void FileSystem::createDirectory(const FilePath& path)
{
	std::filesystem::create_directories(path.str());
	path.recheckExists();
}

std::vector<FilePath> FileSystem::getDirectSubDirectories(const FilePath& path)
{
	std::vector<FilePath> v;

	if (path.exists() && path.isDirectory())
	{
		for (std::filesystem::directory_iterator end, dir(path.str()); dir != end; dir++)
		{
			if (std::filesystem::is_directory(dir->path()))
			{
				v.push_back(FilePath(dir->path().wstring()));
			}
		}
	}

	return v;
}

std::vector<FilePath> FileSystem::getRecursiveSubDirectories(const FilePath& path)
{
	std::vector<FilePath> v;

	if (path.exists() && path.isDirectory())
	{
		for (std::filesystem::recursive_directory_iterator end, dir(path.str()); dir != end; dir++)
		{
			if (std::filesystem::is_directory(dir->path()))
			{
				v.push_back(FilePath(dir->path().wstring()));
			}
		}
	}

	return v;
}
