#include "brightness.hpp"

#include <algorithm>
#include <cwchar>
#include <vector>

#include <qt_windows.h>

#include <lowlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <oleauto.h>
#include <wbemidl.h>

#include <qcoreapplication.h>
#include <qguiapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qmutex.h>
#include <qpointer.h>
#include <qrunnable.h>
#include <qscreen.h>
#include <qthreadpool.h>

#include "../../core/backgroundpool.hpp"
#include "../../core/logcat.hpp"
#include "../startup.hpp"
#include "../util.hpp"
#include "gamma.hpp"

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logBrightness, "quickshell.windows.brightness", QtWarningMsg);

bool ddcQuery(HMONITOR hMonitor, qreal& outBrightness) {
	if (hMonitor == nullptr) return false;

	DWORD count = 0;
	if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &count) || count == 0) return false;

	std::vector<PHYSICAL_MONITOR> monitors(count);
	if (!GetPhysicalMonitorsFromHMONITOR(hMonitor, count, monitors.data())) return false;

	auto ok = false;
	MC_VCP_CODE_TYPE vcpType {};
	DWORD current = 0;
	DWORD maxValue = 0;

	if (GetVCPFeatureAndVCPFeatureReply(monitors[0].hPhysicalMonitor, 0x10, &vcpType, &current, &maxValue)
	    && maxValue > 0)
	{
		outBrightness = qreal(current) / qreal(maxValue);
		ok = true;
	}

	DestroyPhysicalMonitors(count, monitors.data());
	return ok;
}

bool ddcSet(HMONITOR hMonitor, qreal value) {
	if (hMonitor == nullptr) return false;

	DWORD count = 0;
	if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &count) || count == 0) return false;

	std::vector<PHYSICAL_MONITOR> monitors(count);
	if (!GetPhysicalMonitorsFromHMONITOR(hMonitor, count, monitors.data())) return false;

	MC_VCP_CODE_TYPE vcpType {};
	DWORD current = 0;
	DWORD maxValue = 100;
	GetVCPFeatureAndVCPFeatureReply(monitors[0].hPhysicalMonitor, 0x10, &vcpType, &current, &maxValue);

	auto target = static_cast<DWORD>(std::clamp(value, 0.0, 1.0) * qreal(maxValue) + 0.5);
	auto ok = SetVCPFeature(monitors[0].hPhysicalMonitor, 0x10, target) != FALSE;

	DestroyPhysicalMonitors(count, monitors.data());
	return ok;
}

enum class PanelKind : quint8 {
	Unknown,
	Internal,
	External,
};

const char* panelKindName(PanelKind kind) {
	switch (kind) {
	case PanelKind::Internal: return "internal";
	case PanelKind::External: return "external";
	default: return "unknown";
	}
}

bool isInternalOutput(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology) {
	switch (technology) {
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED:
	case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS: return true;
	default: return false;
	}
}

PanelKind panelKind(HMONITOR hMonitor) {
	if (hMonitor == nullptr) return PanelKind::Unknown;

	MONITORINFOEXW info {};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(hMonitor, &info)) return PanelKind::Unknown;

	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	LONG result = ERROR_INSUFFICIENT_BUFFER;

	for (auto attempt = 0; attempt < 3 && result == ERROR_INSUFFICIENT_BUFFER; attempt++) {
		UINT32 pathCount = 0;
		UINT32 modeCount = 0;
		result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
		if (result != ERROR_SUCCESS) return PanelKind::Unknown;

		paths.resize(pathCount);
		modes.resize(modeCount);
		result = QueryDisplayConfig(
		    QDC_ONLY_ACTIVE_PATHS,
		    &pathCount,
		    paths.data(),
		    &modeCount,
		    modes.data(),
		    nullptr
		);
		paths.resize(pathCount);
	}

	if (result != ERROR_SUCCESS) return PanelKind::Unknown;

	auto matched = false;
	for (const auto& path: paths) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
		source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		source.header.size = sizeof(source);
		source.header.adapterId = path.sourceInfo.adapterId;
		source.header.id = path.sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
		if (std::wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) continue;

		matched = true;
		if (isInternalOutput(path.targetInfo.outputTechnology)) return PanelKind::Internal;
	}

	return matched ? PanelKind::External : PanelKind::Unknown;
}

struct MtaScope {
	MtaScope(): hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

	MtaScope(const MtaScope&) = delete;
	MtaScope(MtaScope&&) = delete;
	MtaScope& operator=(const MtaScope&) = delete;
	MtaScope& operator=(MtaScope&&) = delete;

	~MtaScope() {
		if (SUCCEEDED(this->hr)) CoUninitialize();
	}

	HRESULT hr;
};

IWbemServices* connectWmi() {
	IWbemLocator* locator = nullptr;
	auto hr = CoCreateInstance(
	    CLSID_WbemLocator,
	    nullptr,
	    CLSCTX_INPROC_SERVER,
	    IID_IWbemLocator,
	    reinterpret_cast<LPVOID*>(&locator) // NOLINT
	);
	if (FAILED(hr) || locator == nullptr) return nullptr;

	IWbemServices* services = nullptr;
	auto* ns = SysAllocString(L"ROOT\\WMI");
	hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
	SysFreeString(ns);
	locator->Release();
	if (FAILED(hr) || services == nullptr) return nullptr;

	CoSetProxyBlanket(
	    services,
	    RPC_C_AUTHN_WINNT,
	    RPC_C_AUTHZ_NONE,
	    nullptr,
	    RPC_C_AUTHN_LEVEL_CALL,
	    RPC_C_IMP_LEVEL_IMPERSONATE,
	    nullptr,
	    EOAC_NONE
	);

	return services;
}

bool wmiConnectionLost(HRESULT hr) {
	return hr == RPC_E_DISCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE)
	    || hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED) || hr == WBEM_E_TRANSPORT_FAILURE
	    || hr == WBEM_E_SHUTTING_DOWN;
}

struct WmiCache {
	QMutex mutex;
	IWbemServices* services = nullptr;
	bool mtaHeld = false;
	bool mtaAttempted = false;
};

WmiCache& wmiCache() {
	static auto* cache = new WmiCache();
	return *cache;
}

template <typename Fn>
bool withWmi(Fn&& fn) {
	MtaScope com;

	auto runOnce = [&fn]() {
		auto* services = connectWmi();
		if (services == nullptr) return false;
		auto hr = fn(services);
		services->Release();
		return SUCCEEDED(hr);
	};

	if (com.hr == RPC_E_CHANGED_MODE) return runOnce();
	if (FAILED(com.hr)) return false;

	auto& cache = wmiCache();
	QMutexLocker locker(&cache.mutex);

	if (!cache.mtaAttempted) {
		cache.mtaAttempted = true;
		CO_MTA_USAGE_COOKIE cookie = nullptr;
		cache.mtaHeld = SUCCEEDED(CoIncrementMTAUsage(&cookie));
	}

	if (!cache.mtaHeld) return runOnce();

	if (cache.services == nullptr) cache.services = connectWmi();
	if (cache.services == nullptr) return false;

	auto hr = fn(cache.services);
	if (wmiConnectionLost(hr)) {
		qCDebug(logBrightness) << "WMI connection lost, reconnecting";
		cache.services->Release();
		cache.services = connectWmi();
		if (cache.services == nullptr) return false;
		hr = fn(cache.services);
	}

	return SUCCEEDED(hr);
}

HRESULT readWmiBrightness(
    IWbemServices* services,
    int index,
    qreal& outBrightness,
    bool& found,
    QString* outInstanceName
) {
	auto* query = SysAllocString(L"SELECT * FROM WmiMonitorBrightness");
	auto* language = SysAllocString(L"WQL");
	IEnumWbemClassObject* enumerator = nullptr;
	auto hr = services->ExecQuery(
	    language,
	    query,
	    WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
	    nullptr,
	    &enumerator
	);
	SysFreeString(query);
	SysFreeString(language);
	if (FAILED(hr)) return hr;
	if (enumerator == nullptr) return E_FAIL;

	IWbemClassObject* obj = nullptr;
	ULONG returned = 0;
	auto current = 0;

	while ((hr = enumerator->Next(WBEM_INFINITE, 1, &obj, &returned)) == S_OK) {
		if (current == index) {
			VARIANT brightness {};
			VariantInit(&brightness);
			if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &brightness, nullptr, nullptr))) {
				outBrightness = qreal(brightness.bVal) / 100.0;
				found = true;
			}
			VariantClear(&brightness);

			if (found && outInstanceName != nullptr) {
				VARIANT instanceName {};
				VariantInit(&instanceName);
				if (SUCCEEDED(obj->Get(L"InstanceName", 0, &instanceName, nullptr, nullptr))
				    && instanceName.vt == VT_BSTR)
				{
					*outInstanceName = QString::fromWCharArray(instanceName.bstrVal);
				}
				VariantClear(&instanceName);
			}

			obj->Release();
			break;
		}

		obj->Release();
		current++;
	}

	enumerator->Release();
	return FAILED(hr) ? hr : S_OK;
}

bool queryWmiBrightness(int index, qreal& outBrightness) {
	auto found = false;
	auto ok = withWmi([&](IWbemServices* services) {
		return readWmiBrightness(services, index, outBrightness, found, nullptr);
	});

	return ok && found;
}

HRESULT writeWmiBrightness(IWbemServices* services, int index, qreal value, bool& applied) {
	QString instanceName;
	qreal unused = 0.0;
	auto found = false;
	auto hr = readWmiBrightness(services, index, unused, found, &instanceName);
	if (FAILED(hr)) return hr;
	if (!found || instanceName.isEmpty()) return S_OK;

	auto escaped = instanceName;
	escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
	escaped.replace(QStringLiteral("'"), QStringLiteral("\\'"));
	auto queryString =
	    QStringLiteral("SELECT * FROM WmiMonitorBrightnessMethods WHERE InstanceName = '%1'")
	        .arg(escaped);
	auto* queryText = SysAllocString(reinterpret_cast<const wchar_t*>(queryString.utf16()));
	auto* language = SysAllocString(L"WQL");

	IEnumWbemClassObject* enumerator = nullptr;
	hr = services->ExecQuery(
	    language,
	    queryText,
	    WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
	    nullptr,
	    &enumerator
	);
	SysFreeString(queryText);
	SysFreeString(language);
	if (FAILED(hr)) return hr;
	if (enumerator == nullptr) return E_FAIL;

	IWbemClassObject* obj = nullptr;
	ULONG returned = 0;

	hr = enumerator->Next(WBEM_INFINITE, 1, &obj, &returned);
	if (hr == S_OK) {
		VARIANT path {};
		VariantInit(&path);
		obj->Get(L"__PATH", 0, &path, nullptr, nullptr);

		IWbemClassObject* classObj = nullptr;
		IWbemClassObject* inSignature = nullptr;
		auto* className = SysAllocString(L"WmiMonitorBrightnessMethods");
		auto* methodName = SysAllocString(L"WmiSetBrightness");

		hr = services->GetObject(className, 0, nullptr, &classObj, nullptr);
		if (SUCCEEDED(hr)) hr = classObj->GetMethod(methodName, 0, &inSignature, nullptr);

		if (SUCCEEDED(hr)) {
			IWbemClassObject* instance = nullptr;
			if (SUCCEEDED(inSignature->SpawnInstance(0, &instance))) {
				VARIANT timeout {};
				VariantInit(&timeout);
				timeout.vt = VT_UI4;
				timeout.ulVal = 0;
				instance->Put(L"Timeout", 0, &timeout, 0);

				VARIANT brightness {};
				VariantInit(&brightness);
				brightness.vt = VT_UI1;
				brightness.bVal = static_cast<BYTE>(std::clamp(value, 0.0, 1.0) * 100.0 + 0.5);
				instance->Put(L"Brightness", 0, &brightness, 0);

				IWbemClassObject* outParams = nullptr;
				if (path.vt == VT_BSTR) {
					hr = services->ExecMethod(
					    path.bstrVal,
					    methodName,
					    0,
					    nullptr,
					    instance,
					    &outParams,
					    nullptr
					);
					applied = SUCCEEDED(hr);
				}

				if (outParams != nullptr) outParams->Release();
				instance->Release();
			}
		}
		if (inSignature != nullptr) inSignature->Release();
		if (classObj != nullptr) classObj->Release();
		SysFreeString(className);
		SysFreeString(methodName);
		VariantClear(&path);
		obj->Release();
	}

	enumerator->Release();
	return FAILED(hr) ? hr : S_OK;
}

bool setWmiBrightness(int index, qreal value) {
	auto applied = false;
	auto ok = withWmi([&](IWbemServices* services) {
		return writeWmiBrightness(services, index, value, applied);
	});

	return ok && applied;
}

QScreen* screenNamed(const QString& screenName) {
	for (auto* screen: QGuiApplication::screens()) {
		if (screen->name() == screenName) return screen;
	}

	return nullptr;
}

} // namespace

void Brightness::query(const QString& screenName) {
	if (!startup::settled()) {
		startup::afterFirstFrame(this, [this, screenName]() { this->query(screenName); });
		return;
	}

	auto* target = screenNamed(screenName);
	auto hMonitor = target != nullptr ? qs::windows::monitorForScreen(target) : nullptr;
	auto cachedRoute = this->screenRoute.value(screenName, -2);
	auto resolved = cachedRoute != -2;
	auto knownDdc = cachedRoute == -1;
	auto wmiCandidateIndex = cachedRoute >= 0 ? cachedRoute : this->nextWmiInstanceIndex;

	auto guard = QPointer<Brightness>(this);
	auto* task = QRunnable::create([guard, screenName, hMonitor, resolved, knownDdc, wmiCandidateIndex]() {
		auto brightness = 0.0;
		auto available = false;
		auto isDdc = false;
		auto usedWmiIndex = -1;

		if (resolved && knownDdc) {
			available = ddcQuery(hMonitor, brightness);
			isDdc = true;
		} else if (resolved && !knownDdc) {
			available = queryWmiBrightness(wmiCandidateIndex, brightness);
			usedWmiIndex = wmiCandidateIndex;
		} else if (ddcQuery(hMonitor, brightness)) {
			available = true;
			isDdc = true;
		} else if (panelKind(hMonitor) != PanelKind::External
		           && queryWmiBrightness(wmiCandidateIndex, brightness))
		{
			available = true;
			usedWmiIndex = wmiCandidateIndex;
		}

		QMetaObject::invokeMethod(
		    QCoreApplication::instance(),
		    [guard, screenName, available, isDdc, brightness, usedWmiIndex]() {
			    auto* self = guard.data();
			    if (self == nullptr) return;
			    if (!self->screenRoute.contains(screenName)) {
				    if (isDdc) {
					    self->screenRoute.insert(screenName, -1);
				    } else if (usedWmiIndex >= 0) {
					    self->screenRoute.insert(screenName, usedWmiIndex);
					    self->nextWmiInstanceIndex =
					        std::max(self->nextWmiInstanceIndex, usedWmiIndex + 1);
				    }
			    }
			    emit self->queried(screenName, available, isDdc, brightness);
		    },
		    Qt::QueuedConnection
		);
	});

	BackgroundThreadPool::instance()->start(task);
}

void Brightness::probe(const QString& screenName) {
	if (!startup::settled()) {
		startup::afterFirstFrame(this, [this, screenName]() { this->probe(screenName); });
		return;
	}

	auto* target = screenNamed(screenName);
	auto hMonitor = target != nullptr ? qs::windows::monitorForScreen(target) : nullptr;
	auto cachedRoute = this->screenRoute.value(screenName, -2);
	auto wmiCandidateIndex = cachedRoute >= 0 ? cachedRoute : this->nextWmiInstanceIndex;

	auto guard = QPointer<Brightness>(this);
	auto* task = QRunnable::create([guard, screenName, hMonitor, cachedRoute, wmiCandidateIndex]() {
		auto brightness = 1.0;
		auto candidate = cachedRoute >= 0;
		if (cachedRoute == -2) {
			auto kind = panelKind(hMonitor);
			qCDebug(logBrightness) << screenName << "output kind:" << panelKindName(kind);
			candidate = kind != PanelKind::External;
		}

		auto internal = candidate && queryWmiBrightness(wmiCandidateIndex, brightness);

		QMetaObject::invokeMethod(
		    QCoreApplication::instance(),
		    [guard, screenName, internal, brightness, wmiCandidateIndex]() {
			    auto* self = guard.data();
			    if (self == nullptr) return;
			    if (internal && !self->screenRoute.contains(screenName)) {
				    self->screenRoute.insert(screenName, wmiCandidateIndex);
				    self->nextWmiInstanceIndex =
				        std::max(self->nextWmiInstanceIndex, wmiCandidateIndex + 1);
			    }

			    if (internal) emit self->queried(screenName, true, false, brightness);
			    else emit self->queried(screenName, false, true, 1.0);
		    },
		    Qt::QueuedConnection
		);
	});

	BackgroundThreadPool::instance()->start(task);
}

void Brightness::setBrightness(const QString& screenName, bool isDdc, qreal value) {
	auto* target = screenNamed(screenName);
	auto hMonitor = target != nullptr ? qs::windows::monitorForScreen(target) : nullptr;
	auto wmiIndex = this->screenRoute.value(screenName, -1);
	auto clamped = std::clamp(value, 0.0, 1.0);

	if (isDdc && this->software.contains(screenName)) {
		GammaController::instance()->setBrightness(screenName, clamped);
		emit this->brightnessSetFinished(screenName, true);
		return;
	}

	auto guard = QPointer<Brightness>(this);
	auto* task = QRunnable::create([guard, screenName, hMonitor, isDdc, wmiIndex, clamped]() {
		auto ok = false;
		if (isDdc) {
			ok = ddcSet(hMonitor, clamped);
		} else if (wmiIndex >= 0) {
			ok = setWmiBrightness(wmiIndex, clamped);
		}

		QMetaObject::invokeMethod(
		    QCoreApplication::instance(),
		    [guard, screenName, isDdc, clamped, ok]() {
			    auto* self = guard.data();
			    if (self == nullptr) return;
			    if (isDdc && !ok) {
				    qCInfo(logBrightness) << screenName
				                          << "doesn't take DDC/CI brightness; dimming it in software";
				    self->software.insert(screenName, true);
				    GammaController::instance()->setBrightness(screenName, clamped);
				    emit self->brightnessSetFinished(screenName, true);
				    return;
			    }

			    emit self->brightnessSetFinished(screenName, ok);
		    },
		    Qt::QueuedConnection
		);
	});

	BackgroundThreadPool::instance()->start(task);
}

} // namespace qs::windows::sys
