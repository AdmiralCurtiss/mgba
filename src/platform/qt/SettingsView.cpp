/* Copyright (c) 2013-2014 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "SettingsView.h"

#include "AudioProcessor.h"
#include "ConfigController.h"

#include <QFileDialog>

using namespace QGBA;

SettingsView::SettingsView(ConfigController* controller, QWidget* parent)
 	: QWidget(parent)
 	, m_controller(controller)
{
	m_ui.setupUi(this);

	loadSetting("bios", m_ui.bios);
	loadSetting("skipBios", m_ui.skipBios);
	loadSetting("audioBuffers", m_ui.audioBufferSize);
	loadSetting("videoSync", m_ui.videoSync);
	loadSetting("audioSync", m_ui.audioSync);
	loadSetting("frameskip", m_ui.frameskip);
	loadSetting("fpsTarget", m_ui.fpsTarget);
	loadSetting("lockAspectRatio", m_ui.lockAspectRatio);
	loadSetting("rewindEnable", m_ui.rewind);
	loadSetting("rewindBufferInterval", m_ui.rewindInterval);
	loadSetting("rewindBufferCapacity", m_ui.rewindCapacity);
	loadSetting("resampleVideo", m_ui.resampleVideo);

	QString idleOptimization = loadSetting("idleOptimization");
	if (idleOptimization == "ignore") {
		m_ui.idleOptimization->setCurrentIndex(0);
	} else if (idleOptimization == "remove") {
		m_ui.idleOptimization->setCurrentIndex(1);
	} else if (idleOptimization == "detect") {
		m_ui.idleOptimization->setCurrentIndex(2);
	}

	int audioDriver = m_controller->getQtOption("audioDriver").toInt();
#ifdef BUILD_QT_MULTIMEDIA
	m_ui.audioDriver->addItem(tr("Qt Multimedia"), static_cast<int>(AudioProcessor::Driver::QT_MULTIMEDIA));
	if (audioDriver == static_cast<int>(AudioProcessor::Driver::QT_MULTIMEDIA)) {
		m_ui.audioDriver->setCurrentIndex(m_ui.audioDriver->count() - 1);
	}
#endif

#ifdef BUILD_SDL
	m_ui.audioDriver->addItem(tr("SDL"), static_cast<int>(AudioProcessor::Driver::SDL));
	if (audioDriver == static_cast<int>(AudioProcessor::Driver::SDL)) {
		m_ui.audioDriver->setCurrentIndex(m_ui.audioDriver->count() - 1);
	}
#endif

	connect(m_ui.biosBrowse, SIGNAL(clicked()), this, SLOT(selectBios()));
	connect(m_ui.buttonBox, SIGNAL(accepted()), this, SLOT(updateConfig()));
}

void SettingsView::selectBios() {
	QString filename = QFileDialog::getOpenFileName(this, tr("Select BIOS"));
	if (!filename.isEmpty()) {
		m_ui.bios->setText(filename);
	}
}

void SettingsView::updateConfig() {
	saveSetting("bios", m_ui.bios);
	saveSetting("skipBios", m_ui.skipBios);
	saveSetting("audioBuffers", m_ui.audioBufferSize);
	saveSetting("videoSync", m_ui.videoSync);
	saveSetting("audioSync", m_ui.audioSync);
	saveSetting("frameskip", m_ui.frameskip);
	saveSetting("fpsTarget", m_ui.fpsTarget);
	saveSetting("lockAspectRatio", m_ui.lockAspectRatio);
	saveSetting("rewindEnable", m_ui.rewind);
	saveSetting("rewindBufferInterval", m_ui.rewindInterval);
	saveSetting("rewindBufferCapacity", m_ui.rewindCapacity);
	saveSetting("resampleVideo", m_ui.resampleVideo);

	switch (m_ui.idleOptimization->currentIndex() + IDLE_LOOP_IGNORE) {
	case IDLE_LOOP_IGNORE:
		saveSetting("idleOptimization", "ignore");
		break;
	case IDLE_LOOP_REMOVE:
		saveSetting("idleOptimization", "remove");
		break;
	case IDLE_LOOP_DETECT:
		saveSetting("idleOptimization", "detect");
		break;
	}

	QVariant audioDriver = m_ui.audioDriver->itemData(m_ui.audioDriver->currentIndex());
	if (audioDriver != m_controller->getQtOption("audioDriver")) {
		m_controller->setQtOption("audioDriver", audioDriver);
		AudioProcessor::setDriver(static_cast<AudioProcessor::Driver>(audioDriver.toInt()));
		emit audioDriverChanged();
	}

	m_controller->write();

	emit biosLoaded(m_ui.bios->text());
}

void SettingsView::saveSetting(const char* key, const QAbstractButton* field) {
	m_controller->setOption(key, field->isChecked());
	m_controller->updateOption(key);
}

void SettingsView::saveSetting(const char* key, const QComboBox* field) {
	saveSetting(key, field->lineEdit());
}

void SettingsView::saveSetting(const char* key, const QLineEdit* field) {
	saveSetting(key, field->text());
}

void SettingsView::saveSetting(const char* key, const QSpinBox* field) {
	saveSetting(key, field->cleanText());
}

void SettingsView::saveSetting(const char* key, const QString& field) {
	m_controller->setOption(key, field);
	m_controller->updateOption(key);
}

void SettingsView::loadSetting(const char* key, QAbstractButton* field) {
	QString option = loadSetting(key);
	field->setChecked(option != "0");
}

void SettingsView::loadSetting(const char* key, QComboBox* field) {
	loadSetting(key, field->lineEdit());
}

void SettingsView::loadSetting(const char* key, QLineEdit* field) {
	QString option = loadSetting(key);
	field->setText(option);
}

void SettingsView::loadSetting(const char* key, QSpinBox* field) {
	QString option = loadSetting(key);
	field->setValue(option.toInt());
}

QString SettingsView::loadSetting(const char* key) {
	return m_controller->getOption(key);
}
