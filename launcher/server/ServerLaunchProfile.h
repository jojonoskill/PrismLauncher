// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>
#include <QStringList>

/// How a server installation is started. Filled in by ServerLaunchDetector after install, stored in server.cfg,
/// and turned into a java command line by ServerProcess. Memory (-Xms/-Xmx) never lives here: it always comes
/// from the server's settings.
struct ServerLaunchProfile {
    enum class Kind {
        Unknown,
        Jar,       // java <jvmArgs> -jar <target> <programArgs>
        ArgsFile,  // java <jvmArgs> @<target> <programArgs>   (modern Forge/NeoForge: libraries/.../win_args.txt)
    };

    Kind kind = Kind::Unknown;
    QString target;           // path relative to the server folder, '/' separators
    QStringList jvmArgs;      // extra JVM args (e.g. GTNH's @java9args.txt, -Dfml.readTimeout=180); never -Xmx/-Xms
    QStringList programArgs;  // e.g. "nogui"
    QString detectedFrom;     // human readable origin, e.g. "startserver-java9.bat" or "Fabric server launcher"
    int requiredJavaMajor = 0;  // 0 = unknown

    bool isValid() const { return kind != Kind::Unknown && !target.isEmpty(); }

    /// serialization for server.cfg
    QString toJson() const;
    static ServerLaunchProfile fromJson(const QString& json);
};

namespace ServerLaunchDetector {
/// Inspects an installed server folder and works out how to start it. Detectors run in order, first match wins:
/// 1. modern Forge/NeoForge args file  2. Fabric/Quilt server launcher  3. start script (startserver*.bat/.sh, run.bat/.sh,
/// java line parsed)  4. a single obvious server jar (forge-*-universal/server.jar/minecraft_server.*.jar).
/// Returns an invalid profile if nothing matched.
ServerLaunchProfile detect(const QString& serverDir);
}  // namespace ServerLaunchDetector
