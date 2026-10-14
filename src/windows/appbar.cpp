#include "appbar.hpp"
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <shellapi.h>

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qrect.h>
#include <qtypes.h>

#include "util.hpp"

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logAppBar, "quickshell.windows.appbar", QtWarningMsg);

constexpr auto REGISTERED_KEY = L"Software\\Quickshell\\AppBars";

APPBARDATA appBarData(HWND hwnd) {
	APPBARDATA data {};
	data.cbSize = sizeof(data);
	data.hWnd = hwnd;
	data.uCallbackMessage = WinAppBar::callbackMessage();
	return data;
}

std::wstring registeredName(HWND hwnd) {
	wchar_t name[24] {};
	swprintf(name, std::size(name), L"%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
	return name;
}

void rememberRegistration(HWND hwnd) {
	DWORD pid = GetCurrentProcessId();
	RegSetKeyValueW(
	    HKEY_CURRENT_USER,
	    REGISTERED_KEY,
	    registeredName(hwnd).c_str(),
	    REG_DWORD,
	    &pid,
	    sizeof(pid)
	);
}

void forgetRegistration(HWND hwnd) {
	RegDeleteKeyValueW(HKEY_CURRENT_USER, REGISTERED_KEY, registeredName(hwnd).c_str());
}

} // namespace

void WinAppBar::removeStale() {
	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, REGISTERED_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key)
	    != ERROR_SUCCESS)
	{
		return;
	}

	auto entries = std::vector<std::pair<std::wstring, DWORD>>();
	for (DWORD i = 0;; i++) {
		wchar_t name[64] {};
		DWORD nameLength = std::size(name);
		DWORD type = 0;
		DWORD pid = 0;
		DWORD size = sizeof(pid);

		auto status = RegEnumValueW(key, i, name, &nameLength, nullptr, &type, reinterpret_cast<BYTE*>(&pid), &size);
		if (status == ERROR_MORE_DATA) continue;
		if (status != ERROR_SUCCESS) break;
		entries.emplace_back(std::wstring(name, nameLength), type == REG_DWORD ? pid : 0);
	}

	auto removed = 0;
	for (const auto& [name, pid]: entries) {
		auto* hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::wcstoull(name.c_str(), nullptr, 16)));

		if (hwnd != nullptr && IsWindow(hwnd)) {
			DWORD owner = 0;
			GetWindowThreadProcessId(hwnd, &owner);
			if (owner == pid) continue;
		} else if (hwnd != nullptr) {
			auto yield = TrayHookYield();
			auto data = appBarData(hwnd);
			SHAppBarMessage(ABM_REMOVE, &data);
			removed++;
		}

		RegDeleteValueW(key, name.c_str());
	}

	RegCloseKey(key);

	if (removed != 0) {
		qCInfo(logAppBar) << "Removed" << removed << "AppBar registrations left by a closed instance";
	}
}

UINT WinAppBar::callbackMessage() {
	static const UINT message = RegisterWindowMessageW(L"QuickshellAppBarNotify"); // NOLINT
	return message;
}

UINT WinAppBar::taskbarCreatedMessage() {
	static const UINT message = RegisterWindowMessageW(L"TaskbarCreated"); // NOLINT
	return message;
}

bool WinAppBar::ensureRegistered(HWND hwnd) {
	auto yield = TrayHookYield();
	if (this->mRegistered && this->mHwnd == hwnd) return true;
	if (this->mRegistered) this->remove();

	static auto staleRemoved = false;
	if (!staleRemoved) {
		staleRemoved = true;
		WinAppBar::removeStale();
	}

	auto data = appBarData(hwnd);
	if (!SHAppBarMessage(ABM_NEW, &data)) {
		SHAppBarMessage(ABM_REMOVE, &data);
		data = appBarData(hwnd);
		if (!SHAppBarMessage(ABM_NEW, &data)) {
			qCWarning(logAppBar) << "Failed to register AppBar for window" << hwnd;
			return false;
		}
		qCDebug(logAppBar) << "Replaced a stale AppBar registration for" << hwnd;
	}

	rememberRegistration(hwnd);
	this->mHwnd = hwnd;
	this->mRegistered = true;
	this->mReserved = QRect();
	this->mPositionValid = false;
	return true;
}

QRect WinAppBar::reserve(HWND hwnd, UINT edge, const QRect& monitor, qint32 size) {
	if (hwnd == nullptr || size <= 0 || !monitor.isValid()) return {};

	if (this->mPositionValid && this->mRegistered && this->mHwnd == hwnd && this->mEdge == edge
	    && this->mMonitor == monitor && this->mSize == size)
	{
		return this->mReserved;
	}

	auto yield = TrayHookYield();
	if (!this->ensureRegistered(hwnd)) return {};

	auto data = appBarData(hwnd);
	data.uEdge = edge;
	data.rc = toRECT(monitor);

	auto applySize = [&]() {
		switch (edge) {
		case ABE_TOP: data.rc.bottom = data.rc.top + size; break;
		case ABE_BOTTOM: data.rc.top = data.rc.bottom - size; break;
		case ABE_LEFT: data.rc.right = data.rc.left + size; break;
		case ABE_RIGHT: data.rc.left = data.rc.right - size; break;
		default: break;
		}
	};

	applySize();
	SHAppBarMessage(ABM_QUERYPOS, &data);
	applySize();

	auto granted = toQRect(data.rc);

	if (this->mEdge != edge || this->mReserved != granted) {
		SHAppBarMessage(ABM_SETPOS, &data);
		this->mEdge = edge;
		this->mReserved = toQRect(data.rc);
		qCDebug(logAppBar) << "Reserved" << this->mReserved << "on edge" << edge << "for" << hwnd;
	}

	this->mMonitor = monitor;
	this->mSize = size;
	this->mPositionValid = true;
	return this->mReserved;
}

void WinAppBar::invalidatePosition() { this->mPositionValid = false; }

void WinAppBar::remove() {
	if (!this->mRegistered) return;
	auto yield = TrayHookYield();

	auto data = appBarData(this->mHwnd);
	SHAppBarMessage(ABM_REMOVE, &data);
	forgetRegistration(this->mHwnd);
	this->invalidate();
}

void WinAppBar::invalidate() {
	this->mRegistered = false;
	this->mHwnd = nullptr;
	this->mEdge = 0;
	this->mReserved = QRect();
	this->mMonitor = QRect();
	this->mSize = 0;
	this->mPositionValid = false;
}

void WinAppBar::adopt(WinAppBar& other) {
	if (&other == this) return;
	this->remove();

	this->mHwnd = other.mHwnd;
	this->mRegistered = other.mRegistered;
	this->mEdge = other.mEdge;
	this->mReserved = other.mReserved;
	this->mMonitor = other.mMonitor;
	this->mSize = other.mSize;
	this->mPositionValid = other.mPositionValid;
	other.invalidate();
}

void WinAppBar::notifyActivate() {
	if (!this->mRegistered) return;
	auto yield = TrayHookYield();
	auto data = appBarData(this->mHwnd);
	SHAppBarMessage(ABM_ACTIVATE, &data);
}

void WinAppBar::notifyWindowPosChanged() {
	if (!this->mRegistered) return;
	auto yield = TrayHookYield();
	auto data = appBarData(this->mHwnd);
	SHAppBarMessage(ABM_WINDOWPOSCHANGED, &data);
}

} // namespace qs::windows
