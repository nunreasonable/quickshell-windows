#include "wallpaper.hpp"

#include <atomic>
#include <thread>

#include <qcolor.h>
#include <qcoreapplication.h>
#include <qdir.h>
#include <qfileinfo.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qpointer.h>
#include <qstring.h>

#include <qt_windows.h>

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.UserProfile.h>

#include "../services/message_window.hpp"

using Microsoft::WRL::ComPtr;

namespace qs::windows::sys {

namespace {
Q_LOGGING_CATEGORY(logWallpaper, "quickshell.windows.wallpaper", QtWarningMsg);

std::atomic<bool> gShuttingDown = false;

const wchar_t* const kPersonalizeKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
const wchar_t* const kDwmKey = L"Software\\Microsoft\\Windows\\DWM";
const wchar_t* const kContentDeliveryKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager";
const wchar_t* const kLockScreenKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Lock Screen";

void broadcastSettingChange(const wchar_t* setting) {
	std::thread([setting] {
		DWORD_PTR result = 0;
		SendMessageTimeoutW(
		    HWND_BROADCAST,
		    WM_SETTINGCHANGE,
		    0,
		    reinterpret_cast<LPARAM>(setting),
		    SMTO_ABORTIFHUNG,
		    2000,
		    &result
		);
	}).detach();
}

bool readDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD& out) {
	HKEY key = nullptr;
	if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
	DWORD value = 0;
	DWORD size = sizeof(value);
	DWORD type = 0;
	auto ok = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
	RegCloseKey(key);
	if (ok != ERROR_SUCCESS || type != REG_DWORD) return false;
	out = value;
	return true;
}

bool writeDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD value) {
	HKEY key = nullptr;
	if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr)
	    != ERROR_SUCCESS)
	{
		return false;
	}

	auto ok =
	    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));

	RegCloseKey(key);
	return ok == ERROR_SUCCESS;
}

bool writeString(HKEY root, const wchar_t* subkey, const wchar_t* name, const wchar_t* value) {
	HKEY key = nullptr;
	if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr)
	    != ERROR_SUCCESS)
	{
		return false;
	}

	auto bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
	auto ok = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value), bytes);

	RegCloseKey(key);
	return ok == ERROR_SUCCESS;
}

bool parseHexColor(const QString& hex, QColor& out) {
	auto trimmed = hex.startsWith(u'#') ? hex.mid(1) : hex;
	if (trimmed.length() != 6) return false;

	bool ok = false;
	auto value = trimmed.toUInt(&ok, 16);
	if (!ok) return false;

	out = QColor(
	    static_cast<int>((value >> 16) & 0xFF),
	    static_cast<int>((value >> 8) & 0xFF),
	    static_cast<int>(value & 0xFF)
	);

	return true;
}

void openUri(const wchar_t* uri) {
	auto result = reinterpret_cast<INT_PTR>( // NOLINT
	    ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL)
	);

	if (result <= 32) {
		qCWarning(logWallpaper) << "Could not open" << QString::fromWCharArray(uri) << ":" << result;
	}
}

struct LockScreenResult {
	bool ok = false;
	QString errorMessage;
};

LockScreenResult setLockScreenImageWorker(const QString& path) {
	auto apartment = true;
	try {
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
	} catch (...) {
		apartment = false;
	}

	LockScreenResult result;
	try {
		using namespace winrt::Windows::Storage;
		using namespace winrt::Windows::System::UserProfile;

		auto file = StorageFile::GetFileFromPathAsync(winrt::hstring(path.toStdWString())).get();
		LockScreen::SetImageFileAsync(file).get();
		result.ok = true;
	} catch (const winrt::hresult_error& e) {
		result.errorMessage = QString::fromWCharArray(e.message().c_str());
	} catch (...) {
		result.errorMessage = QStringLiteral("unknown error");
	}

	if (apartment) winrt::uninit_apartment();
	return result;
}

} // namespace

QString Wallpaper::currentWallpaper() {
	ComPtr<IDesktopWallpaper> wallpaper;
	auto hr =
	    CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wallpaper));

	if (SUCCEEDED(hr) && wallpaper) {
		LPWSTR path = nullptr;
		hr = wallpaper->GetWallpaper(nullptr, &path);
		if (SUCCEEDED(hr) && path != nullptr) {
			auto result = QString::fromWCharArray(path);
			CoTaskMemFree(path);
			if (!result.isEmpty()) return result;
		}

		UINT count = 0;
		if (SUCCEEDED(wallpaper->GetMonitorDevicePathCount(&count))) {
			for (UINT i = 0; i < count; i++) {
				LPWSTR monitorId = nullptr;
				if (FAILED(wallpaper->GetMonitorDevicePathAt(i, &monitorId)) || monitorId == nullptr) continue;

				LPWSTR monitorPath = nullptr;
				auto ok = SUCCEEDED(wallpaper->GetWallpaper(monitorId, &monitorPath)) && monitorPath != nullptr;
				CoTaskMemFree(monitorId);
				if (!ok) continue;

				auto result = QString::fromWCharArray(monitorPath);
				CoTaskMemFree(monitorPath);
				if (!result.isEmpty() && QFileInfo::exists(result)) return result;
			}
		}
	} else {
		qCDebug(logWallpaper) << "IDesktopWallpaper unavailable for read:" << Qt::hex << hr;
	}

	wchar_t buf[MAX_PATH] = {};
	if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, buf, 0)) {
		return QString::fromWCharArray(buf);
	}

	return {};
}

bool Wallpaper::setWallpaper(const QString& path, bool animate) {
	if (path.isEmpty()) return false;

	auto wpath = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toStdWString();

	if (!animate) {
		writeString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallpaperStyle", L"10");
		writeString(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"TileWallpaper", L"0");

		auto ok = SystemParametersInfoW(
		    SPI_SETDESKWALLPAPER,
		    0,
		    const_cast<wchar_t*>(wpath.c_str()),
		    SPIF_UPDATEINIFILE | SPIF_SENDCHANGE
		);

		if (ok) return true;
		qCWarning(logWallpaper) << "SystemParametersInfo(SPI_SETDESKWALLPAPER) failed:" << GetLastError();
	}

	ComPtr<IDesktopWallpaper> wallpaper;
	auto hr =
	    CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wallpaper));

	if (FAILED(hr) || !wallpaper) {
		qCWarning(logWallpaper) << "IDesktopWallpaper unavailable:" << Qt::hex << hr;
		return false;
	}

	wallpaper->SetPosition(DWPOS_FILL);

	bool anyOk = false;

	UINT count = 0;
	if (SUCCEEDED(wallpaper->GetMonitorDevicePathCount(&count)) && count > 0) {
		for (UINT i = 0; i < count; i++) {
			LPWSTR monitorId = nullptr;
			if (FAILED(wallpaper->GetMonitorDevicePathAt(i, &monitorId)) || monitorId == nullptr) {
				continue;
			}

			auto setHr = wallpaper->SetWallpaper(monitorId, wpath.c_str());
			if (SUCCEEDED(setHr)) {
				anyOk = true;
			} else {
				qCWarning(logWallpaper) << "SetWallpaper failed for monitor" << i << ":" << Qt::hex
				                        << setHr;
			}

			CoTaskMemFree(monitorId);
		}
	}

	if (!anyOk) {
		auto setHr = wallpaper->SetWallpaper(nullptr, wpath.c_str());
		anyOk = SUCCEEDED(setHr);
		if (!anyOk) qCWarning(logWallpaper) << "SetWallpaper(all monitors) failed:" << Qt::hex << setHr;
	}

	return anyOk;
}

bool Wallpaper::isDarkMode() {
	DWORD value = 1;
	if (!readDword(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", value)) return false;
	return value == 0;
}

void Wallpaper::setDarkMode(bool dark) {
	DWORD value = dark ? 0 : 1;
	auto ok1 = writeDword(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", value);
	auto ok2 = writeDword(HKEY_CURRENT_USER, kPersonalizeKey, L"SystemUsesLightTheme", value);

	if (!ok1 || !ok2) qCWarning(logWallpaper) << "failed writing theme registry values";

	broadcastSettingChange(L"ImmersiveColorSet");
}

bool Wallpaper::setAccentColor(const QString& hex) {
	QColor color;
	if (!parseHexColor(hex, color)) {
		qCWarning(logWallpaper) << "setAccentColor: not a hex color:" << hex;
		return false;
	}

	DWORD accentColor = 0xFF000000u | (static_cast<DWORD>(color.blue()) << 16)
	                   | (static_cast<DWORD>(color.green()) << 8) | static_cast<DWORD>(color.red());

	DWORD colorizationColor = 0xC4000000u | (static_cast<DWORD>(color.red()) << 16)
	                         | (static_cast<DWORD>(color.green()) << 8) | static_cast<DWORD>(color.blue());

	DWORD current = 0;
	auto unchanged = readDword(HKEY_CURRENT_USER, kDwmKey, L"AccentColor", current)
	              && current == accentColor
	              && readDword(HKEY_CURRENT_USER, kDwmKey, L"ColorizationColor", current)
	              && current == colorizationColor
	              && readDword(HKEY_CURRENT_USER, kDwmKey, L"ColorizationAfterglow", current)
	              && current == colorizationColor;
	if (unchanged) return true;

	auto ok1 = writeDword(HKEY_CURRENT_USER, kDwmKey, L"AccentColor", accentColor);
	auto ok2 = writeDword(HKEY_CURRENT_USER, kDwmKey, L"ColorizationColor", colorizationColor);
	auto ok3 = writeDword(HKEY_CURRENT_USER, kDwmKey, L"ColorizationAfterglow", colorizationColor);

	if (!ok1 || !ok2 || !ok3) {
		qCWarning(logWallpaper) << "failed writing accent color registry values";
	}

	broadcastSettingChange(L"ImmersiveColorSet");
	return ok1 && ok2;
}

QString Wallpaper::matugenPath() {
	auto path = QCoreApplication::applicationDirPath() + QStringLiteral("/matugen.exe");
	return QFileInfo::exists(path) ? path : QString();
}

Wallpaper::Wallpaper(QObject* parent): QObject(parent) {
	QObject::connect(qApp, &QCoreApplication::aboutToQuit, []() { gShuttingDown.store(true); });

	this->mSystemDarkMode = Wallpaper::isDarkMode();

	auto* window = qs::windows::services::ServiceMessageWindow::instance();
	window->addHandler(WM_SETTINGCHANGE, [self = QPointer(this)](WPARAM /*wparam*/, LPARAM lparam) {
		if (!self || lparam == 0) return;

		auto name = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(lparam)); // NOLINT
		if (name.compare(QStringLiteral("ImmersiveColorSet"), Qt::CaseInsensitive) == 0) {
			self->refreshSystemDarkMode();
		}
	});
}

void Wallpaper::refreshSystemDarkMode() {
	auto value = Wallpaper::isDarkMode();
	if (value != this->mSystemDarkMode) {
		this->mSystemDarkMode = value;
		emit this->systemDarkModeChanged();
	}
}

void Wallpaper::setLockScreenImage(const QString& path) {
	if (path.isEmpty() || this->mLockScreenAttemptRunning) return;
	this->mLockScreenAttemptRunning = true;

	auto absolutePath = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
	auto self = QPointer<Wallpaper>(this);

	std::thread([self, absolutePath]() {
		auto result = setLockScreenImageWorker(absolutePath);

		if (gShuttingDown.load()) return;
		auto* app = QCoreApplication::instance();
		if (app == nullptr) return;

		QMetaObject::invokeMethod(
		    app,
		    [self, result]() {
			    if (self) self->applyLockScreenResult(result.ok, result.errorMessage);
		    },
		    Qt::QueuedConnection
		);
	}).detach();
}

void Wallpaper::applyLockScreenResult(bool ok, const QString& errorMessage) {
	this->mLockScreenAttemptRunning = false;

	if (!ok) {
		qCWarning(logWallpaper) << "setLockScreenImage failed:" << errorMessage;
	}

	if (ok != this->mLockScreenImageSupported) {
		this->mLockScreenImageSupported = ok;
		emit this->lockScreenImageSupportedChanged();
	}

	emit this->lockScreenImageApplied(ok);
}

QString Wallpaper::lockScreenMode() {
	DWORD value = 0;
	if (readDword(HKEY_CURRENT_USER, kContentDeliveryKey, L"RotatingLockScreenEnabled", value)
	    && value != 0)
	{
		return QStringLiteral("spotlight");
	}

	value = 0;
	if (readDword(HKEY_CURRENT_USER, kLockScreenKey, L"SlideshowEnabled", value) && value != 0) {
		return QStringLiteral("slideshow");
	}

	return QStringLiteral("picture");
}

void Wallpaper::openLockScreenPersonalizationSettings() { openUri(L"ms-settings:lockscreen"); }

} // namespace qs::windows::sys
