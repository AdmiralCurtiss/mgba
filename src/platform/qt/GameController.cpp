/* Copyright (c) 2013-2014 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "GameController.h"

#include "AudioProcessor.h"
#include "InputController.h"

#include <QDateTime>
#include <QThread>

#include <ctime>

extern "C" {
#include "gba/audio.h"
#include "gba/gba.h"
#include "gba/serialize.h"
#include "gba/renderers/video-software.h"
#include "gba/supervisor/config.h"
#include "util/vfs.h"
}

using namespace QGBA;
using namespace std;

const int GameController::LUX_LEVELS[10] = { 5, 11, 18, 27, 42, 62, 84, 109, 139, 183 };

GameController::GameController(QObject* parent)
	: QObject(parent)
	, m_drawContext(new uint32_t[256 * 256])
	, m_threadContext()
	, m_activeKeys(0)
	, m_logLevels(0)
	, m_gameOpen(false)
	, m_audioThread(new QThread(this))
	, m_audioProcessor(AudioProcessor::create())
	, m_videoSync(VIDEO_SYNC)
	, m_audioSync(AUDIO_SYNC)
	, m_turbo(false)
	, m_turboForced(false)
	, m_inputController(nullptr)
{
	m_renderer = new GBAVideoSoftwareRenderer;
	GBAVideoSoftwareRendererCreate(m_renderer);
	m_renderer->outputBuffer = (color_t*) m_drawContext;
	m_renderer->outputBufferStride = 256;

	GBACheatDeviceCreate(&m_cheatDevice);

	m_threadContext.state = THREAD_INITIALIZED;
	m_threadContext.debugger = 0;
	m_threadContext.frameskip = 0;
	m_threadContext.bios = 0;
	m_threadContext.renderer = &m_renderer->d;
	m_threadContext.userData = this;
	m_threadContext.rewindBufferCapacity = 0;
	m_threadContext.cheats = &m_cheatDevice;
	m_threadContext.logLevel = -1;

	m_lux.p = this;
	m_lux.sample = [] (GBALuminanceSource* context) {
		GameControllerLux* lux = static_cast<GameControllerLux*>(context);
		lux->value = 0xFF - lux->p->m_luxValue;
	};

	m_lux.readLuminance = [] (GBALuminanceSource* context) {
		GameControllerLux* lux = static_cast<GameControllerLux*>(context);
		return lux->value;
	};
	setLuminanceLevel(0);

	m_rtc.p = this;
	m_rtc.override = GameControllerRTC::NO_OVERRIDE;
	m_rtc.sample = [] (GBARTCSource* context) { };
	m_rtc.unixTime = [] (GBARTCSource* context) -> time_t {
		GameControllerRTC* rtc = static_cast<GameControllerRTC*>(context);
		switch (rtc->override) {
		case GameControllerRTC::NO_OVERRIDE:
		default:
			return time(nullptr);
		case GameControllerRTC::FIXED:
			return rtc->value;
		case GameControllerRTC::FAKE_EPOCH:
			return rtc->value + rtc->p->m_threadContext.gba->video.frameCounter * (int64_t) VIDEO_TOTAL_LENGTH / GBA_ARM7TDMI_FREQUENCY;
		}
	};

	m_threadContext.startCallback = [] (GBAThread* context) {
		GameController* controller = static_cast<GameController*>(context->userData);
		controller->m_audioProcessor->setInput(context);
		// Override the GBA object's log level to prevent stdout spew
		context->gba->logLevel = GBA_LOG_FATAL;
		context->gba->luminanceSource = &controller->m_lux;
		context->gba->rtcSource = &controller->m_rtc;
		controller->gameStarted(context);
	};

	m_threadContext.cleanCallback = [] (GBAThread* context) {
		GameController* controller = static_cast<GameController*>(context->userData);
		controller->gameStopped(context);
	};

	m_threadContext.frameCallback = [] (GBAThread* context) {
		GameController* controller = static_cast<GameController*>(context->userData);
		controller->m_pauseMutex.lock();
		if (controller->m_pauseAfterFrame) {
			GBAThreadPauseFromThread(context);
			controller->m_pauseAfterFrame = false;
			controller->gamePaused(&controller->m_threadContext);
		}
		controller->m_pauseMutex.unlock();
		controller->frameAvailable(controller->m_drawContext);
	};

	m_threadContext.logHandler = [] (GBAThread* context, enum GBALogLevel level, const char* format, va_list args) {
		GameController* controller = static_cast<GameController*>(context->userData);
		if (level == GBA_LOG_FATAL) {
			QMetaObject::invokeMethod(controller, "crashGame", Q_ARG(const QString&, QString().vsprintf(format, args)));
		} else if (!(controller->m_logLevels & level)) {
			return;
		}
		controller->postLog(level, QString().vsprintf(format, args));
	};

	m_audioThread->start(QThread::TimeCriticalPriority);
	m_audioProcessor->moveToThread(m_audioThread);
	connect(this, SIGNAL(gameStarted(GBAThread*)), m_audioProcessor, SLOT(start()));
	connect(this, SIGNAL(gameStopped(GBAThread*)), m_audioProcessor, SLOT(pause()));
	connect(this, SIGNAL(gamePaused(GBAThread*)), m_audioProcessor, SLOT(pause()));
	connect(this, SIGNAL(gameUnpaused(GBAThread*)), m_audioProcessor, SLOT(start()));

#ifdef BUILD_SDL
	connect(this, SIGNAL(frameAvailable(const uint32_t*)), this, SLOT(testSDLEvents()));
#endif
}

GameController::~GameController() {
	m_audioThread->quit();
	m_audioThread->wait();
	disconnect();
	closeGame();
	GBACheatDeviceDestroy(&m_cheatDevice);
	delete m_renderer;
	delete[] m_drawContext;
}

void GameController::setOverride(const GBACartridgeOverride& override) {
	m_threadContext.override = override;
	m_threadContext.hasOverride = true;
}

void GameController::setOptions(const GBAOptions* opts) {
	setFrameskip(opts->frameskip);
	setAudioSync(opts->audioSync);
	setVideoSync(opts->videoSync);
	setSkipBIOS(opts->skipBios);
	setRewind(opts->rewindEnable, opts->rewindBufferCapacity, opts->rewindBufferInterval);

	threadInterrupt();
	m_threadContext.idleOptimization = opts->idleOptimization;
	threadContinue();
}

#ifdef USE_GDB_STUB
ARMDebugger* GameController::debugger() {
	return m_threadContext.debugger;
}

void GameController::setDebugger(ARMDebugger* debugger) {
	threadInterrupt();
	if (m_threadContext.debugger && GBAThreadIsActive(&m_threadContext)) {
		GBADetachDebugger(m_threadContext.gba);
	}
	m_threadContext.debugger = debugger;
	if (m_threadContext.debugger && GBAThreadIsActive(&m_threadContext)) {
		GBAAttachDebugger(m_threadContext.gba, m_threadContext.debugger);
	}
	threadContinue();
}
#endif

void GameController::loadGame(const QString& path, bool dirmode) {
	closeGame();
	if (!dirmode) {
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly)) {
			return;
		}
		file.close();
	}

	m_fname = path;
	m_dirmode = dirmode;
	openGame();
}

void GameController::openGame() {
	m_gameOpen = true;

	m_pauseAfterFrame = false;

	if (m_turbo) {
		m_threadContext.sync.videoFrameWait = false;
		m_threadContext.sync.audioWait = false;
	} else {
		m_threadContext.sync.videoFrameWait = m_videoSync;
		m_threadContext.sync.audioWait = m_audioSync;
	}

	m_threadContext.gameDir = 0;
	m_threadContext.fname = strdup(m_fname.toLocal8Bit().constData());
	if (m_dirmode) {
		m_threadContext.gameDir = VDirOpen(m_threadContext.fname);
		m_threadContext.stateDir = m_threadContext.gameDir;
	} else {
		m_threadContext.rom = VFileOpen(m_threadContext.fname, O_RDONLY);
#if ENABLE_LIBZIP
		if (!m_threadContext.gameDir) {
			m_threadContext.gameDir = VDirOpenZip(m_threadContext.fname, 0);
		}
#endif
#if ENABLE_LZMA
		if (!m_threadContext.gameDir) {
			m_threadContext.gameDir = VDirOpen7z(m_threadContext.fname, 0);
		}
#endif
	}

	if (!m_bios.isNull()) {
		m_threadContext.bios = VFileOpen(m_bios.toLocal8Bit().constData(), O_RDONLY);
	}

	if (!m_patch.isNull()) {
		m_threadContext.patch = VFileOpen(m_patch.toLocal8Bit().constData(), O_RDONLY);
	}

	if (!GBAThreadStart(&m_threadContext)) {
		m_gameOpen = false;
		emit gameFailed();
	}
}

void GameController::loadBIOS(const QString& path) {
	if (m_bios == path) {
		return;
	}
	m_bios = path;
	if (m_gameOpen) {
		closeGame();
		openGame();
	}
}

void GameController::loadPatch(const QString& path) {
	if (m_gameOpen) {
		closeGame();
		m_patch = path;
		openGame();
	} else {
		m_patch = path;
	}
}

void GameController::closeGame() {
	if (!m_gameOpen) {
		return;
	}
	if (GBAThreadIsPaused(&m_threadContext)) {
		GBAThreadUnpause(&m_threadContext);
	}
	GBAThreadEnd(&m_threadContext);
	GBAThreadJoin(&m_threadContext);
	if (m_threadContext.fname) {
		free(const_cast<char*>(m_threadContext.fname));
		m_threadContext.fname = nullptr;
	}

	m_patch = QString();

	for (size_t i = 0; i < GBACheatSetsSize(&m_cheatDevice.cheats); ++i) {
		GBACheatSet* set = *GBACheatSetsGetPointer(&m_cheatDevice.cheats, i);
		GBACheatSetDeinit(set);
		delete set;
	}
	GBACheatSetsClear(&m_cheatDevice.cheats);

	m_gameOpen = false;
	emit gameStopped(&m_threadContext);
}

void GameController::crashGame(const QString& crashMessage) {
	closeGame();
	emit gameCrashed(crashMessage);
}

bool GameController::isPaused() {
	if (!m_gameOpen) {
		return false;
	}
	return GBAThreadIsPaused(&m_threadContext);
}

void GameController::setPaused(bool paused) {
	if (paused == GBAThreadIsPaused(&m_threadContext)) {
		return;
	}
	if (paused) {
		GBAThreadPause(&m_threadContext);
		emit gamePaused(&m_threadContext);
	} else {
		GBAThreadUnpause(&m_threadContext);
		emit gameUnpaused(&m_threadContext);
	}
}

void GameController::reset() {
	GBAThreadReset(&m_threadContext);
}

void GameController::threadInterrupt() {
	if (m_gameOpen) {
		GBAThreadInterrupt(&m_threadContext);
	}
}

void GameController::threadContinue() {
	if (m_gameOpen) {
		GBAThreadContinue(&m_threadContext);
	}
}

void GameController::frameAdvance() {
	m_pauseMutex.lock();
	m_pauseAfterFrame = true;
	setPaused(false);
	m_pauseMutex.unlock();
}

void GameController::setRewind(bool enable, int capacity, int interval) {
	if (m_gameOpen) {
		threadInterrupt();
		GBARewindSettingsChanged(&m_threadContext, enable ? capacity : 0, enable ? interval : 0);
		threadContinue();
	} else {
		if (enable) {
			m_threadContext.rewindBufferInterval = interval;
			m_threadContext.rewindBufferCapacity = capacity;
		} else {
			m_threadContext.rewindBufferInterval = 0;
			m_threadContext.rewindBufferCapacity = 0;
		}
	}
}

void GameController::rewind(int states) {
	threadInterrupt();
	if (!states) {
		GBARewindAll(&m_threadContext);
	} else {
		GBARewind(&m_threadContext, states);
	}
	threadContinue();
}

void GameController::keyPressed(int key) {
	int mappedKey = 1 << key;
	m_activeKeys |= mappedKey;
	updateKeys();
}

void GameController::keyReleased(int key) {
	int mappedKey = 1 << key;
	m_activeKeys &= ~mappedKey;
	updateKeys();
}

void GameController::clearKeys() {
	m_activeKeys = 0;
	updateKeys();
}

void GameController::setAudioBufferSamples(int samples) {
	threadInterrupt();
	redoSamples(samples);
	threadContinue();
	QMetaObject::invokeMethod(m_audioProcessor, "setBufferSamples", Q_ARG(int, samples));
}

void GameController::setFPSTarget(float fps) {
	threadInterrupt();
	m_threadContext.fpsTarget = fps;
	redoSamples(m_audioProcessor->getBufferSamples());
	threadContinue();
	QMetaObject::invokeMethod(m_audioProcessor, "inputParametersChanged");
}

void GameController::setSkipBIOS(bool set) {
	threadInterrupt();
	m_threadContext.skipBios = set;
	threadContinue();
}

void GameController::loadState(int slot) {
	threadInterrupt();
	GBALoadState(&m_threadContext, m_threadContext.stateDir, slot);
	threadContinue();
	emit stateLoaded(&m_threadContext);
	emit frameAvailable(m_drawContext);
}

void GameController::saveState(int slot) {
	threadInterrupt();
	GBASaveState(&m_threadContext, m_threadContext.stateDir, slot, true);
	threadContinue();
}

void GameController::setVideoSync(bool set) {
	m_videoSync = set;
	if (!m_turbo) {
		threadInterrupt();
		m_threadContext.sync.videoFrameWait = set;
		threadContinue();
	}
}

void GameController::setAudioSync(bool set) {
	m_audioSync = set;
	if (!m_turbo) {
		threadInterrupt();
		m_threadContext.sync.audioWait = set;
		threadContinue();
	}
}

void GameController::setFrameskip(int skip) {
	m_threadContext.frameskip = skip;
}

void GameController::setTurbo(bool set, bool forced) {
	if (m_turboForced && !forced) {
		return;
	}
	m_turbo = set;
	m_turboForced = set && forced;
	threadInterrupt();
	m_threadContext.sync.audioWait = set ? false : m_audioSync;
	m_threadContext.sync.videoFrameWait = set ? false : m_videoSync;
	threadContinue();
}

void GameController::setAVStream(GBAAVStream* stream) {
	threadInterrupt();
	m_threadContext.stream = stream;
	threadContinue();
}

void GameController::clearAVStream() {
	threadInterrupt();
	m_threadContext.stream = nullptr;
	threadContinue();
}

void GameController::reloadAudioDriver() {
	QMetaObject::invokeMethod(m_audioProcessor, "pause", Qt::BlockingQueuedConnection);
	int samples = m_audioProcessor->getBufferSamples();
	delete m_audioProcessor;
	m_audioProcessor = AudioProcessor::create();
	m_audioProcessor->setBufferSamples(samples);
	m_audioProcessor->moveToThread(m_audioThread);
	connect(this, SIGNAL(gameStarted(GBAThread*)), m_audioProcessor, SLOT(start()));
	connect(this, SIGNAL(gameStopped(GBAThread*)), m_audioProcessor, SLOT(pause()));
	connect(this, SIGNAL(gamePaused(GBAThread*)), m_audioProcessor, SLOT(pause()));
	connect(this, SIGNAL(gameUnpaused(GBAThread*)), m_audioProcessor, SLOT(start()));
	if (isLoaded()) {
		m_audioProcessor->setInput(&m_threadContext);
		QMetaObject::invokeMethod(m_audioProcessor, "start");
	}
}

void GameController::setLuminanceValue(uint8_t value) {
	m_luxValue = value;
	value = std::max<int>(value - 0x16, 0);
	m_luxLevel = 10;
	for (int i = 0; i < 10; ++i) {
		if (value < LUX_LEVELS[i]) {
			m_luxLevel = i;
			break;
		}
	}
}

void GameController::setLuminanceLevel(int level) {
	int value = 0x16;
	level = std::max(0, std::min(10, level));
	if (level > 0) {
		value += LUX_LEVELS[level - 1];
	}
	setLuminanceValue(value);
}

void GameController::setRealTime() {
	m_rtc.override = GameControllerRTC::NO_OVERRIDE;
}

void GameController::setFixedTime(const QDateTime& time) {
	m_rtc.override = GameControllerRTC::FIXED;
	m_rtc.value = time.toMSecsSinceEpoch() / 1000;
}

void GameController::setFakeEpoch(const QDateTime& time) {
	m_rtc.override = GameControllerRTC::FAKE_EPOCH;
	m_rtc.value = time.toMSecsSinceEpoch() / 1000;
}

void GameController::updateKeys() {
	int activeKeys = m_activeKeys;
#ifdef BUILD_SDL
	activeKeys |= m_activeButtons;
#endif
	m_threadContext.activeKeys = activeKeys;
}

void GameController::redoSamples(int samples) {
#if RESAMPLE_LIBRARY != RESAMPLE_BLIP_BUF
	float sampleRate = 0x8000;
	float ratio;
	if (m_threadContext.gba) {
		sampleRate = m_threadContext.gba->audio.sampleRate;
	}
	ratio = GBAAudioCalculateRatio(sampleRate, m_threadContext.fpsTarget, 44100);
	m_threadContext.audioBuffers = ceil(samples / ratio);
#else
	m_threadContext.audioBuffers = samples;
#endif
	if (m_threadContext.gba) {
		GBAAudioResizeBuffer(&m_threadContext.gba->audio, m_threadContext.audioBuffers);
	}
}

void GameController::setLogLevel(int levels) {
	threadInterrupt();
	m_logLevels = levels;
	threadContinue();
}

void GameController::enableLogLevel(int levels) {
	threadInterrupt();
	m_logLevels |= levels;
	threadContinue();
}

void GameController::disableLogLevel(int levels) {
	threadInterrupt();
	m_logLevels &= ~levels;
	threadContinue();
}

#ifdef BUILD_SDL
void GameController::testSDLEvents() {
	if (!m_inputController) {
		return;
	}

	m_activeButtons = m_inputController->testSDLEvents();
	updateKeys();
}
#endif
