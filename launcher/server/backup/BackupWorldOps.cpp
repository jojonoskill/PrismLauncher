// SPDX-License-Identifier: GPL-3.0-only
#include "server/backup/BackupWorldOps.h"

#include <archive.h>
#include <archive_entry.h>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <QThread>

#include <io/stream_reader.h>
#include <io/stream_writer.h>
#include <nbt_tags.h>
#include <algorithm>
#include <memory>
#include <sstream>

#include "GZip.h"

namespace ServerBackupOps {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("ServerBackups", text);
}

using ArchivePtr = std::unique_ptr<struct archive, int (*)(struct archive*)>;

ArchivePtr openArchive(const QString& path, QString* error)
{
    ArchivePtr a(archive_read_new(), archive_read_free);
    archive_read_support_format_all(a.get());
    archive_read_support_filter_all(a.get());
    auto wpath = QDir::toNativeSeparators(path).toStdWString();
    if (archive_read_open_filename_w(a.get(), wpath.c_str(), 64 * 1024) != ARCHIVE_OK) {
        *error = tr("Could not open the archive %1: %2").arg(path, QString::fromLocal8Bit(archive_error_string(a.get())));
        qWarning() << "[Server Backup] Failed to open archive" << path << "-" << archive_error_string(a.get());
        return ArchivePtr(nullptr, archive_read_free);
    }
    return a;
}

QString archiveError(struct archive* a)
{
    auto* text = archive_error_string(a);
    return text ? QString::fromLocal8Bit(text) : tr("unknown error");
}

QString entryName(archive_entry* entry)
{
    QString name;
    if (auto* utf8 = archive_entry_pathname_utf8(entry))
        name = QString::fromUtf8(utf8);
    if (name.isEmpty()) {
        if (auto* raw = archive_entry_pathname(entry))
            name = QString::fromLocal8Bit(raw);
    }
    return name;
}

/// Normalizes an archive entry path to "a/b/c" (no leading "./", '/' separators, no empty parts).
/// Returns false for anything that could escape the target folder (absolute paths, drive letters, "..").
bool normalizeEntryPath(QString name, QString* out, bool* isDir)
{
    name.replace('\\', '/');
    while (name.startsWith("./"))
        name = name.mid(2);
    if (name.isEmpty() || name.startsWith('/'))
        return false;
    if (name.size() >= 2 && name.at(1) == ':')  // C:...
        return false;
    *isDir = name.endsWith('/');
    const auto parts = name.split('/', Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return false;
    for (const auto& part : parts) {
        if (part == ".." || part == ".")
            return false;
#ifdef Q_OS_WIN
        if (part.contains(':'))  // alternate data streams
            return false;
#endif
    }
    *out = parts.join('/');
    return true;
}

/// target dir + relative path, guaranteed to stay inside the target dir. Empty if it would not.
QString safeJoin(const QString& targetDir, const QString& relative)
{
    const auto root = QDir::cleanPath(QDir(targetDir).absolutePath());
    const auto full = QDir::cleanPath(root + '/' + relative);
    if (!full.startsWith(root + '/'))
        return {};
    return full;
}

std::unique_ptr<nbt::tag_compound> parseNbt(const QByteArray& raw, QString* error)
{
    QByteArray data = raw;
    if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0x1f && static_cast<unsigned char>(raw[1]) == 0x8b) {
        if (!GZip::unzip(raw, data)) {
            *error = tr("not a valid gzip file");
            return nullptr;
        }
    }
    std::istringstream in(std::string(data.constData(), static_cast<size_t>(data.size())));
    try {
        auto pair = nbt::io::read_compound(in);
        if (!pair.second)
            *error = tr("empty NBT data");
        return std::move(pair.second);
    } catch (const std::exception& e) {
        *error = QString::fromLocal8Bit(e.what());
        return nullptr;
    }
}

bool serializeGzippedNbt(const nbt::tag& tag, QByteArray* out)
{
    std::ostringstream s;
    nbt::io::write_tag("", tag, s);
    const auto str = s.str();
    return GZip::zip(QByteArray(str.data(), static_cast<qsizetype>(str.size())), *out);
}

bool writeFileAtomic(const QString& path, const QByteArray& data, QString* error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = file.errorString();
        return false;
    }
    if (file.write(data) != data.size()) {
        *error = file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}

bool renameWithRetry(const QString& from, const QString& to)
{
    // Windows: antivirus / indexer / explorer can briefly hold handles on freshly written files and folders.
    for (int attempt = 1; attempt <= 10; attempt++) {
        if (QDir().rename(from, to)) {
            qInfo().noquote() << "[Server Backup] Renamed" << from << "->" << to;
            return true;
        }
        if (!QFileInfo::exists(from) || QFileInfo::exists(to))
            break;
        qWarning().noquote() << "[Server Backup] Rename" << from << "->" << to << "failed (attempt" << attempt << "of 10), retrying";
        QThread::msleep(500);
    }
    qWarning().noquote() << "[Server Backup] Rename" << from << "->" << to << "FAILED";
    return false;
}

bool isInside(const QString& path, const QString& dir)
{
    const auto p = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const auto d = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
#ifdef Q_OS_WIN
    const auto cs = Qt::CaseInsensitive;
#else
    const auto cs = Qt::CaseSensitive;
#endif
    return p.compare(d, cs) == 0 || p.startsWith(d + '/', cs);
}

QString stagingSuffix()
{
    return QStringLiteral(".elytra-restoring");
}

}  // namespace

QString normalizeUuid(const QString& uuid)
{
    QString hex;
    for (auto c : uuid) {
        if (c == '-' || c == '{' || c == '}' || c.isSpace())
            continue;
        hex += c.toLower();
    }
    static const QRegularExpression hexRe("^[0-9a-f]{32}$");
    if (!hexRe.match(hex).hasMatch())
        return {};
    return hex.mid(0, 8) + '-' + hex.mid(8, 4) + '-' + hex.mid(12, 4) + '-' + hex.mid(16, 4) + '-' + hex.mid(20);
}

QString worldIdFromUniverseDat(const QByteArray& data)
{
    QString error;
    auto root = parseNbt(data, &error);
    if (!root) {
        qWarning() << "[Server Backup] Could not parse universe.dat:" << error;
        return {};
    }
    if (!root->has_key("UUID", nbt::tag_type::String))
        return {};
    const auto& value = root->at("UUID").as<nbt::tag_string>().get();
    return normalizeUuid(QString::fromStdString(value));
}

QString readWorldId(const QString& worldDir)
{
    QFile file(QDir(worldDir).filePath("serverutilities/universe.dat"));
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[Server Backup] Could not read" << file.fileName() << file.errorString();
        return {};
    }
    return worldIdFromUniverseDat(file.readAll());
}

SuProperties parseSuProperties(const QByteArray& data)
{
    SuProperties props;
    const auto lines = QString::fromUtf8(data).split('\n');
    for (auto line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith('!'))
            continue;
        const auto eq = line.indexOf('=');
        if (eq < 0)
            continue;
        const auto key = line.left(eq).trimmed();
        const auto value = line.mid(eq + 1).trimmed();
        if (key == "createdAt") {
            bool ok = false;
            const auto ms = value.toLongLong(&ok);
            if (ok && ms > 0)
                props.created = QDateTime::fromMSecsSinceEpoch(ms);
        } else if (key == "customName") {
            props.customName = value.compare("true", Qt::CaseInsensitive) == 0;
        } else if (key == "worldId") {
            props.worldId = normalizeUuid(value);
        } else if (key == "version") {
            props.valid = true;
        }
    }
    props.valid = props.valid || props.created.isValid() || !props.worldId.isEmpty();
    return props;
}

ZipScan verifyArchive(const QString& archivePath, const Progress& progress)
{
    ZipScan scan;
    const qint64 fileSize = QFileInfo(archivePath).size();
    progress.setStatus(tr("Checking %1 (reading every file)...").arg(QFileInfo(archivePath).fileName()));
    qInfo().noquote() << "[Server Backup] Verifying archive" << archivePath << "(" << fileSize << "bytes)";

    auto a = openArchive(archivePath, &scan.error);
    if (!a)
        return scan;

    struct Entry {
        QString path;
        bool isDir;
        bool isFile;
        qint64 size;
    };
    QList<Entry> entries;
    QMap<QString, QByteArray> universeDats;  // entry path -> content
    QByteArray properties;
    bool hasProperties = false;

    for (;;) {
        archive_entry* entry = nullptr;
        const int r = archive_read_next_header(a.get(), &entry);
        if (r == ARCHIVE_EOF)
            break;
        if (r == ARCHIVE_WARN) {
            qWarning().noquote() << "[Server Backup] Warning while reading entry header:" << archiveError(a.get());
        } else if (r != ARCHIVE_OK) {
            scan.error = tr("The archive is damaged or incomplete (after %1 entries: %2).").arg(scan.entries).arg(archiveError(a.get()));
            return scan;
        }
        scan.entries++;
        const auto rawName = entryName(entry);
        QString path;
        bool isDirName = false;
        if (!normalizeEntryPath(rawName, &path, &isDirName)) {
            scan.error = tr("The archive contains an unsafe path \"%1\"; refusing to use it.").arg(rawName);
            return scan;
        }
        const auto type = archive_entry_filetype(entry);
        const bool isDir = type == AE_IFDIR || isDirName;
        const bool isFile = !isDir && type == AE_IFREG;

        const bool keep = !isDir && (path == suPropertiesName || path.endsWith("serverutilities/universe.dat"));
        QByteArray content;
        qint64 bytes = 0;
        const void* buff = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;
        int dr = ARCHIVE_OK;
        while ((dr = archive_read_data_block(a.get(), &buff, &size, &offset)) == ARCHIVE_OK) {
            bytes += static_cast<qint64>(size);
            if (keep && content.size() < 16 * 1024 * 1024)
                content.append(static_cast<const char*>(buff), static_cast<qsizetype>(size));
        }
        if (dr != ARCHIVE_EOF) {
            // ARCHIVE_WARN here is how libarchive reports a CRC mismatch
            scan.error = tr("The archive is damaged: \"%1\" could not be read correctly (%2).").arg(rawName, archiveError(a.get()));
            qWarning().noquote() << "[Server Backup] Verification failed at" << rawName << "-" << archiveError(a.get());
            return scan;
        }
        if (isFile && archive_entry_size_is_set(entry) && bytes != archive_entry_size(entry)) {
            scan.error = tr("The archive is damaged: \"%1\" has %2 bytes instead of %3.").arg(rawName).arg(bytes).arg(archive_entry_size(entry));
            return scan;
        }
        if (path == suPropertiesName) {
            properties = content;
            hasProperties = true;
        } else if (keep) {
            universeDats.insert(path, content);
        }
        if (!isDir && !isFile)
            qWarning().noquote() << "[Server Backup] Entry" << rawName << "is not a regular file (type" << type << "), it will be skipped";
        entries.append({ path, isDir, isFile, bytes });
        if (fileSize > 0)
            progress.setProgress(archive_filter_bytes(a.get(), -1), fileSize);
    }
    progress.setProgress(fileSize, fileSize);

    // find the world: the folder holding level.dat. Allowed: "level.dat", "<name>/level.dat", "saves/<name>/level.dat"
    QStringList candidates;
    int bestDepth = 99;
    for (const auto& e : entries) {
        if (e.isDir || !(e.path == "level.dat" || e.path.endsWith("/level.dat")))
            continue;
        const auto parent = e.path.left(e.path.size() - QString("level.dat").size());  // "" or ".../"
        const auto parts = parent.split('/', Qt::SkipEmptyParts);
        const int depth = parts.size();
        if (depth > 2 || (depth == 2 && parts.first() != "saves"))
            continue;
        if (depth < bestDepth) {
            bestDepth = depth;
            candidates.clear();
        }
        if (depth == bestDepth)
            candidates << parent;
    }
    if (candidates.isEmpty()) {
        scan.error = tr("No world found in the archive (no level.dat at the top, in a top folder or in saves/<world>/).");
        return scan;
    }
    if (candidates.size() > 1) {
        scan.error = tr("The archive contains more than one world (%1); can't tell which one to use.").arg(candidates.join(", "));
        return scan;
    }
    scan.worldPrefix = candidates.first();
    qInfo().noquote() << "[Server Backup] World inside the archive:" << (scan.worldPrefix.isEmpty() ? "<archive root>" : scan.worldPrefix);

    QSet<QString> seen;
    for (const auto& e : entries) {
        const bool inWorld = e.path != suPropertiesName && e.path.startsWith(scan.worldPrefix) && e.path.size() > scan.worldPrefix.size();
        if (!inWorld) {
            if (e.path + '/' != scan.worldPrefix)
                scan.outsideWorld << e.path;
            continue;
        }
        if (e.isDir)
            continue;
        const auto key = e.path.toLower();  // Windows: names differing only in case would collide
        if (seen.contains(key)) {
            scan.error = tr("The archive contains \"%1\" twice; refusing to use it.").arg(e.path);
            return scan;
        }
        seen.insert(key);
        if (e.isFile) {
            scan.worldFiles++;
            scan.worldBytes += e.size;
        }
    }
    if (scan.worldFiles == 0) {
        scan.error = tr("The world in the archive is empty.");
        return scan;
    }

    if (hasProperties) {
        scan.properties = parseSuProperties(properties);
        scan.worldId = scan.properties.worldId;
    }
    if (scan.worldId.isEmpty()) {
        const auto universe = universeDats.value(scan.worldPrefix + "serverutilities/universe.dat");
        if (!universe.isEmpty())
            scan.worldId = worldIdFromUniverseDat(universe);
    }
    qInfo().noquote() << "[Server Backup] Archive OK:" << scan.entries << "entries," << scan.worldFiles << "world files,"
                      << scan.worldBytes << "bytes uncompressed, worldId" << (scan.worldId.isEmpty() ? "unknown" : scan.worldId) << ","
                      << scan.outsideWorld.size() << "entries outside the world";
    return scan;
}

QString extractWorld(const QString& archivePath, const ZipScan& scan, const QString& targetDir, const Progress& progress)
{
    qInfo().noquote() << "[Server Backup] Extracting" << archivePath << "into" << targetDir;
    progress.setStatus(tr("Extracting the world..."));
    QString error;
    auto a = openArchive(archivePath, &error);
    if (!a)
        return error;

    int skippedLogged = 0;
    int skipped = 0;
    int files = 0;
    qint64 written = 0;
    for (;;) {
        archive_entry* entry = nullptr;
        const int r = archive_read_next_header(a.get(), &entry);
        if (r == ARCHIVE_EOF)
            break;
        if (r != ARCHIVE_OK && r != ARCHIVE_WARN)
            return tr("The archive could not be read while extracting (%1).").arg(archiveError(a.get()));
        const auto rawName = entryName(entry);
        QString path;
        bool isDirName = false;
        if (!normalizeEntryPath(rawName, &path, &isDirName))
            return tr("The archive contains an unsafe path \"%1\"; refusing to use it.").arg(rawName);
        const auto type = archive_entry_filetype(entry);
        const bool isDir = type == AE_IFDIR || isDirName;

        const bool inWorld = path != suPropertiesName && path.startsWith(scan.worldPrefix) && path.size() > scan.worldPrefix.size();
        if (!inWorld) {
            if (path + '/' != scan.worldPrefix) {
                skipped++;
                if (skippedLogged++ < 50)
                    qDebug().noquote() << "[Server Backup] Skipping" << path << "(not part of the world)";
            }
            continue;  // next_header skips the data
        }
        const auto relative = path.mid(scan.worldPrefix.size());
        const auto target = safeJoin(targetDir, relative);
        if (target.isEmpty())
            return tr("The archive contains an unsafe path \"%1\"; refusing to use it.").arg(rawName);

        if (isDir) {
            if (!QDir().mkpath(target))
                return tr("Could not create the folder %1.").arg(target);
            continue;
        }
        if (type != AE_IFREG) {
            qWarning().noquote() << "[Server Backup] Skipping" << path << "(not a regular file)";
            continue;
        }
        if (!QDir().mkpath(QFileInfo(target).absolutePath()))
            return tr("Could not create the folder %1.").arg(QFileInfo(target).absolutePath());

        QFile out(target);
        if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            return tr("Could not create %1: %2").arg(target, out.errorString());
        const void* buff = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;
        int dr = ARCHIVE_OK;
        while ((dr = archive_read_data_block(a.get(), &buff, &size, &offset)) == ARCHIVE_OK) {
            if (offset != out.pos() && !out.seek(offset))
                return tr("Could not write %1: %2").arg(target, out.errorString());
            if (out.write(static_cast<const char*>(buff), static_cast<qint64>(size)) != static_cast<qint64>(size))
                return tr("Could not write %1: %2").arg(target, out.errorString());
            written += static_cast<qint64>(size);
        }
        if (dr != ARCHIVE_EOF)
            return tr("The archive is damaged: \"%1\" could not be read correctly (%2).").arg(rawName, archiveError(a.get()));
        if (archive_entry_mtime_is_set(entry))
            out.setFileTime(QDateTime::fromSecsSinceEpoch(archive_entry_mtime(entry)), QFileDevice::FileModificationTime);
        if (!out.flush())
            return tr("Could not write %1: %2").arg(target, out.errorString());
        out.close();
        if (out.error() != QFileDevice::NoError)
            return tr("Could not write %1: %2").arg(target, out.errorString());
        if (archive_entry_size_is_set(entry) && QFileInfo(target).size() != archive_entry_size(entry))
            return tr("%1 has the wrong size after extracting.").arg(target);

        files++;
        if (files % 25 == 0 || files == scan.worldFiles)
            progress.setStatus(tr("Extracting the world... (%1 of %2 files)").arg(files).arg(scan.worldFiles));
        progress.setProgress(written, scan.worldBytes);
    }
    if (skipped > 0)
        qInfo().noquote() << "[Server Backup] Skipped" << skipped << "archive entries that are not part of the world";
    if (files != scan.worldFiles)
        return tr("Extracted %1 files but the archive check found %2; the archive changed while restoring?").arg(files).arg(scan.worldFiles);
    qInfo().noquote() << "[Server Backup] Extracted" << files << "files," << written << "bytes";
    return {};
}

QString findWorldRoot(const QString& folder, QString* error)
{
    const QDir dir(folder);
    if (!dir.exists()) {
        if (error)
            *error = tr("The folder %1 does not exist.").arg(folder);
        return {};
    }
    if (QFileInfo(dir.filePath("level.dat")).isFile())
        return QDir::cleanPath(dir.absolutePath());
    QStringList found;
    for (const auto& sub : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (QFileInfo(QDir(sub.absoluteFilePath()).filePath("level.dat")).isFile())
            found << QDir::cleanPath(sub.absoluteFilePath());
    }
    if (found.size() == 1)
        return found.first();
    if (error) {
        *error = found.isEmpty() ? tr("%1 is not a Minecraft world (no level.dat).").arg(folder)
                                 : tr("%1 contains more than one world; choose the world folder itself.").arg(folder);
    }
    return {};
}

qint64 folderSize(const QString& path, int* fileCount)
{
    qint64 total = 0;
    int count = 0;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
        count++;
    }
    if (fileCount)
        *fileCount = count;
    return total;
}

QString copyFolder(const QString& src, const QString& dst, qint64 totalBytes, const Progress& progress)
{
    qInfo().noquote() << "[Server Backup] Copying" << src << "to" << dst;
    progress.setStatus(tr("Copying the world..."));
    const QDir srcDir(src);
    qint64 copied = 0;
    int files = 0;
    QDirIterator it(src, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const auto path = it.next();
        const auto info = it.fileInfo();
        const auto relative = srcDir.relativeFilePath(path);
        if (info.isSymLink()) {
            qWarning().noquote() << "[Server Backup] Skipping link" << path;
            continue;
        }
        const auto target = safeJoin(dst, relative);
        if (target.isEmpty())
            return tr("Unexpected path %1 while copying.").arg(path);
        if (info.isDir()) {
            if (!QDir().mkpath(target))
                return tr("Could not create the folder %1.").arg(target);
            continue;
        }
        if (!QDir().mkpath(QFileInfo(target).absolutePath()))
            return tr("Could not create the folder %1.").arg(QFileInfo(target).absolutePath());
        QFile file(path);
        if (!file.copy(target))
            return tr("Could not copy %1: %2").arg(path, file.errorString());
        if (QFileInfo(target).size() != info.size())
            return tr("%1 has the wrong size after copying (was it changed while copying?).").arg(target);
        copied += info.size();
        files++;
        if (files % 25 == 0)
            progress.setStatus(tr("Copying the world... (%1 files)").arg(files));
        progress.setProgress(copied, totalBytes);
    }
    qInfo().noquote() << "[Server Backup] Copied" << files << "files," << copied << "bytes";
    return {};
}

QString checkFreeSpace(const QString& dir, qint64 neededBytes)
{
    QStorageInfo storage(dir);
    if (!storage.isValid() || !storage.isReady()) {
        qWarning().noquote() << "[Server Backup] Could not determine free space for" << dir << "- continuing";
        return {};
    }
    const qint64 margin = qMax<qint64>(neededBytes / 10, 256LL * 1024 * 1024);
    const qint64 available = storage.bytesAvailable();
    qInfo().noquote() << "[Server Backup] Free space on" << storage.rootPath() << ":" << available << "bytes, need" << neededBytes << "+"
                      << margin << "margin";
    if (available < neededBytes + margin) {
        return tr("Not enough free disk space: %1 MiB needed, %2 MiB free on %3.")
            .arg((neededBytes + margin) / (1024 * 1024))
            .arg(available / (1024 * 1024))
            .arg(storage.rootPath());
    }
    return {};
}

QString checkWorld(const QString& worldDir)
{
    QFile file(QDir(worldDir).filePath("level.dat"));
    if (!file.exists())
        return tr("The restored world has no level.dat.");
    if (!file.open(QIODevice::ReadOnly))
        return tr("Could not read level.dat: %1").arg(file.errorString());
    QString error;
    auto root = parseNbt(file.readAll(), &error);
    if (!root)
        return tr("level.dat is damaged (%1).").arg(error);
    if (!root->has_key("Data", nbt::tag_type::Compound))
        return tr("level.dat is damaged (no Data tag).");
    return {};
}

QString moveHostPlayer(const QString& worldDir, const QString& playerUuid)
{
    const auto uuid = normalizeUuid(playerUuid);
    if (uuid.isEmpty()) {
        qWarning().noquote() << "[Server Backup] Host player: no valid player UUID given (" << playerUuid << "), skipping the move";
        return {};
    }
    const QDir world(worldDir);
    if (world.exists("players"))
        qInfo().noquote() << "[Server Backup] Host player: the world has an old name-based players/ folder; leaving it as it is";

    const auto levelPath = world.filePath("level.dat");
    QFile levelFile(levelPath);
    if (!levelFile.open(QIODevice::ReadOnly))
        return tr("Could not read level.dat: %1").arg(levelFile.errorString());
    QString error;
    auto root = parseNbt(levelFile.readAll(), &error);
    levelFile.close();
    if (!root)
        return tr("Could not read level.dat (%1).").arg(error);
    if (!root->has_key("Data", nbt::tag_type::Compound))
        return tr("level.dat is damaged (no Data tag).");
    auto& data = root->at("Data").as<nbt::tag_compound>();
    if (!data.has_key("Player", nbt::tag_type::Compound)) {
        qInfo().noquote() << "[Server Backup] Host player: level.dat has no Data/Player tag (not a singleplayer world?), nothing to move";
        return {};
    }

    const auto playerFile = world.filePath("playerdata/" + uuid + ".dat");
    if (QFileInfo::exists(playerFile)) {
        // Minecraft 1.7.10 singleplayer also writes the host to playerdata/, so this is normally the same data.
        qInfo().noquote() << "[Server Backup] Host player:" << playerFile
                          << "already exists; keeping it and leaving level.dat unchanged (the server ignores level.dat's Player tag)";
        return {};
    }

    QByteArray playerBytes;
    if (!serializeGzippedNbt(data.at("Player").get(), &playerBytes))
        return tr("Could not compress the player data.");
    {
        QString check;
        if (!parseNbt(playerBytes, &check))
            return tr("Could not convert the player data (%1).").arg(check);
    }
    if (!world.mkpath("playerdata"))
        return tr("Could not create the playerdata folder.");
    if (!writeFileAtomic(playerFile, playerBytes, &error))
        return tr("Could not write %1: %2").arg(playerFile, error);
    qInfo().noquote() << "[Server Backup] Host player: wrote level.dat Data/Player to" << playerFile;

    data.erase("Player");
    QByteArray levelBytes;
    if (!serializeGzippedNbt(*root, &levelBytes))
        return tr("Could not compress level.dat.");
    {
        QString check;
        auto reparsed = parseNbt(levelBytes, &check);
        if (!reparsed || !reparsed->has_key("Data", nbt::tag_type::Compound))
            return tr("Could not rewrite level.dat (%1).").arg(check);
    }
    if (!writeFileAtomic(levelPath, levelBytes, &error))
        return tr("Could not write level.dat: %1").arg(error);
    qInfo().noquote() << "[Server Backup] Host player: removed Data/Player from level.dat (level.dat_old left as it was)";
    return {};
}

QString preRestoreRoot(const QString& serverDir)
{
    return QDir(serverDir).filePath("elytra-pre-restore");
}

bool isPreRestoreName(const QString& name)
{
    static const QRegularExpression re("^\\d{4}-\\d{2}-\\d{2}-\\d{2}-\\d{2}-\\d{2}(-\\d+)?$");
    return re.match(name).hasMatch();
}

QDateTime preRestoreTime(const QString& name)
{
    return QDateTime::fromString(name.left(19), "yyyy-MM-dd-HH-mm-ss");
}

void discardStaging(const QString& stagingDir)
{
    if (stagingDir.isEmpty() || !stagingDir.endsWith(stagingSuffix()) || !QFileInfo::exists(stagingDir))
        return;
    qInfo().noquote() << "[Server Backup] Removing staging folder" << stagingDir;
    if (!QDir(stagingDir).removeRecursively())
        qWarning().noquote() << "[Server Backup] Could not completely remove" << stagingDir;
}

ReplaceResult prepare(const ReplaceRequest& request, const Progress& progress)
{
    ReplaceResult result;
    const auto worldDir = QDir::cleanPath(QFileInfo(request.worldDir).absoluteFilePath());
    const auto staging = worldDir + stagingSuffix();
    qInfo().noquote() << "[Server Backup] Starting" << request.what << "of" << request.sourcePath << "into" << worldDir;

    // 1. verify the source
    ZipScan scan;
    QString sourceWorld;
    qint64 bytes = 0;
    QString sourceId;
    if (request.source == ReplaceRequest::Source::Archive) {
        scan = verifyArchive(request.sourcePath, progress);
        if (!scan.error.isEmpty()) {
            result.error = scan.error;
            return result;
        }
        for (int i = 0; i < scan.outsideWorld.size() && i < 50; i++)
            qInfo().noquote() << "[Server Backup] Not part of the world, will be skipped:" << scan.outsideWorld.at(i);
        if (scan.outsideWorld.size() > 50)
            qInfo().noquote() << "[Server Backup] ..." << scan.outsideWorld.size() - 50 << "more entries outside the world";
        bytes = scan.worldBytes;
        sourceId = scan.worldId;
    } else {
        progress.setStatus(tr("Checking the world folder..."));
        QString error;
        sourceWorld = findWorldRoot(request.sourcePath, &error);
        if (sourceWorld.isEmpty()) {
            result.error = error;
            return result;
        }
        if (isInside(sourceWorld, worldDir) || isInside(worldDir, sourceWorld) || isInside(sourceWorld, staging)) {
            result.error = tr("The selected world is the server's own world folder.");
            return result;
        }
        if (auto bad = checkWorld(sourceWorld); !bad.isEmpty()) {
            result.error = bad;
            return result;
        }
        int count = 0;
        bytes = folderSize(sourceWorld, &count);
        sourceId = readWorldId(sourceWorld);
        qInfo().noquote() << "[Server Backup] Source world" << sourceWorld << ":" << count << "files," << bytes << "bytes, worldId"
                          << (sourceId.isEmpty() ? "unknown" : sourceId);
    }

    // 2. worldId
    if (request.checkWorldId) {
        const auto currentId = readWorldId(worldDir);
        qInfo().noquote() << "[Server Backup] worldId: backup" << (sourceId.isEmpty() ? "unknown" : sourceId) << ", current world"
                          << (currentId.isEmpty() ? "unknown" : currentId);
        if (!sourceId.isEmpty() && !currentId.isEmpty() && sourceId != currentId) {
            if (!request.allowWorldMismatch) {
                qWarning().noquote() << "[Server Backup] worldId mismatch, refusing (nothing was changed)";
                result.worldMismatch = true;
                result.error = tr("This backup belongs to a different world than the server's current world.");
                return result;
            }
            qWarning().noquote() << "[Server Backup] worldId mismatch, continuing because the user allowed it";
        }
    }

    // 3. disk space
    if (auto bad = checkFreeSpace(request.serverDir, bytes); !bad.isEmpty()) {
        result.error = bad;
        return result;
    }

    // 4. extract / copy into the staging folder next to the world
    if (QFileInfo::exists(staging)) {
        qWarning().noquote() << "[Server Backup] Leftover staging folder from an earlier attempt:" << staging;
        discardStaging(staging);
        if (QFileInfo::exists(staging)) {
            result.error = tr("Could not remove the old temporary folder %1.").arg(staging);
            return result;
        }
    }
    if (!QDir().mkpath(staging)) {
        result.error = tr("Could not create the temporary folder %1.").arg(staging);
        return result;
    }
    QString error = request.source == ReplaceRequest::Source::Archive ? extractWorld(request.sourcePath, scan, staging, progress)
                                                                       : copyFolder(sourceWorld, staging, bytes, progress);
    // 5. sanity check + singleplayer host fix, still only inside the staging folder
    if (error.isEmpty()) {
        progress.setStatus(tr("Checking the extracted world..."));
        error = checkWorld(staging);
    }
    if (error.isEmpty() && !request.playerUuid.isEmpty()) {
        progress.setStatus(tr("Moving the singleplayer host's data to playerdata..."));
        error = moveHostPlayer(staging, request.playerUuid);
    }
    if (!error.isEmpty()) {
        qWarning().noquote() << "[Server Backup] Preparing failed:" << error << "- the current world was not touched";
        discardStaging(staging);
        result.error = error;
        return result;
    }
    qInfo().noquote() << "[Server Backup] New world ready in" << staging;
    result.ok = true;
    result.stagingDir = staging;
    return result;
}

ReplaceResult swap(const ReplaceRequest& request, const QString& stagingDir, const Progress& progress)
{
    ReplaceResult result;
    const auto worldDir = QDir::cleanPath(QFileInfo(request.worldDir).absoluteFilePath());
    progress.setStatus(tr("Swapping in the restored world..."));

    QFileInfo worldInfo(worldDir);
    bool hasWorld = worldInfo.exists() || worldInfo.isSymLink();
    if (hasWorld && worldInfo.isDir() && !worldInfo.isSymLink() &&
        QDir(worldDir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty()) {
        qInfo().noquote() << "[Server Backup] Current world folder" << worldDir << "is empty, removing it instead of keeping a copy";
        if (QDir().rmdir(worldDir))
            hasWorld = false;
    }

    QString preDir;
    QString preWorld;
    if (hasWorld) {
        const QDir root(preRestoreRoot(request.serverDir));
        const auto stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd-HH-mm-ss");
        auto name = stamp;
        for (int i = 2; root.exists(name); i++)
            name = stamp + '-' + QString::number(i);
        preDir = root.filePath(name);
        if (!QDir().mkpath(preDir)) {
            discardStaging(stagingDir);
            result.error = tr("Could not create %1. Nothing was changed.").arg(preDir);
            return result;
        }
        preWorld = QDir(preDir).filePath(worldInfo.fileName());
        qInfo().noquote() << "[Server Backup] Moving the current world to" << preWorld;
        if (!renameWithRetry(worldDir, preWorld)) {
            QDir().rmdir(preDir);
            discardStaging(stagingDir);
            result.error = tr("Could not move the current world aside (is it open in another program, or is the server still running?). "
                              "Nothing was changed.");
            return result;
        }
        result.preRestorePath = preWorld;
    } else {
        qInfo().noquote() << "[Server Backup] The server has no world yet, nothing to keep";
    }

    qInfo().noquote() << "[Server Backup] Moving" << stagingDir << "to" << worldDir;
    if (!renameWithRetry(stagingDir, worldDir)) {
        if (!hasWorld) {
            discardStaging(stagingDir);
            result.error = tr("Could not move the new world into place.");
            return result;
        }
        qWarning().noquote() << "[Server Backup] Rolling back: moving" << preWorld << "back to" << worldDir;
        if (renameWithRetry(preWorld, worldDir)) {
            QDir().rmdir(preDir);
            discardStaging(stagingDir);
            result.preRestorePath.clear();
            result.error = tr("Could not move the new world into place. The original world was put back; nothing was changed.");
            return result;
        }
        qCritical().noquote() << "[Server Backup] ROLLBACK FAILED. Original world:" << preWorld << "restored world:" << stagingDir;
        result.error = tr("Could not move the new world into place, and moving the original world back failed too.\n"
                          "Nothing was deleted: your original world is in %1 and the restored world in %2.\n"
                          "Move one of them to %3 by hand while the server is stopped.")
                           .arg(QDir::toNativeSeparators(preWorld), QDir::toNativeSeparators(stagingDir), QDir::toNativeSeparators(worldDir));
        return result;
    }
    if (!QFileInfo(QDir(worldDir).filePath("level.dat")).isFile())
        qWarning().noquote() << "[Server Backup] level.dat missing after the swap?!";
    qInfo().noquote() << "[Server Backup]" << request.what << "done. Previous world kept at" << (preWorld.isEmpty() ? "-" : preWorld);

    // keep the newest pre-restore copies
    const QDir root(preRestoreRoot(request.serverDir));
    auto names = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    names.erase(std::remove_if(names.begin(), names.end(), [](const QString& n) { return !isPreRestoreName(n); }), names.end());
    for (int i = keepPreRestoreCopies; i < names.size(); i++) {
        const auto old = root.filePath(names.at(i));
        if (!preDir.isEmpty() && QDir::cleanPath(old) == QDir::cleanPath(preDir))
            continue;
        qInfo().noquote() << "[Server Backup] Removing old pre-restore copy" << old << "(keeping the newest" << keepPreRestoreCopies << ")";
        if (!QDir(old).removeRecursively())
            qWarning().noquote() << "[Server Backup] Could not completely remove" << old;
    }
    result.ok = true;
    return result;
}

}  // namespace ServerBackupOps
