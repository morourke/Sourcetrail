#include "JavaPathDetectorLinux.h"

#include "FilePath.h"
#include "utilityApp.h"
#include "utilityString.h"

JavaPathDetectorLinux::JavaPathDetectorLinux(const std::string javaVersion)
	: JavaPathDetector(
		javaVersion.empty() ? "Java for Linux" : "Java " + javaVersion + " for Linux",
		javaVersion)
{
}

std::vector<FilePath> JavaPathDetectorLinux::doGetPaths() const
{
	std::vector<FilePath> paths;
	FilePath p = getJavaInPath();
	if (!p.empty())
	{
		paths.push_back(p);
	}
	p = getJavaInJavaHome();
	if (!p.empty())
	{
		paths.push_back(p);
	}

	// some default paths for java
	paths.push_back(FilePath(L"/etc/alternatives/java"));
	paths.push_back(FilePath(L"/usr/lib/jvm/default/bin/java"));
	paths.push_back(FilePath(L"/usr/lib/jvm/java-openjdk/bin/java"));

	for (const FilePath& path: paths)
	{
		if (checkVersion(path))
		{
			FilePath absoluteJavaPath = readLink(path);
			FilePath jvmLibrary = getFilePathRelativeToJavaExecutable(absoluteJavaPath);
			if (jvmLibrary.exists())
			{
				std::vector<FilePath> foundPath = {jvmLibrary};
				return foundPath;
			}
		}
	}

	return std::vector<FilePath>();
}

FilePath JavaPathDetectorLinux::getJavaInPath() const
{
	bool ok;
	FilePath javaPath(utility::searchPath(L"java", ok));
	if (ok && !javaPath.empty() && javaPath.exists())
	{
		return javaPath;
	}
	return FilePath();
}

FilePath JavaPathDetectorLinux::readLink(const FilePath& path) const
{
	const utility::ProcessOutput out = utility::executeProcess(
		L"readlink", std::vector<std::wstring> {L"-f", path.wstr()});

	if (out.exitCode == 0 && !out.output.empty())
	{
		FilePath javaPath(utility::trim(out.output));

		if (!javaPath.empty())
		{
			return javaPath;
		}
	}
	return FilePath();
}

FilePath JavaPathDetectorLinux::getFilePathRelativeToJavaExecutable(FilePath& javaExecutablePath) const
{
	// JDK 9+: lib/server/libjvm.so (no architecture subdirectory)
	FilePath p = javaExecutablePath.getParentDirectory().concatenate(
		L"/../lib/server/libjvm.so");
	if (p.exists())
	{
		return p.makeCanonical();
	}

	// JDK 8 and earlier: lib/<arch>/server/libjvm.so
#ifdef __x86_64__
	p = javaExecutablePath.getParentDirectory().concatenate(
		L"/../lib/amd64/server/libjvm.so");
#else
	p = javaExecutablePath.getParentDirectory().concatenate(
		L"/../lib/i386/server/libjvm.so");
#endif
	if (p.exists())
	{
		return p.makeCanonical();
	}

	return FilePath();
}

FilePath JavaPathDetectorLinux::getJavaInJavaHome() const
{
	std::string command = "";
#pragma warning(push)
#pragma warning(disable : 4996)
	char* p = getenv("JAVA_HOME");
#pragma warning(pop)
	if (p == nullptr)
	{
		return FilePath();
	}

	FilePath javaPath(std::string(p) + "/bin/java");
	if (!javaPath.empty() && javaPath.exists())
	{
		return javaPath;
	}
	return FilePath();
}

bool JavaPathDetectorLinux::checkVersion(const FilePath& path) const
{
	const utility::ProcessOutput out = utility::executeProcess(path.wstr(), {L"-version"});

	if (out.exitCode != 0)
	{
		return false;
	}

	if (m_javaVersion.empty())
	{
		return true;
	}

	return out.output.find(utility::decodeFromUtf8(m_javaVersion)) != std::string::npos;
}
