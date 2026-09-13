// SPDX-License-Identifier: GPL-3.0-or-later
// The desktop application entry point.
//
// Installation and first launch never start a music rewrite (FRD section 16):
// this opens a window and waits for the user.
#include "MainWindow.hpp"

#include <mlversion/Version.hpp>

#include <QApplication>
#include <QCommandLineParser>
#include <QStyleFactory>

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
	parser.process(application);

	ml::desktop::MainWindow window;
	window.show();

	return application.exec();
}
