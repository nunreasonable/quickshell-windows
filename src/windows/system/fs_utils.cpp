#include "fs_utils.hpp"
#include <string>

#include <qt_windows.h>

#include <qcoreapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qstandardpaths.h>

namespace qs::windows::sys {

QString FsUtils::classify(const QString& path) {
	auto wpath = path.toStdWString();
	auto attrs = GetFileAttributesW(wpath.c_str());
	if (attrs == INVALID_FILE_ATTRIBUTES) return QStringLiteral("invalid");
	return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? QStringLiteral("dir") : QStringLiteral("file");
}

bool FsUtils::isAccessibleDir(const QString& path) {
	if (path.isEmpty()) return false;

	auto wpath = path.toStdWString();
	auto attrs = GetFileAttributesW(wpath.c_str());
	if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) return false;

	auto pattern = wpath;
	if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
	pattern += L'*';

	WIN32_FIND_DATAW findData;
	auto handle = FindFirstFileExW(
	    pattern.c_str(),
	    FindExInfoBasic,
	    &findData,
	    FindExSearchNameMatch,
	    nullptr,
	    0
	);

	if (handle == INVALID_HANDLE_VALUE) return false;
	FindClose(handle);
	return true;
}

QStringList FsUtils::listDir(const QString& path) {
	return QDir(path).entryList(QDir::Files, QDir::Name);
}

QString FsUtils::findExecutable(const QString& name) {
	if (name.isEmpty()) return QString();

	auto local = QFileInfo(QDir(QCoreApplication::applicationDirPath()), name);
	if (local.isFile()) return local.absoluteFilePath();

	return QStandardPaths::findExecutable(name);
}

bool FsUtils::makePath(const QString& path) {
	if (path.isEmpty()) return false;
	return QDir().mkpath(path);
}

bool FsUtils::removeFile(const QString& path) {
	auto info = QFileInfo(path);
	if (!info.exists()) return true;
	if (!info.isFile()) return false;
	if (!info.isWritable()) QFile::setPermissions(path, info.permissions() | QFileDevice::WriteOwner | QFileDevice::WriteUser);
	return QFile::remove(path);
}

QString FsUtils::canonicalPath(const QString& path) {
	if (path.isEmpty()) return QString();
	auto native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
	auto wide = native.toStdWString();
	auto size = GetLongPathNameW(wide.c_str(), nullptr, 0);
	if (size > 0) {
		auto buffer = std::wstring(size, L'\0');
		auto written = GetLongPathNameW(wide.c_str(), buffer.data(), size);
		if (written > 0 && written < size) native = QString::fromWCharArray(buffer.c_str(), static_cast<qsizetype>(written));
	}
	auto canonical = QFileInfo(native).canonicalFilePath();
	return canonical.isEmpty() ? QDir::fromNativeSeparators(native) : canonical;
}

} // namespace qs::windows::sys
