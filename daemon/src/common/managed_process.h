#pragma once

#include <optional>
#include <string>
#include <vector>
#include <chrono>
#include <QProcess>

namespace WallpaperEngine::Common {

/** Small owned subprocess wrapper with deterministic cleanup semantics.

    Intended for short-lived daemon helper processes (OST playback, audio
    capture, one-shot probes). Not a general process runner.
 */
class ManagedProcess {
public:
    ManagedProcess() = default;
    ~ManagedProcess() { terminate(); }

    ManagedProcess(const ManagedProcess&) = delete;
    ManagedProcess& operator=(const ManagedProcess&) = delete;

    /** Start the process. Returns true if the process started within the
        given timeout. Replaces any previously started process. */
    bool start(const QString& program,
               const QStringList& args,
               int startTimeoutMs = 2000);

    /** Best-effort terminate then optional forced kill if the process is
        still running after `timeout` (default 500ms). Safe to call when
        no process is running. */
    void terminate(int forceKillAfterMs = 500);

    /** Immediate forced kill, then wait for the process to exit. */
    void forceKill(int waitMs = 300);

    bool isRunning() const;
    std::optional<int> processId() const;
    bool hasProcess() const;

    QProcess* process() { return m_process; }
    const QProcess* process() const { return m_process; }

private:
    QProcess* m_process = nullptr;
};

} // namespace WallpaperEngine::Common
