// SPDX-License-Identifier: GPL-3.0-or-later
// UI-001: long-running work never blocks browsing.
//
// Every Library command that can take more than a moment runs on a worker
// thread. The GUI thread keeps paging the catalogue through its own read-only
// connection while a scan or an export is in flight.
#pragma once

#include "mlapp/Library.hpp"

#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <functional>
#include <memory>

namespace ml::desktop {

/// One unit of background work. The function runs on the worker thread and
/// reports progress through the callback it is given.
using TaskFunction = std::function<bool(const ProgressCallback&)>;

/// Runs a TaskFunction on its own thread and emits Qt signals for progress.
///
/// Ownership follows Qt's parent convention: the worker is moved to a QThread
/// this object owns, and both are destroyed with it. No object has two owners.
class TaskRunner : public QObject {
	Q_OBJECT

public:
	explicit TaskRunner(QObject* parent = nullptr);
	~TaskRunner() override;

	TaskRunner(const TaskRunner&) = delete;
	TaskRunner& operator=(const TaskRunner&) = delete;

	/// Starts a task. Returns false when one is already running.
	bool start(const QString& title, TaskFunction task);

	bool isRunning() const { return m_running.load(std::memory_order_relaxed); }
	QString currentTitle() const { return m_title; }

	/// Requests cancellation at the next checkpoint.
	void cancel();
	bool cancelRequested() const { return m_cancelled.load(std::memory_order_relaxed); }

	/// Pauses progress at the next checkpoint. The task stays alive.
	void setPaused(bool paused);
	bool isPaused() const { return m_paused.load(std::memory_order_relaxed); }

signals:
	void started(const QString& title);
	void progressed(qint64 done, qint64 total, const QString& message);
	void finished(bool success, const QString& title);

private:
	void run(TaskFunction task);

	QThread* m_thread = nullptr;
	QString m_title;
	std::atomic<bool> m_running{false};
	std::atomic<bool> m_cancelled{false};
	std::atomic<bool> m_paused{false};
};

} // namespace ml::desktop
