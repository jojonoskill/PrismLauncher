// SPDX-License-Identifier: GPL-3.0-only
#include "server/ServerProcess.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringDecoder>

#include "launch/LogModel.h"
#include "server/ServerInstance.h"
#include "settings/SettingsObject.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

constexpr int s_maxLogLines = 50000;
constexpr int s_autoRestartDelayMs = 10000;
constexpr int s_autoRestartMaxCount = 3;
constexpr qint64 s_autoRestartWindowSecs = 10 * 60;
constexpr int s_maxPartialLine = 1024 * 1024;

/// number of servers currently keeping the machine awake (all on the GUI thread)
int s_keepAwakeCount = 0;

QString quoteArg(const QString& arg)
{
    if (arg.isEmpty())
        return "\"\"";
    if (arg.contains(' ') || arg.contains('\t') || arg.contains('"'))
        return '"' + QString(arg).replace('"', "\\\"") + '"';
    return arg;
}

MessageLevel guessLevel(const QString& line)
{
    if (line.contains("FATAL"))
        return MessageLevel::Fatal;
    if (line.contains("ERROR") || line.contains("Exception") || line.startsWith("\tat ") || line.startsWith("Caused by: "))
        return MessageLevel::Error;
    if (line.contains("WARN"))
        return MessageLevel::Warning;
    return MessageLevel::Info;
}

/// "[12:34:56] [Server thread/INFO] [minecraft/DedicatedServer]: Steve joined the game" -> "Steve joined the game"
QString stripLogPrefix(const QString& line)
{
    static const QRegularExpression prefix(R"(^(?:\s*\[[^\]]*\])+:?\s*)");
    auto m = prefix.match(line);
    return m.hasMatch() ? line.mid(m.capturedEnd()) : line;
}

}  // namespace

ServerProcess::ServerProcess(ServerInstance* instance) : QObject(instance), m_instance(instance)
{
    m_log = new LogModel(this);
    m_log->setMaxLines(s_maxLogLines);
    m_log->setStopOnOverflow(false);

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    // servers are console programs: don't let Windows pop up a console window for them
    m_process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] { readOutput(false); });
    connect(m_process, &QProcess::finished, this, &ServerProcess::processFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart && error != QProcess::Crashed)
            qWarning() << "[Server]" << m_instance->id() << "process error:" << error << m_process->errorString();
    });

    m_killTimer.setSingleShot(true);
    connect(&m_killTimer, &QTimer::timeout, this, [this] {
        if (m_process->state() == QProcess::NotRunning)
            return;
        launcherLine(tr("The server did not stop in time, killing it."), MessageLevel::Warning);
        m_process->kill();
    });

    m_autoRestartTimer.setSingleShot(true);
    connect(&m_autoRestartTimer, &QTimer::timeout, this, [this] {
        if (m_state != State::Crashed)
            return;
        m_keepLog = true;
        QString error;
        if (!start(&error))
            launcherLine(tr("Automatic restart failed: %1").arg(error), MessageLevel::Error);
    });
}

ServerProcess::~ServerProcess()
{
    m_killTimer.stop();
    m_autoRestartTimer.stop();
    if (m_process && m_process->state() != QProcess::NotRunning) {
        // application shutdown: give the server a chance to save the world
        disconnect(m_process, nullptr, this, nullptr);
        qInfo() << "[Server]" << m_instance->id() << "is still running, stopping it before exit";
        if (m_state != State::Stopping)
            m_process->write("stop\n");
        if (!m_process->waitForFinished(60000)) {
            qWarning() << "[Server]" << m_instance->id() << "did not stop within 60 s, killing it";
            m_process->kill();
            m_process->waitForFinished(5000);
        }
    }
    setKeepAwake(false);
}

bool ServerProcess::start(QString* error)
{
    auto fail = [this, error](const QString& message) {
        if (error)
            *error = message;
        qWarning() << "[Server]" << m_instance->id() << "cannot start:" << message;
        return false;
    };

    if (isActive() || m_process->state() != QProcess::NotRunning)
        return fail(tr("The server is already running."));

    const auto serverDir = m_instance->serverDir();
    const auto profile = m_instance->launchProfile();
    if (!profile.isValid())
        return fail(tr("Elytra does not know how to start this server. Pick the server jar in the server's settings."));
    if (!QFileInfo::exists(QDir(serverDir).filePath(profile.target)))
        return fail(tr("The server's start file %1 does not exist.").arg(profile.target));

    auto java = m_instance->javaPath();
#ifdef Q_OS_WIN
    // javaw has no console streams worth having; servers need java.exe
    if (QFileInfo javaInfo(java); javaInfo.fileName().compare("javaw.exe", Qt::CaseInsensitive) == 0) {
        auto consoleJava = javaInfo.dir().filePath("java.exe");
        if (QFileInfo::exists(consoleJava))
            java = consoleJava;
    }
#endif
    if (java.isEmpty())
        return fail(tr("No Java is set. Choose one in the server's settings or in the launcher's Java settings."));
    if (!QFileInfo(java).exists())
        return fail(tr("Java was not found at %1.").arg(java));
    if (!m_instance->eulaAccepted())
        return fail(tr("The Minecraft EULA must be accepted in Elytra before the server can start."));

    auto settings = m_instance->settings();
    int maxMem = settings->get("MaxMemAlloc").toInt();
    if (maxMem <= 0)
        maxMem = 4096;
    int minMem = settings->get("MinMemAlloc").toInt();
    if (minMem <= 0 || minMem > maxMem)
        minMem = maxMem;

    QStringList args;
    args << QString("-Xms%1M").arg(minMem) << QString("-Xmx%1M").arg(maxMem);
    args << profile.jvmArgs;
    args << QProcess::splitCommand(settings->get("JvmArgs").toString());
    if (profile.kind == ServerLaunchProfile::Kind::Jar)
        args << "-jar" << profile.target;
    else
        args << ('@' + profile.target);
    args << profile.programArgs;

    m_autoRestartTimer.stop();
    m_killTimer.stop();
    if (m_keepLog) {
        launcherLine(QString());
    } else {
        m_log->clear();
    }
    m_keepLog = false;
    m_partialLine.clear();
    if (!m_players.isEmpty()) {
        m_players.clear();
        emit playersChanged(m_players);
    }

    m_lastCommand = QStringList{ java } + args;
    QStringList quoted;
    for (const auto& a : std::as_const(m_lastCommand))
        quoted << quoteArg(a);
    launcherLine(tr("Starting server in %1").arg(QDir::toNativeSeparators(serverDir)));
    launcherLine(quoted.join(' '));

    m_process->setWorkingDirectory(serverDir);
    m_process->setProgram(java);
    m_process->setArguments(args);
    m_process->start(QIODevice::ReadWrite);
    if (!m_process->waitForStarted(15000)) {
        auto message = tr("Could not start Java: %1").arg(m_process->errorString());
        launcherLine(message, MessageLevel::Error);
        if (m_process->state() != QProcess::NotRunning)
            m_process->kill();
        return fail(message);
    }

    qInfo() << "[Server]" << m_instance->id() << "started:" << quoted.join(' ');
    m_startedAt = QDateTime::currentDateTime();
    setState(State::Starting);
    return true;
}

void ServerProcess::stop(int timeoutMs)
{
    m_autoRestartTimer.stop();
    if (m_state == State::Starting || m_state == State::Running) {
        launcherLine(tr("Stopping the server..."));
        m_process->write("stop\n");
        setState(State::Stopping);
        m_killTimer.start(timeoutMs);
    } else if (m_state != State::Stopping) {
        m_restartRequested = false;
    }
}

void ServerProcess::kill()
{
    m_autoRestartTimer.stop();
    m_restartRequested = false;
    if (m_process->state() == QProcess::NotRunning)
        return;
    launcherLine(tr("Killing the server process."), MessageLevel::Warning);
    // a kill is something the user asked for, so the exit is not a crash
    setState(State::Stopping);
    m_process->kill();
}

void ServerProcess::restart()
{
    if (isActive()) {
        m_restartRequested = true;
        stop();
        return;
    }
    m_keepLog = m_state == State::Crashed;
    QString error;
    if (!start(&error))
        launcherLine(tr("Restart failed: %1").arg(error), MessageLevel::Error);
}

void ServerProcess::sendCommand(const QString& command)
{
    if (m_process->state() != QProcess::Running)
        return;
    auto cmd = command.trimmed();
    if (cmd.startsWith('/'))
        cmd = cmd.mid(1);
    if (cmd.isEmpty())
        return;
    m_log->append(MessageLevel::Launcher, "> " + cmd);
    m_process->write((cmd + '\n').toUtf8());
    if (cmd.compare("stop", Qt::CaseInsensitive) == 0 && (m_state == State::Starting || m_state == State::Running)) {
        // typed by the user: a requested stop, not a crash
        m_autoRestartTimer.stop();
        setState(State::Stopping);
        m_killTimer.start(180000);
    }
}

void ServerProcess::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    setKeepAwake(isActive());
    emit stateChanged(state);
}

void ServerProcess::launcherLine(const QString& line, MessageLevel level)
{
    m_log->append(level, line);
    emit logLine(line, level);
}

void ServerProcess::readOutput(bool flush)
{
    m_partialLine += m_process->readAll();
    qsizetype start = 0;
    qsizetype newline;
    while ((newline = m_partialLine.indexOf('\n', start)) >= 0) {
        auto raw = m_partialLine.mid(start, newline - start);
        if (raw.endsWith('\r'))
            raw.chop(1);
        QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
        QString line = decoder.decode(raw);
        if (decoder.hasError())
            line = QString::fromLocal8Bit(raw);
        handleLine(line);
        start = newline + 1;
    }
    m_partialLine.remove(0, start);
    if ((flush || m_partialLine.size() > s_maxPartialLine) && !m_partialLine.isEmpty()) {
        auto raw = m_partialLine;
        m_partialLine.clear();
        if (raw.endsWith('\r'))
            raw.chop(1);
        QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
        QString line = decoder.decode(raw);
        if (decoder.hasError())
            line = QString::fromLocal8Bit(raw);
        handleLine(line);
    }
}

void ServerProcess::handleLine(const QString& rawLine)
{
    static const QRegularExpression ansi(R"(\x1B\[[0-9;?]*[A-Za-z])");
    QString line = rawLine;
    if (line.contains(QChar(0x1B)))
        line.remove(ansi);

    auto level = guessLevel(line);
    m_log->append(level, line);
    emit logLine(line, level);

    const auto message = stripLogPrefix(line);
    if (m_state == State::Starting && message.startsWith("Done (")) {
        setState(State::Running);
        return;
    }

    // chat lines look like "<Steve> hello" and never match: the name must come first
    static const QRegularExpression joinLeave(R"(^([A-Za-z0-9_.\-]{1,32})(?: \(formerly known as [^)]+\))? (joined|left) the game$)");
    if (auto m = joinLeave.match(message); m.hasMatch()) {
        auto name = m.captured(1);
        bool changed = false;
        if (m.captured(2) == "joined") {
            if (!m_players.contains(name)) {
                m_players.append(name);
                changed = true;
            }
        } else {
            changed = m_players.removeAll(name) > 0;
        }
        if (changed)
            emit playersChanged(m_players);
    }
}

void ServerProcess::processFinished(int exitCode, QProcess::ExitStatus status)
{
    readOutput(true);
    m_killTimer.stop();
    if (!m_players.isEmpty()) {
        m_players.clear();
        emit playersChanged(m_players);
    }

    State newState;
    if (m_state == State::Stopping)
        newState = State::Stopped;
    else if (m_state == State::Running && status == QProcess::NormalExit && exitCode == 0)
        newState = State::Stopped;  // e.g. stopped from inside the game by an operator
    else
        newState = State::Crashed;

    if (status == QProcess::CrashExit)
        launcherLine(tr("The server process was terminated."), newState == State::Crashed ? MessageLevel::Error : MessageLevel::Launcher);
    else
        launcherLine(tr("The server exited with code %1.").arg(exitCode),
                     newState == State::Crashed ? MessageLevel::Error : MessageLevel::Launcher);
    qInfo() << "[Server]" << m_instance->id() << "exited, code" << exitCode << "status" << status << "->" << newState;

    setState(newState);

    if (m_restartRequested) {
        m_restartRequested = false;
        QTimer::singleShot(0, this, [this] {
            if (isActive())
                return;
            m_keepLog = true;
            QString error;
            if (!start(&error))
                launcherLine(tr("Restart failed: %1").arg(error), MessageLevel::Error);
        });
        return;
    }

    if (newState == State::Crashed && m_instance->settings()->get("AutoRestart").toBool()) {
        auto now = QDateTime::currentDateTime();
        m_autoRestarts.removeIf([&now](const QDateTime& t) { return t.secsTo(now) > s_autoRestartWindowSecs; });
        if (m_autoRestarts.size() < s_autoRestartMaxCount) {
            m_autoRestarts.append(now);
            launcherLine(tr("The server crashed. Restarting it in %1 seconds (attempt %2 of %3)...")
                             .arg(s_autoRestartDelayMs / 1000)
                             .arg(m_autoRestarts.size())
                             .arg(s_autoRestartMaxCount),
                         MessageLevel::Warning);
            m_autoRestartTimer.start(s_autoRestartDelayMs);
        } else {
            launcherLine(tr("The server crashed %1 times within 10 minutes. It will not be restarted automatically.").arg(s_autoRestartMaxCount),
                         MessageLevel::Error);
        }
    }
}

void ServerProcess::setKeepAwake(bool on)
{
    if (on == m_keepAwake)
        return;
    m_keepAwake = on;
    s_keepAwakeCount += on ? 1 : -1;
#ifdef Q_OS_WIN
    // SetThreadExecutionState is per thread: all calls happen on the GUI thread
    if (on && s_keepAwakeCount == 1)
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    else if (!on && s_keepAwakeCount == 0)
        SetThreadExecutionState(ES_CONTINUOUS);
#endif
}
