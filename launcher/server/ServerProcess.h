// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDateTime>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include "MessageLevel.h"

class ServerInstance;
class LogModel;

/// Runs one server's java process: start, console input/output, graceful stop, crash detection, optional auto-restart.
/// Owned by its ServerInstance.
class ServerProcess : public QObject {
    Q_OBJECT
   public:
    enum class State {
        Stopped,   // not running (clean exit or never started)
        Starting,  // process started, "Done (...)!" not seen yet
        Running,   // "Done (...)!" seen
        Stopping,  // "stop" sent, waiting for exit
        Crashed,   // exited without being asked to (non-zero exit, or exit while Starting/Running)
    };
    Q_ENUM(State)

    explicit ServerProcess(ServerInstance* instance);
    ~ServerProcess() override;

    State state() const { return m_state; }
    bool isActive() const { return m_state == State::Starting || m_state == State::Running || m_state == State::Stopping; }

    /// Builds the command from the instance's launch profile + settings and starts it. On failure returns false and
    /// sets *error (no Java, invalid profile, EULA not accepted, already running...).
    bool start(QString* error = nullptr);
    /// Sends "stop" and waits; force-kills after timeoutMs. Safe to call in any state.
    void stop(int timeoutMs = 180000);
    /// Immediate kill (UI must confirm first: may lose unsaved chunks).
    void kill();
    /// stop, then start again once stopped.
    void restart();
    /// Writes a console command (no leading '/') to the server's stdin.
    void sendCommand(const QString& command);

    /// Console output of the current/last run (kept after exit so crashes can be read).
    LogModel* log() const { return m_log; }
    QStringList onlinePlayers() const { return m_players; }
    QDateTime startedAt() const { return m_startedAt; }
    /// The exact command line of the last start, for the console header / debugging.
    QStringList lastCommand() const { return m_lastCommand; }

   signals:
    void stateChanged(ServerProcess::State state);
    void playersChanged(const QStringList& players);
    /// every console line, already appended to log()
    void logLine(const QString& line, MessageLevel level);

   private:
    ServerInstance* m_instance;
    QProcess* m_process = nullptr;
    LogModel* m_log = nullptr;
    State m_state = State::Stopped;
    QStringList m_players;
    QDateTime m_startedAt;
    QStringList m_lastCommand;
    QTimer m_killTimer;
    bool m_restartRequested = false;
    QList<QDateTime> m_autoRestarts;  // for limiting crash loops
    QByteArray m_partialLine;
    QTimer m_autoRestartTimer;
    bool m_keepAwake = false;
    bool m_keepLog = false;  // restart / auto-restart: append to the previous run's log instead of clearing it

    void setState(State state);
    void launcherLine(const QString& line, MessageLevel level = MessageLevel::Launcher);
    void readOutput(bool flush);
    void handleLine(const QString& line);
    void processFinished(int exitCode, QProcess::ExitStatus status);
    void setKeepAwake(bool on);
};
