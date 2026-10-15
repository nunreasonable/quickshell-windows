#include "processcore.hpp"

#include <qcontainerfwd.h>
#include <qhash.h>
#include <qprocess.h>
#include <qvariant.h>

#include "../core/common.hpp"

#ifdef _WIN32
#include <qt_windows.h>
#endif

namespace qs::io::process {

void setupProcessEnvironment(
    QProcess* process,
    bool clear,
    const QHash<QString, QVariant>& envChanges
) {
	const auto& sysenv = qs::Common::INITIAL_ENVIRONMENT;
	auto env = clear ? QProcessEnvironment() : sysenv;

	for (auto& name: envChanges.keys()) {
		auto value = envChanges.value(name);
		if (!value.isValid()) continue;

		if (clear) {
			if (value.isNull()) {
				if (sysenv.contains(name)) env.insert(name, sysenv.value(name));
			} else env.insert(name, value.toString());
		} else {
			if (value.isNull()) env.remove(name);
			else env.insert(name, value.toString());
		}
	}

	process->setProcessEnvironment(env);
}

void disownHandles(QProcess* process) {
#ifdef _WIN32
	process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
		args->inheritHandles = false;
		args->startupInfo->dwFlags &= ~static_cast<DWORD>(STARTF_USESTDHANDLES);
		args->startupInfo->hStdInput = nullptr;
		args->startupInfo->hStdOutput = nullptr;
		args->startupInfo->hStdError = nullptr;
	});
#else
	Q_UNUSED(process);
#endif
}

} // namespace qs::io::process
