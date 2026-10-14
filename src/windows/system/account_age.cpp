#include "account_age.hpp"
#include <atomic>
#include <thread>

#include <qcoreapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qpointer.h>
#include <qstring.h>
#include <qt_windows.h>

#include <lm.h>
#include <lmcons.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>

#include "../../core/logcat.hpp"
#include "../startup.hpp"

namespace qs::windows::sys {

namespace {

QS_LOGGING_CATEGORY(logAccountAge, "quickshell.windows.accountage", QtWarningMsg);

struct AccountInfo {
	QString kind = QStringLiteral("unknown");
	QString principal;
};

AccountInfo lookUpAccount() {
	auto info = AccountInfo();
	wchar_t name[UNLEN + 1] {};
	DWORD size = UNLEN + 1;
	if (!GetUserNameW(name, &size)) return info;

	LPBYTE buffer = nullptr;
	auto status = NetUserGetInfo(nullptr, name, 24, &buffer);
	if (status != NERR_Success || buffer == nullptr) {
		if (buffer != nullptr) NetApiBufferFree(buffer);
		qCInfo(logAccountAge) << "No level 24 info for this user (status" << status << "), treating it as not a Microsoft account";
		info.kind = QStringLiteral("other");
		return info;
	}

	auto* user = reinterpret_cast<USER_INFO_24*>(buffer);
	info.kind = QStringLiteral("local");
	if (user->usri24_internet_identity) {
		auto provider = user->usri24_internet_provider_name != nullptr
		                  ? QString::fromWCharArray(user->usri24_internet_provider_name)
		                  : QString();
		info.kind = provider.compare(QStringLiteral("MicrosoftAccount"), Qt::CaseInsensitive) == 0
		              ? QStringLiteral("microsoft")
		              : QStringLiteral("other");
		if (user->usri24_internet_principal_name != nullptr)
			info.principal = QString::fromWCharArray(user->usri24_internet_principal_name);
	}

	NetApiBufferFree(buffer);
	return info;
}

QString principalOf(const winrt::Windows::System::User& user) {
	using namespace winrt::Windows::System;
	try {
		auto value = user.GetPropertyAsync(KnownUserProperties::PrincipalName()).get();
		if (auto text = value.try_as<winrt::hstring>()) return QString::fromWCharArray(text->c_str());
	} catch (...) {
	}
	return QString();
}

QString lookUpAge(const QString& principal) {
	auto apartment = true;
	try {
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
	} catch (...) {
		apartment = false;
	}

	auto result = QStringLiteral("unknown");
	try {
		using namespace winrt::Windows::System;
		auto users = User::FindAllAsync().get();
		User user = nullptr;
		if (users.Size() == 1) {
			user = users.GetAt(0);
		} else {
			auto matches = 0;
			for (const auto& candidate: users) {
				if (!principal.isEmpty() && principalOf(candidate).compare(principal, Qt::CaseInsensitive) == 0) {
					user = candidate;
					matches++;
				}
			}
			if (matches != 1) {
				user = nullptr;
				qCInfo(logAccountAge) << "Couldn't tell which of" << users.Size() << "signed-in users runs the shell";
			}
		}

		if (user != nullptr && !user.try_as<IUser2>()) {
			result = QStringLiteral("unsupported");
		} else if (user != nullptr) {
			switch (user.CheckUserAgeConsentGroupAsync(UserAgeConsentGroup::Adult).get()) {
			case UserAgeConsentResult::Included: result = QStringLiteral("adult"); break;
			case UserAgeConsentResult::NotIncluded: result = QStringLiteral("notAdult"); break;
			default: result = QStringLiteral("unknown"); break;
			}
		}
	} catch (const winrt::hresult_error& e) {
		qCWarning(logAccountAge) << "Couldn't check the account's age group:"
		                         << QString::fromWCharArray(e.message().c_str());
	} catch (...) {
		qCWarning(logAccountAge) << "Couldn't check the account's age group";
	}

	if (apartment) winrt::uninit_apartment();
	return result;
}

std::atomic<bool> gShuttingDown = false;

} // namespace

AccountAge::AccountAge(QObject* parent): QObject(parent) {
	QObject::connect(qApp, &QCoreApplication::aboutToQuit, []() { gShuttingDown.store(true); });
	startup::afterFirstFrame(this, [this]() { this->start(); });
}

void AccountAge::refresh() { this->start(); }

void AccountAge::start() {
	if (this->mRunning || gShuttingDown.load()) return;
	this->mRunning = true;

	auto self = QPointer<AccountAge>(this);
	std::thread([self]() {
		auto kind = QStringLiteral("unknown");
		auto age = QStringLiteral("unknown");
		try {
			auto account = lookUpAccount();
			kind = account.kind;
			if (kind == QStringLiteral("microsoft")) age = lookUpAge(account.principal);
		} catch (...) {
			kind = QStringLiteral("unknown");
			age = QStringLiteral("unknown");
		}
		if (gShuttingDown.load()) return;
		auto* app = QCoreApplication::instance();
		if (app == nullptr) return;
		QMetaObject::invokeMethod(
		    app,
		    [self, kind, age]() {
			    if (self) self->apply(kind, age);
		    },
		    Qt::QueuedConnection
		);
	}).detach();
}

void AccountAge::apply(const QString& kind, const QString& age) {
	this->mRunning = false;
	this->mReady = true;
	this->mKind = kind;
	this->mAge = age;
	qCInfo(logAccountAge) << "Account kind" << kind << "age group" << age;
	emit this->changed();
}

bool AccountAge::spicyAllowed() const {
	return this->mReady && this->mKind == QStringLiteral("microsoft") && this->mAge == QStringLiteral("adult");
}

QString AccountAge::spicyRestriction() const {
	if (!this->mReady) return QStringLiteral("Checking this Windows account...");
	if (this->mKind == QStringLiteral("local")) {
		return QStringLiteral("Spicy Stuff needs a Microsoft account aged 18 or older. This Windows user is a local account.");
	}
	if (this->mKind != QStringLiteral("microsoft")) {
		return QStringLiteral("Spicy Stuff needs a personal Microsoft account aged 18 or older.");
	}
	if (this->mAge == QStringLiteral("notAdult")) {
		return QStringLiteral("Spicy Stuff needs a Microsoft account aged 18 or older. Windows says this account is not an adult account.");
	}
	if (this->mAge == QStringLiteral("unsupported")) {
		return QStringLiteral("Spicy Stuff needs a Microsoft account aged 18 or older. This version of Windows can't confirm an account's age; Windows 11 can.");
	}
	if (this->mAge != QStringLiteral("adult")) {
		return QStringLiteral("Spicy Stuff needs a Microsoft account aged 18 or older. Windows couldn't confirm this account's age.");
	}
	return QString();
}

} // namespace qs::windows::sys
