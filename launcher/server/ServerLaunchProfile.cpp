// SPDX-License-Identifier: GPL-3.0-only
#include "server/ServerLaunchProfile.h"

#include <QCollator>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>

namespace {

QString kindToString(ServerLaunchProfile::Kind kind)
{
    switch (kind) {
        case ServerLaunchProfile::Kind::Jar:
            return "jar";
        case ServerLaunchProfile::Kind::ArgsFile:
            return "argsfile";
        case ServerLaunchProfile::Kind::Unknown:
            break;
    }
    return "unknown";
}

ServerLaunchProfile::Kind kindFromString(const QString& kind)
{
    if (kind == "jar")
        return ServerLaunchProfile::Kind::Jar;
    if (kind == "argsfile")
        return ServerLaunchProfile::Kind::ArgsFile;
    return ServerLaunchProfile::Kind::Unknown;
}

QJsonArray toArray(const QStringList& list)
{
    QJsonArray out;
    for (const auto& s : list)
        out.append(s);
    return out;
}

QStringList fromArray(const QJsonValue& value)
{
    QStringList out;
    for (const auto& v : value.toArray())
        out.append(v.toString());
    return out;
}

}  // namespace

QString ServerLaunchProfile::toJson() const
{
    QJsonObject obj;
    obj["kind"] = kindToString(kind);
    obj["target"] = target;
    obj["jvmArgs"] = toArray(jvmArgs);
    obj["programArgs"] = toArray(programArgs);
    obj["detectedFrom"] = detectedFrom;
    obj["requiredJavaMajor"] = requiredJavaMajor;
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

ServerLaunchProfile ServerLaunchProfile::fromJson(const QString& json)
{
    ServerLaunchProfile profile;
    if (json.trimmed().isEmpty())
        return profile;
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[Server] Invalid launch profile JSON:" << error.errorString();
        return profile;
    }
    auto obj = doc.object();
    profile.kind = kindFromString(obj.value("kind").toString());
    profile.target = obj.value("target").toString();
    profile.jvmArgs = fromArray(obj.value("jvmArgs"));
    profile.programArgs = fromArray(obj.value("programArgs"));
    profile.detectedFrom = obj.value("detectedFrom").toString();
    profile.requiredJavaMajor = obj.value("requiredJavaMajor").toInt();
    return profile;
}

namespace ServerLaunchDetector {
namespace {

#ifdef Q_OS_WIN
constexpr bool s_preferBat = true;
#else
constexpr bool s_preferBat = false;
#endif

/// Java version needed by a Minecraft version ("1.20.1" -> 17). 0 if unknown.
int javaForMinecraft(const QString& mcVersion)
{
    static const QRegularExpression re(R"(^1\.(\d+)(?:\.(\d+))?)");
    auto m = re.match(mcVersion);
    if (!m.hasMatch())
        return 0;
    int minor = m.captured(1).toInt();
    int patch = m.captured(2).isEmpty() ? 0 : m.captured(2).toInt();
    if (minor > 20 || (minor == 20 && patch >= 5))
        return 21;
    if (minor >= 18)
        return 17;
    if (minor == 17)
        return 16;
    return 8;
}

bool isMemoryFlag(const QString& arg)
{
    static const QStringList prefixes = { "-Xmx",       "-Xms",           "-XX:MaxRAM",          "-XX:MinRAM",
                                          "-XX:InitialRAM", "-XX:MaxHeapSize", "-XX:InitialHeapSize" };
    return std::any_of(prefixes.begin(), prefixes.end(), [&arg](const QString& p) { return arg.startsWith(p); });
}

bool isJavaExecutable(const QString& token)
{
    QString name = QDir::fromNativeSeparators(token).section('/', -1).toLower();
    return name == "java" || name == "java.exe" || name == "javaw" || name == "javaw.exe";
}

bool isPassThrough(const QString& token)
{
    return token == "%*" || token == "$@" || token == "$*";
}

/// Splits a command line into tokens. Double quotes group (and are removed); in shell mode single quotes and backslash
/// escapes are handled too.
QStringList tokenize(const QString& line, bool shell)
{
    QStringList tokens;
    QString current;
    bool inToken = false;
    QChar quote;
    for (qsizetype i = 0; i < line.size(); ++i) {
        QChar c = line.at(i);
        if (!quote.isNull()) {
            if (c == quote) {
                quote = QChar();
            } else if (shell && quote == '"' && c == '\\' && i + 1 < line.size() && QString("\"\\$`").contains(line.at(i + 1))) {
                current += line.at(++i);
            } else {
                current += c;
            }
            continue;
        }
        if (c == '"' || (shell && c == '\'')) {
            quote = c;
            inToken = true;
        } else if (shell && c == '\\' && i + 1 < line.size()) {
            current += line.at(++i);
            inToken = true;
        } else if (c.isSpace()) {
            if (inToken) {
                tokens.append(current);
                current.clear();
                inToken = false;
            }
        } else {
            current += c;
            inToken = true;
        }
    }
    if (inToken)
        tokens.append(current);
    return tokens;
}

bool isCommentLine(const QString& trimmed, bool shell)
{
    if (shell)
        return trimmed.startsWith('#');
    auto lower = trimmed.toLower();
    if (lower.startsWith('@'))
        lower = lower.mid(1);
    return lower.startsWith("::") || lower == "rem" || lower.startsWith("rem ") || lower.startsWith("rem\t");
}

/// Substitutes %VAR% (bat) or $VAR / ${VAR} (sh). Returns false if something could not be resolved.
bool substituteVars(QString& text, const QHash<QString, QString>& vars, bool shell)
{
    static const QRegularExpression batVar(R"(%([A-Za-z_][A-Za-z0-9_]*)%)");
    static const QRegularExpression batLeftover(R"(%[^%\s]+%)");
    static const QRegularExpression shVar(R"(\$\{?([A-Za-z_][A-Za-z0-9_]*)\}?)");
    if (!shell) {
        // %~dp0 = folder of the script = the server folder (our working directory)
        if (text.contains("%~dp0", Qt::CaseInsensitive)) {
            text.replace("%~dp0", "", Qt::CaseInsensitive);
            while (text.startsWith('\\') || text.startsWith('/'))
                text.remove(0, 1);
        }
    }
    const auto& re = shell ? shVar : batVar;
    for (int guard = 0; guard < 20; ++guard) {
        auto m = re.match(text);
        if (!m.hasMatch())
            return shell ? !text.contains('$') : !(text.contains(batLeftover) || text.contains("%~"));
        auto key = shell ? m.captured(1) : m.captured(1).toUpper();
        if (!vars.contains(key))
            return false;
        text.replace(m.capturedStart(), m.capturedLength(), vars.value(key));
    }
    return false;
}

struct ScriptResult {
    bool ok = false;
    ServerLaunchProfile profile;
};

/// Parses a "java ..." command (tokenized, java executable included) into a profile.
ScriptResult parseJavaTokens(const QStringList& tokens, const QString& serverDir)
{
    ScriptResult result;
    auto& p = result.profile;
    QStringList jvm;
    qsizetype jarIndex = tokens.indexOf("-jar");
    if (jarIndex > 0) {
        if (jarIndex + 1 >= tokens.size())
            return result;
        // everything before -jar is for the JVM, including JVM @argfiles such as GTNH's @java9args.txt
        jvm = tokens.mid(1, jarIndex - 1);
        p.kind = ServerLaunchProfile::Kind::Jar;
        p.target = QDir::fromNativeSeparators(tokens.at(jarIndex + 1));
        p.programArgs = tokens.mid(jarIndex + 2);
    } else {
        // no -jar: the last @argsfile is the launch target (modern Forge run.bat: @user_jvm_args.txt @libraries/.../win_args.txt)
        qsizetype targetIndex = -1;
        for (qsizetype k = 1; k < tokens.size(); ++k) {
            if (tokens.at(k).startsWith('@'))
                targetIndex = k;
        }
        if (targetIndex < 0)
            return result;  // main class / classpath launches are not supported
        jvm = tokens.mid(1, targetIndex - 1);
        p.kind = ServerLaunchProfile::Kind::ArgsFile;
        p.target = QDir::fromNativeSeparators(tokens.at(targetIndex).mid(1));
        p.programArgs = tokens.mid(targetIndex + 1);
        // user_jvm_args.txt only holds the user's memory settings, which come from Elytra's settings instead
        jvm.removeIf([](const QString& a) { return a.startsWith('@') && a.contains("user_jvm_args"); });
    }
    if (p.target.isEmpty() || !QFileInfo::exists(QDir(serverDir).filePath(p.target)))
        return result;
    for (const auto& a : jvm) {
        if (!isMemoryFlag(a))
            p.jvmArgs.append(a);
    }
    p.programArgs.removeIf(isPassThrough);
    result.ok = true;
    return result;
}

ScriptResult parseScript(const QString& path, const QString& serverDir)
{
    ScriptResult result;
    bool shell = path.endsWith(".sh", Qt::CaseInsensitive) || path.endsWith(".command", Qt::CaseInsensitive);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return result;
    auto content = QString::fromUtf8(file.readAll());

    QHash<QString, QString> vars;
    static const QRegularExpression batSet(R"re(^@?set\s+"?([A-Za-z_][A-Za-z0-9_]*)=([^"]*)"?\s*$)re", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression shSet(R"(^(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)=(.*)$)");

    for (auto line : content.split('\n')) {
        line = line.trimmed();
        if (line.isEmpty() || isCommentLine(line, shell))
            continue;

        if (auto m = (shell ? shSet : batSet).match(line); m.hasMatch()) {
            auto value = m.captured(2).trimmed();
            if (shell)
                value = tokenize(value, true).join(' ');
            if (substituteVars(value, vars, shell))
                vars.insert(shell ? m.captured(1) : m.captured(1).toUpper(), value);
            continue;
        }

        auto tokens = tokenize(line, shell);
        // strip launch prefixes
        while (!tokens.isEmpty()) {
            if (!shell && tokens.first().startsWith('@'))
                tokens.first().remove(0, 1);
            auto first = tokens.first().toLower();
            if (first == "exec" || first == "call" || first == "nohup" || first == "do" || first == "then") {
                tokens.removeFirst();
                continue;
            }
            break;
        }
        if (tokens.isEmpty() || !isJavaExecutable(tokens.first()))
            continue;

        // resolve variables in every argument (the java executable itself is dropped anyway); a variable may expand
        // to several arguments (e.g. set JAVA_ARGS=-Xmx4G -Dfoo=bar)
        QStringList expanded{ tokens.first() };
        for (qsizetype k = 1; k < tokens.size(); ++k) {
            auto t = tokens.at(k);
            if (isPassThrough(t)) {
                continue;
            }
            auto original = t;
            if (!substituteVars(t, vars, shell)) {
                qDebug() << "[Server] Skipping start script with unresolved variables:" << path << original;
                return result;
            }
            if (t == original)
                expanded.append(t);
            else
                expanded.append(tokenize(t, shell));
        }

        result = parseJavaTokens(expanded, serverDir);
        if (result.ok)
            result.profile.detectedFrom = QFileInfo(path).fileName();
        else
            qDebug() << "[Server] Could not use the java command in" << path;
        return result;
    }
    return result;
}

void finalize(ServerLaunchProfile& p, const QString& serverDir)
{
    // without nogui the server opens its own Swing window
    if (!p.programArgs.contains("nogui") && !p.programArgs.contains("--nogui"))
        p.programArgs.append("nogui");
    // lwjgl3ify (GTNH and other 1.7.10 packs on modern Java) needs Java 17+
    if (p.target.contains("lwjgl3ify", Qt::CaseInsensitive) || QFileInfo::exists(QDir(serverDir).filePath("lwjgl3ify-forgePatches.jar")))
        p.requiredJavaMajor = std::max(p.requiredJavaMajor, 17);
}

QString newestVersionDir(const QString& baseDir, const QString& argsFile)
{
    QDir dir(baseDir);
    if (!dir.exists())
        return {};
    auto versions = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    versions.removeIf([&](const QString& v) { return !QFileInfo::exists(dir.filePath(v + "/" + argsFile)); });
    if (versions.isEmpty())
        return {};
    QCollator collator;
    collator.setNumericMode(true);
    std::sort(versions.begin(), versions.end(), [&](const QString& a, const QString& b) { return collator.compare(a, b) < 0; });
    return versions.last();
}

bool detectArgsFile(const QString& serverDir, ServerLaunchProfile& p)
{
#ifdef Q_OS_WIN
    const QString argsFile = "win_args.txt";
#else
    const QString argsFile = "unix_args.txt";
#endif
    QDir root(serverDir);
    if (auto v = newestVersionDir(root.filePath("libraries/net/minecraftforge/forge"), argsFile); !v.isEmpty()) {
        p.kind = ServerLaunchProfile::Kind::ArgsFile;
        p.target = "libraries/net/minecraftforge/forge/" + v + "/" + argsFile;
        p.detectedFrom = QString("Forge %1").arg(v);
        p.requiredJavaMajor = javaForMinecraft(v.section('-', 0, 0));
        return true;
    }
    if (auto v = newestVersionDir(root.filePath("libraries/net/neoforged/neoforge"), argsFile); !v.isEmpty()) {
        p.kind = ServerLaunchProfile::Kind::ArgsFile;
        p.target = "libraries/net/neoforged/neoforge/" + v + "/" + argsFile;
        p.detectedFrom = QString("NeoForge %1").arg(v);
        // NeoForge 20.4.x = MC 1.20.4, 21.x = MC 1.21.x
        auto major = v.section('.', 0, 0).toInt();
        auto minor = v.section('.', 1, 1).toInt();
        p.requiredJavaMajor = (major > 20 || (major == 20 && minor >= 5)) ? 21 : 17;
        return true;
    }
    if (auto v = newestVersionDir(root.filePath("libraries/net/neoforged/forge"), argsFile); !v.isEmpty()) {
        // NeoForge for 1.20.1 still used the old forge coordinates
        p.kind = ServerLaunchProfile::Kind::ArgsFile;
        p.target = "libraries/net/neoforged/forge/" + v + "/" + argsFile;
        p.detectedFrom = QString("NeoForge %1").arg(v);
        p.requiredJavaMajor = javaForMinecraft(v.section('-', 0, 0));
        return true;
    }
    return false;
}

bool detectLauncherJar(const QString& serverDir, ServerLaunchProfile& p)
{
    QDir root(serverDir);
    if (root.exists("fabric-server-launch.jar")) {
        p.kind = ServerLaunchProfile::Kind::Jar;
        p.target = "fabric-server-launch.jar";
        p.detectedFrom = "Fabric server launcher";
        return true;
    }
    if (root.exists("quilt-server-launch.jar")) {
        p.kind = ServerLaunchProfile::Kind::Jar;
        p.target = "quilt-server-launch.jar";
        p.detectedFrom = "Quilt server launcher";
        return true;
    }
    return false;
}

bool detectScript(const QString& serverDir, ServerLaunchProfile& p)
{
    QDir root(serverDir);
    const QStringList patterns = { "startserver*", "run", "ServerStart*", "start*", "LaunchServer*" };
    QStringList ordered;
    for (bool bat : { s_preferBat, !s_preferBat }) {
        for (const auto& pattern : patterns) {
            auto files = root.entryList({ pattern + (bat ? ".bat" : ".sh"), pattern + (bat ? ".cmd" : ".command") },
                                        QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
            for (const auto& f : files) {
                if (!ordered.contains(f, Qt::CaseInsensitive))
                    ordered.append(f);
            }
        }
    }
    static const QRegularExpression javaInName(R"(java[-_]?(\d+))", QRegularExpression::CaseInsensitiveOption);
    for (const auto& f : ordered) {
        auto r = parseScript(root.filePath(f), serverDir);
        if (!r.ok)
            continue;
        p = r.profile;
        if (auto m = javaInName.match(f); m.hasMatch())
            p.requiredJavaMajor = std::max(p.requiredJavaMajor, m.captured(1).toInt());
        return true;
    }
    return false;
}

bool detectSingleJar(const QString& serverDir, ServerLaunchProfile& p)
{
    QDir root(serverDir);
    auto jars = root.entryList({ "*.jar" }, QDir::Files, QDir::Name | QDir::IgnoreCase);
    jars.removeIf([](const QString& j) { return j.contains("installer", Qt::CaseInsensitive); });
    if (jars.isEmpty())
        return false;

    static const QRegularExpression forgeUniversal(R"(^forge-.*-universal\.jar$)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression forgeAny(R"(^forge-.*\.jar$)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression vanilla(R"(^minecraft_server\..+\.jar$)", QRegularExpression::CaseInsensitiveOption);

    auto findMatch = [&jars](const QRegularExpression& re) -> QString {
        for (const auto& j : jars) {
            if (re.match(j).hasMatch())
                return j;
        }
        return {};
    };

    QString pick = findMatch(forgeUniversal);
    if (pick.isEmpty())
        pick = findMatch(forgeAny);
    if (pick.isEmpty() && jars.contains("server.jar", Qt::CaseInsensitive))
        pick = "server.jar";
    if (pick.isEmpty())
        pick = findMatch(vanilla);
    if (pick.isEmpty() && jars.size() == 1)
        pick = jars.first();
    if (pick.isEmpty())
        return false;

    p.kind = ServerLaunchProfile::Kind::Jar;
    p.target = pick;
    p.detectedFrom = QString("Server jar (%1)").arg(pick);
    static const QRegularExpression mcInName(R"((?:forge-|minecraft_server\.)(1\.\d+(?:\.\d+)?))", QRegularExpression::CaseInsensitiveOption);
    if (auto m = mcInName.match(pick); m.hasMatch())
        p.requiredJavaMajor = javaForMinecraft(m.captured(1));
    return true;
}

}  // namespace

ServerLaunchProfile detect(const QString& serverDir)
{
    ServerLaunchProfile p;
    if (!QDir(serverDir).exists()) {
        qWarning() << "[Server] Cannot detect launch profile, folder does not exist:" << serverDir;
        return p;
    }
    if (detectArgsFile(serverDir, p) || detectLauncherJar(serverDir, p) || detectScript(serverDir, p) || detectSingleJar(serverDir, p)) {
        finalize(p, serverDir);
        qDebug() << "[Server] Detected launch profile in" << serverDir << ":" << p.toJson();
        return p;
    }
    qWarning() << "[Server] Could not detect how to start the server in" << serverDir;
    return {};
}

}  // namespace ServerLaunchDetector
