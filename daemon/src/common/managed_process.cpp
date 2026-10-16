#include "managed_process.h"
#include <QTimer>
#include <csignal>
#include <iostream>

namespace WallpaperEngine::Common {

bool ManagedProcess::start(const QString& program,
                           const QStringList& args,
                           int startTimeoutMs) {
    terminate();

    if (m_process) {
        m_process->deleteLater();
    }

    m_process = new QProcess();

    m_process->start(program, args);

    if (!m_process->waitForStarted(startTimeoutMs)) {
        std::cerr << "ManagedProcess: failed to start "
                  << program.toStdString() << std::endl;
        m_process->deleteLater();
        m_process = nullptr;
        return false;
    }
    return true;
}

void ManagedProcess::terminate(int forceKillAfterMs) {
    if (!m_process) return;

    if (m_process->state() == QProcess::NotRunning) {
        m_process->deleteLater();
        m_process = nullptr;
        return;
    }

    m_process->terminate();

    if (!m_process->waitForFinished(forceKillAfterMs)) {
        forceKill(forceKillAfterMs);
    }

    m_process->deleteLater();
    m_process = nullptr;
}

void ManagedProcess::forceKill(int waitMs) {
    if (!m_process) return;

    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(waitMs);
    }
}

bool ManagedProcess::isRunning() const {
    return m_process && m_process->state() == QProcess::Running;
}

std::optional<int> ManagedProcess::processId() const {
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        return std::nullopt;
    }
    return m_process->processId();
}

bool ManagedProcess::hasProcess() const {
    return m_process != nullptr;
}

} // namespace WallpaperEngine::Common
