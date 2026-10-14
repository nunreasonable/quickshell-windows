#include "autostart.hpp"

#include <algorithm>
#include <cstring>

#include <qbytearray.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qregularexpression.h>

#include <qt_windows.h>

#include "registry_util.hpp"
#include "shell_link.hpp"

namespace qs::windows::sys {

namespace {

const wchar_t* const RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* const APPROVED_RUN_KEY =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
const wchar_t* const APPROVED_FOLDER_KEY =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder";
const auto SELF_RUN_VALUE = QStringLiteral("illogical-impulse");

QString userStartupFolder() {
	auto appData = qEnvironmentVariable("AppData");
	if (appData.isEmpty()) return QString();
	return appData + QStringLiteral("/Microsoft/Windows/Start Menu/Programs/Startup");
}

QString machineStartupFolder() {
	auto programData = qEnvironmentVariable("ProgramData");
	if (programData.isEmpty()) return QString();
	return programData + QStringLiteral("/Microsoft/Windows/Start Menu/Programs/Startup");
}

bool isApprovedEnabled(const wchar_t* approvedSubkey, const QString& name) {
	auto wname = name.toStdWString();
	auto data = readRegistryBinary(HKEY_CURRENT_USER, approvedSubkey, wname.c_str());
	if (data.isEmpty()) return true;
	return static_cast<unsigned char>(data.at(0)) != 0x03;
}

bool setApproved(const wchar_t* approvedSubkey, const QString& name, bool enabled) {
	auto wname = name.toStdWString();
	if (enabled) return deleteRegistryValue(HKEY_CURRENT_USER, approvedSubkey, wname.c_str());

	auto buffer = QByteArray(12, '\0');
	buffer[0] = 0x03;
	FILETIME fileTime;
	GetSystemTimeAsFileTime(&fileTime);
	std::memcpy(buffer.data() + 4, &fileTime, sizeof(fileTime));
	return writeRegistryBinary(HKEY_CURRENT_USER, approvedSubkey, wname.c_str(), buffer);
}

void appendRegistryEntries(QVariantList& items, HKEY root, const QString& scope) {
	const auto isUser = root == HKEY_CURRENT_USER;

	auto appendFromView = [&](REGSAM extraFlags) {
		for (const auto& value: enumerateRegistryStrings(root, RUN_KEY, extraFlags)) {
			auto alreadyListed = std::any_of(items.cbegin(), items.cend(), [&](const QVariant& v) {
				auto map = v.toMap();
				return map.value(QStringLiteral("source")) == QStringLiteral("registry")
				    && map.value(QStringLiteral("scope")) == scope
				    && map.value(QStringLiteral("name")) == value.name;
			});
			if (alreadyListed) continue;

			auto isSelf = isUser && value.name.compare(SELF_RUN_VALUE, Qt::CaseInsensitive) == 0;

			auto item = QVariantMap();
			item[QStringLiteral("id")] = (isUser ? QStringLiteral("reg:user:") : QStringLiteral("reg:machine:"))
			                            + value.name;
			item[QStringLiteral("name")] = value.name;
			item[QStringLiteral("command")] = value.stringValue;
			item[QStringLiteral("scope")] = scope;
			item[QStringLiteral("source")] = QStringLiteral("registry");
			item[QStringLiteral("enabled")] =
			    isUser ? isApprovedEnabled(APPROVED_RUN_KEY, value.name) : true;
			item[QStringLiteral("removable")] = isUser && !isSelf;
			item[QStringLiteral("isSelf")] = isSelf;
			items.append(item);
		}
	};

	appendFromView(0);
	if (!isUser) appendFromView(KEY_WOW64_32KEY);
}

void appendFolderEntries(QVariantList& items, const QString& folder, const QString& scope) {
	const auto isUser = scope == QStringLiteral("user");
	auto dir = QDir(folder);
	if (!dir.exists()) return;

	const auto files = dir.entryInfoList({QStringLiteral("*.lnk")}, QDir::Files);
	for (const auto& fileInfo: files) {
		auto link = readShellLink(fileInfo.absoluteFilePath());
		auto fileName = fileInfo.fileName();

		auto command = link.valid
		    ? (link.arguments.isEmpty() ? link.targetPath : link.targetPath + u' ' + link.arguments)
		    : fileInfo.absoluteFilePath();

		auto item = QVariantMap();
		item[QStringLiteral("id")] =
		    (isUser ? QStringLiteral("folder:user:") : QStringLiteral("folder:machine:")) + fileName;
		item[QStringLiteral("name")] = fileInfo.completeBaseName();
		item[QStringLiteral("command")] = command;
		item[QStringLiteral("scope")] = scope;
		item[QStringLiteral("source")] = QStringLiteral("folder");
		item[QStringLiteral("enabled")] = isUser ? isApprovedEnabled(APPROVED_FOLDER_KEY, fileName) : true;
		item[QStringLiteral("removable")] = isUser;
		item[QStringLiteral("isSelf")] = false;
		items.append(item);
	}
}

} // namespace

void Autostart::refresh() {
	auto items = QVariantList();

	appendRegistryEntries(items, HKEY_CURRENT_USER, QStringLiteral("user"));
	appendRegistryEntries(items, HKEY_LOCAL_MACHINE, QStringLiteral("machine"));
	appendFolderEntries(items, userStartupFolder(), QStringLiteral("user"));
	appendFolderEntries(items, machineStartupFolder(), QStringLiteral("machine"));

	std::sort(items.begin(), items.end(), [](const QVariant& a, const QVariant& b) {
		auto mapA = a.toMap();
		auto mapB = b.toMap();
		if (mapA.value(QStringLiteral("scope")) != mapB.value(QStringLiteral("scope"))) {
			return mapA.value(QStringLiteral("scope")).toString() == QStringLiteral("user");
		}
		return mapA.value(QStringLiteral("name")).toString().toLower()
		     < mapB.value(QStringLiteral("name")).toString().toLower();
	});

	this->mEntries = items;
	emit this->entriesChanged();
}

void Autostart::setEnabled(const QString& id, bool enabled) {
	if (id.startsWith(QStringLiteral("reg:user:"))) {
		setApproved(APPROVED_RUN_KEY, id.mid(9), enabled);
	} else if (id.startsWith(QStringLiteral("folder:user:"))) {
		setApproved(APPROVED_FOLDER_KEY, id.mid(12), enabled);
	} else {
		return;
	}

	this->refresh();
}

bool Autostart::removeEntry(const QString& id) {
	auto ok = false;

	if (id.startsWith(QStringLiteral("reg:user:"))) {
		auto name = id.mid(9);
		if (name.compare(SELF_RUN_VALUE, Qt::CaseInsensitive) == 0) return false;

		auto wname = name.toStdWString();
		ok = deleteRegistryValue(HKEY_CURRENT_USER, RUN_KEY, wname.c_str());
		deleteRegistryValue(HKEY_CURRENT_USER, APPROVED_RUN_KEY, wname.c_str());
	} else if (id.startsWith(QStringLiteral("folder:user:"))) {
		auto fileName = id.mid(12);
		if (fileName.contains(u'/') || fileName.contains(u'\\')
		    || fileName.contains(QStringLiteral("..")))
		{
			return false;
		}

		ok = QFile::remove(userStartupFolder() + u'/' + fileName);
		deleteRegistryValue(HKEY_CURRENT_USER, APPROVED_FOLDER_KEY, fileName.toStdWString().c_str());
	} else {
		return false;
	}

	this->refresh();
	return ok;
}

bool Autostart::addFromPath(
    const QString& filePath,
    const QString& name,
    const QStringList& arguments
) {
	if (filePath.isEmpty()) return false;

	auto folder = userStartupFolder();
	if (folder.isEmpty() || !QDir().mkpath(folder)) return false;

	auto safeName = name.isEmpty() ? QFileInfo(filePath).completeBaseName() : name;
	safeName.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|]+)")), QStringLiteral("_"));
	safeName = safeName.trimmed();
	if (safeName.isEmpty()) safeName = QStringLiteral("App");

	auto lnkPath = folder + u'/' + safeName + QStringLiteral(".lnk");
	auto counter = 2;
	while (QFile::exists(lnkPath)) {
		lnkPath = QStringLiteral("%1/%2 (%3).lnk").arg(folder, safeName, QString::number(counter));
		counter++;
	}

	auto data = ShellLinkData();
	data.targetPath = filePath;
	data.arguments = arguments.join(u' ');
	data.description = safeName;
	if (!filePath.startsWith(QStringLiteral("shell:"), Qt::CaseInsensitive)) {
		data.workingDirectory = QFileInfo(filePath).absolutePath();
	}

	auto ok = writeShellLink(lnkPath, data);
	if (ok) this->refresh();
	return ok;
}

} // namespace qs::windows::sys
