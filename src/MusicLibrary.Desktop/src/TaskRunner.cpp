// SPDX-License-Identifier: GPL-3.0-or-later
#include "TaskRunner.hpp"

#include <QMetaObject>

#include <chrono>
#include <thread>

namespace ml::desktop {

TaskRunner::TaskRunner(QObject* parent) : QObject(parent) {}

TaskRunner::~TaskRunner() {
	cancel();
	if (m_thread) {
		m_thread->quit();
		// Bounded wait: a task that ignores cancellation must not hang shutdown
		// forever, but it is also never killed mid-write.
		if (!m_thread->wait(30000)) {
			m_thread->terminate();
			m_thread->wait(1000);
		}
	}
}

bool TaskRunner::start(const QString& title, TaskFunction task) {
	if (m_running.load(std::memory_order_relaxed)) return false;

	m_title = title;
	m_cancelled.store(false, std::memory_order_relaxed);
	m_paused.store(false, std::memory_order_relaxed);
	m_running.store(true, std::memory_order_relaxed);

	if (m_thread) {
		m_thread->quit();
		m_thread->wait();
		delete m_thread;
		m_thread = nullptr;
	}

	m_thread = QThread::create([this, task = std::move(task)]() mutable { run(std::move(task)); });
	m_thread->setParent(this);   // Qt parent ownership: destroyed with this object.
	m_thread->start();

	emit started(title);
	return true;
}

void TaskRunner::run(TaskFunction task) {
	bool success = false;
	try {
		const ProgressCallback progress = [this](std::int64_t done, std::int64_t total,
			const std::string& message) {
			// Pause is cooperative and checked here, so a paused export stops
			// between files rather than mid-write.
			while (m_paused.load(std::memory_order_relaxed)
				&& !m_cancelled.load(std::memory_order_relaxed)) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			if (m_cancelled.load(std::memory_order_relaxed)) return false;

			// Queued so the GUI thread receives it on its own event loop.
			QMetaObject::invokeMethod(this, [this, done, total, message]() {
				emit progressed(done, total, QString::fromStdString(message));
			}, Qt::QueuedConnection);
			return true;
		};

		success = task(progress);
	} catch (const std::exception&) {
		// FRD section 15: exceptions are converted at the job boundary into a
		// durable failure, never allowed to cross a thread boundary.
		success = false;
	} catch (...) {
		success = false;
	}

	m_running.store(false, std::memory_order_relaxed);
	const QString title = m_title;
	QMetaObject::invokeMethod(this, [this, success, title]() {
		emit finished(success, title);
	}, Qt::QueuedConnection);
}

void TaskRunner::cancel() {
	m_cancelled.store(true, std::memory_order_relaxed);
	m_paused.store(false, std::memory_order_relaxed);
}

void TaskRunner::setPaused(bool paused) {
	m_paused.store(paused, std::memory_order_relaxed);
}

} // namespace ml::desktop
