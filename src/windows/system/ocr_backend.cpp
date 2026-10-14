#include "ocr_backend.hpp"

#include <qmetaobject.h>
#include <qobject.h>
#include <qstring.h>
#include <qthread.h>

#include "ocr.hpp"
#include "ocr_worker.hpp"

namespace qs::windows::sys {

OcrBackend::OcrBackend(Ocr* frontend) {
	this->mWorker = new OcrWorker(frontend);
	this->mWorker->moveToThread(&this->mThread);

	QObject::connect(&this->mThread, &QThread::started, this->mWorker, &OcrWorker::start);
	QObject::connect(&this->mThread, &QThread::finished, this->mWorker, &OcrWorker::shutdown);
}

OcrBackend::~OcrBackend() {
	if (this->mThread.isRunning()) {
		this->mThread.quit();
		this->mThread.wait();
	}
	delete this->mWorker;
}

void OcrBackend::requestRecognize(int requestId, const QString& path) {
	if (!this->mStarted) {
		this->mStarted = true;
		this->mThread.start();
	}

	auto* worker = this->mWorker;
	QMetaObject::invokeMethod(
	    worker,
	    [worker, requestId, path] { worker->cmdRecognize(requestId, path); },
	    Qt::QueuedConnection
	);
}

} // namespace qs::windows::sys
