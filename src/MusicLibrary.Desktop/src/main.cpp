// SPDX-License-Identifier: GPL-3.0-or-later
// The desktop application entry point.
//
// Installation and first launch never start a music rewrite (FRD section 16):
// this opens a window and waits for the user.
#include "MainWindow.hpp"
#include "Theme.hpp"

#include <mlversion/Version.hpp>

#include <QApplication>
#include <QCommandLineParser>
#include <QPixmap>
#include <QStyleFactory>
#include <QTimer>

#include <cstdio>

int main(int argc, char** argv) {
	QApplication application(argc, argv);

	QCoreApplication::setOrganizationName(QStringLiteral("blueangelcpt"));
	QCoreApplication::setApplicationName(QStringLiteral("Resonance"));
	QCoreApplication::setApplicationVersion(
		QString::fromUtf8(ml::version::kVersion.data(),
			static_cast<int>(ml::version::kVersion.size())));

	QCommandLineParser parser;
	parser.setApplicationDescription(
		QString::fromUtf8(ml::version::kDescription.data(),
			static_cast<int>(ml::version::kDescription.size())));
	parser.addHelpOption();
	parser.addVersionOption();

	const QCommandLineOption sourceOption(QStringLiteral("source"),
		QStringLiteral("Open this source folder on startup. Never written to."),
		QStringLiteral("path"));
	const QCommandLineOption outputOption(QStringLiteral("output"),
		QStringLiteral("Output root for exported copies."), QStringLiteral("path"));
	const QCommandLineOption dataOption(QStringLiteral("data"),
		QStringLiteral("Catalogue and cache directory."), QStringLiteral("path"));
	const QCommandLineOption offlineOption(QStringLiteral("offline"),
		QStringLiteral("Disable all network lookups."));
	// A smoke test that proves the interface composes and paints, usable from CI
	// with the offscreen platform plugin.
	const QCommandLineOption screenshotOption(QStringLiteral("screenshot"),
		QStringLiteral("Render the window to a PNG and exit."), QStringLiteral("file"));
	const QCommandLineOption sizeOption(QStringLiteral("size"),
		QStringLiteral("Window size for --screenshot, as WIDTHxHEIGHT."),
		QStringLiteral("geometry"), QStringLiteral("1680x1000"));

	parser.addOption(sourceOption);
	parser.addOption(outputOption);
	parser.addOption(dataOption);
	parser.addOption(offlineOption);
	const QCommandLineOption selectOption(QStringLiteral("select"),
		QStringLiteral("Filter the track list to this text and select the first match."),
		QStringLiteral("text"));
	const QCommandLineOption analyseOption(QStringLiteral("analyse"),
		QStringLiteral("Analyse the selected track's spectrum on startup."));

	const QCommandLineOption playOption(QStringLiteral("play"),
		QStringLiteral("Start playing the selected track on startup."));

	parser.addOption(playOption);
	parser.addOption(selectOption);
	parser.addOption(analyseOption);
	parser.addOption(screenshotOption);
	parser.addOption(sizeOption);
	parser.process(application);

	// The Resonance visual language, applied once for the whole application.
	application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
	application.setFont(ml::desktop::theme::bodyFont(9));
	application.setStyleSheet(ml::desktop::theme::applicationStyleSheet());

	ml::desktop::MainWindow window;

	if (parser.isSet(sizeOption)) {
		const QStringList parts = parser.value(sizeOption).split(QLatin1Char('x'));
		if (parts.size() == 2) {
			window.resize(parts[0].toInt(), parts[1].toInt());
		}
	}

	if (parser.isSet(sourceOption)) {
		window.openLibraryAt(parser.value(sourceOption), parser.value(outputOption),
			parser.value(dataOption), parser.isSet(offlineOption));
	}

	window.show();

	if (parser.isSet(selectOption)) {
		QTimer::singleShot(300, &window, [&window, &parser, selectOption, analyseOption, playOption]() {
			window.selectTrackMatching(parser.value(selectOption));
			if (parser.isSet(analyseOption)) window.analyseSelectedTrack();
			if (parser.isSet(playOption)) window.playSelectedTrack();
		});
	} else if (parser.isSet(analyseOption)) {
		QTimer::singleShot(300, &window, [&window]() {
			window.selectFirstTrack();
			window.analyseSelectedTrack();
		});
	}

	if (parser.isSet(screenshotOption)) {
		const QString path = parser.value(screenshotOption);
		// Let the event loop settle so every panel has laid out and painted once
		// before the frame is captured.
		QTimer::singleShot(1400, &window, [&window, path]() {
			if (!window.hasSelection()) window.selectFirstTrack();
			QTimer::singleShot(700, &window, [&window, path]() {
				const QPixmap frame = window.grab();
				const bool saved = frame.save(path);
				std::fprintf(saved ? stdout : stderr, "%s %s\n",
					saved ? "wrote" : "could not write", qPrintable(path));
				QCoreApplication::exit(saved ? 0 : 1);
			});
		});
	}

	return application.exec();
}
