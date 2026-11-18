// SPDX-License-Identifier: GPL-3.0-only
#include "ServerSettingsPage.h"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include "Application.h"
#include "DesktopServices.h"
#include "HardwareInfo.h"
#include "server/ServerInstance.h"
#include "server/ServerLaunchProfile.h"
#include "server/ServerProcess.h"
#include "settings/SettingsObject.h"

namespace {
QLabel* makeValueLabel(QWidget* parent)
{
    auto* label = new QLabel(parent);
    label->setObjectName("serverProfileValue");
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

QString memoryNote()
{
    const auto systemMiB = HardwareInfo::totalRamMiB();
    if (systemMiB == 0)
        return {};
    return ServerSettingsPage::tr("This computer has %1 MiB of memory. Leave some for the system: the server's real usage is "
                                  "a bit above the maximum.")
        .arg(systemMiB);
}

QString kindText(ServerLaunchProfile::Kind kind)
{
    switch (kind) {
        case ServerLaunchProfile::Kind::Jar:
            return ServerSettingsPage::tr("Server jar (java -jar)");
        case ServerLaunchProfile::Kind::ArgsFile:
            return ServerSettingsPage::tr("Arguments file (modern Forge / NeoForge)");
        case ServerLaunchProfile::Kind::Unknown:
            break;
    }
    return ServerSettingsPage::tr("Not detected");
}
}  // namespace

ServerSettingsPage::ServerSettingsPage(QWidget* parent) : QWidget(parent)
{
    setObjectName("serverSettingsPage");

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget(scroll);
    content->setObjectName("serverSettingsContent");
    scroll->setWidget(content);

    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 12, 8, 12);
    layout->setSpacing(14);

    m_runningNote = new QLabel(tr("The server is running. Java, memory and start command can't be changed now; "
                                  "stop the server first. Changes take effect the next time it starts."),
                               content);
    m_runningNote->setObjectName("serverInfoNote");
    m_runningNote->setWordWrap(true);
    layout->addWidget(m_runningNote);

    // general
    {
        auto* group = new QGroupBox(tr("General"), content);
        auto* form = new QFormLayout(group);
        form->setSpacing(10);

        m_nameEdit = new QLineEdit(group);
        connect(m_nameEdit, &QLineEdit::editingFinished, this, &ServerSettingsPage::saveName);
        form->addRow(tr("Name:"), m_nameEdit);

        m_autoRestartCheck = new QCheckBox(tr("Restart the server automatically when it crashes"), group);
        connect(m_autoRestartCheck, &QCheckBox::toggled, this, [this](bool checked) {
            if (!m_loading && m_server)
                m_server->settings()->set("AutoRestart", checked);
        });
        form->addRow(QString(), m_autoRestartCheck);
        layout->addWidget(group);
    }

    // java & memory
    {
        m_javaGroup = new QGroupBox(tr("Java and memory"), content);
        auto* form = new QFormLayout(m_javaGroup);
        form->setSpacing(10);

        auto* javaRow = new QHBoxLayout();
        m_javaPathEdit = new QLineEdit(m_javaGroup);
        m_javaPathEdit->setClearButtonEnabled(true);
        connect(m_javaPathEdit, &QLineEdit::editingFinished, this, &ServerSettingsPage::saveJavaPath);
        javaRow->addWidget(m_javaPathEdit, 1);
        auto* browseJava = new QPushButton(tr("Browse..."), m_javaGroup);
        connect(browseJava, &QPushButton::clicked, this, &ServerSettingsPage::browseJava);
        javaRow->addWidget(browseJava);
        form->addRow(tr("Java:"), javaRow);

        const int systemMiB = static_cast<int>(qMin<uint64_t>(HardwareInfo::totalRamMiB(), 1024 * 1024));
        const int maxAllowed = systemMiB > 1024 ? systemMiB : 65536;

        m_maxMemSpin = new QSpinBox(m_javaGroup);
        m_maxMemSpin->setRange(512, maxAllowed);
        m_maxMemSpin->setSingleStep(512);
        m_maxMemSpin->setSuffix(tr(" MiB"));
        m_maxMemSpin->setMinimumWidth(160);
        connect(m_maxMemSpin, &QSpinBox::valueChanged, this, &ServerSettingsPage::saveMemory);
        form->addRow(tr("Maximum memory:"), m_maxMemSpin);

        m_minMemSpin = new QSpinBox(m_javaGroup);
        m_minMemSpin->setRange(0, maxAllowed);
        m_minMemSpin->setSingleStep(512);
        m_minMemSpin->setSuffix(tr(" MiB"));
        m_minMemSpin->setSpecialValueText(tr("Same as maximum"));
        m_minMemSpin->setMinimumWidth(160);
        connect(m_minMemSpin, &QSpinBox::valueChanged, this, &ServerSettingsPage::saveMemory);
        form->addRow(tr("Minimum memory:"), m_minMemSpin);

        m_memNote = new QLabel(m_javaGroup);
        m_memNote->setObjectName("serverHintLabel");
        m_memNote->setWordWrap(true);
        m_memNote->setText(memoryNote());
        form->addRow(QString(), m_memNote);

        m_jvmArgsEdit = new QLineEdit(m_javaGroup);
        m_jvmArgsEdit->setPlaceholderText(tr("e.g. -XX:+UseG1GC (added after the start command's own arguments)"));
        connect(m_jvmArgsEdit, &QLineEdit::editingFinished, this, &ServerSettingsPage::saveJvmArgs);
        form->addRow(tr("Extra JVM arguments:"), m_jvmArgsEdit);

        layout->addWidget(m_javaGroup);
    }

    // start command
    {
        m_profileGroup = new QGroupBox(tr("Start command"), content);
        auto* box = new QVBoxLayout(m_profileGroup);
        box->setSpacing(10);

        m_profileWarning = new QLabel(tr("Elytra could not work out how to start this server. Click \"Choose jar...\" and pick the "
                                         "server's jar file (usually forge-...-universal.jar, server.jar or similar)."),
                                      m_profileGroup);
        m_profileWarning->setObjectName("serverWarningNote");
        m_profileWarning->setWordWrap(true);
        box->addWidget(m_profileWarning);

        auto* form = new QFormLayout();
        form->setSpacing(8);
        m_kindValue = makeValueLabel(m_profileGroup);
        m_targetValue = makeValueLabel(m_profileGroup);
        m_detectedFromValue = makeValueLabel(m_profileGroup);
        m_jvmArgsValue = makeValueLabel(m_profileGroup);
        m_programArgsValue = makeValueLabel(m_profileGroup);
        m_javaVersionValue = makeValueLabel(m_profileGroup);
        form->addRow(tr("Method:"), m_kindValue);
        form->addRow(tr("Target:"), m_targetValue);
        form->addRow(tr("Detected from:"), m_detectedFromValue);
        form->addRow(tr("JVM arguments:"), m_jvmArgsValue);
        form->addRow(tr("Program arguments:"), m_programArgsValue);
        form->addRow(tr("Required Java:"), m_javaVersionValue);
        box->addLayout(form);

        auto* row = new QHBoxLayout();
        m_detectButton = new QPushButton(QIcon::fromTheme("refresh"), tr("Detect again"), m_profileGroup);
        m_detectButton->setToolTip(tr("Look at the server folder again (start scripts, jars) and work out the start command."));
        connect(m_detectButton, &QPushButton::clicked, this, &ServerSettingsPage::detectAgain);
        row->addWidget(m_detectButton);
        m_chooseJarButton = new QPushButton(tr("Choose jar..."), m_profileGroup);
        m_chooseJarButton->setToolTip(tr("Start the server with \"java -jar\" on a jar you pick."));
        connect(m_chooseJarButton, &QPushButton::clicked, this, &ServerSettingsPage::chooseJar);
        row->addWidget(m_chooseJarButton);
        row->addStretch(1);
        box->addLayout(row);

        layout->addWidget(m_profileGroup);
    }

    // files
    {
        auto* row = new QHBoxLayout();
        m_openFolderButton = new QPushButton(QIcon::fromTheme("viewfolder"), tr("Open server folder"), content);
        connect(m_openFolderButton, &QPushButton::clicked, this, &ServerSettingsPage::openServerFolder);
        row->addWidget(m_openFolderButton);
        m_editPropertiesButton = new QPushButton(QIcon::fromTheme("rename"), tr("Edit server.properties"), content);
        m_editPropertiesButton->setToolTip(tr("Open server.properties in your text editor. Changes apply on the next start."));
        connect(m_editPropertiesButton, &QPushButton::clicked, this, &ServerSettingsPage::editServerProperties);
        row->addWidget(m_editPropertiesButton);
        row->addStretch(1);
        layout->addLayout(row);
    }

    layout->addStretch(1);

    setServer(nullptr);
}

void ServerSettingsPage::setServer(ServerInstance* server)
{
    for (auto& connection : m_connections)
        disconnect(connection);
    m_connections.clear();

    m_server = server;
    if (server) {
        m_connections << connect(server, &ServerInstance::nameChanged, this, [this](const QString& name) {
            if (!m_nameEdit->hasFocus())
                m_nameEdit->setText(name);
        });
        if (server->process())
            m_connections << connect(server->process(), &ServerProcess::stateChanged, this, &ServerSettingsPage::updateState);
    }
    reload();
}

void ServerSettingsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!event->spontaneous())
        reload();
}

void ServerSettingsPage::reload()
{
    m_loading = true;
    const QString globalJava = APPLICATION->settings()->get("JavaPath").toString();
    m_javaPathEdit->setPlaceholderText(globalJava.isEmpty() ? tr("Use launcher default")
                                                            : tr("Use launcher default (%1)").arg(QDir::toNativeSeparators(globalJava)));
    if (m_server) {
        auto* s = m_server->settings();
        m_nameEdit->setText(m_server->name());
        m_autoRestartCheck->setChecked(s->get("AutoRestart").toBool());
        m_javaPathEdit->setText(s->get("JavaPath").toString());
        const int maxMem = s->get("MaxMemAlloc").toInt();
        m_maxMemSpin->setValue(maxMem > 0 ? maxMem : 4096);
        m_minMemSpin->setValue(s->get("MinMemAlloc").toInt());
        m_jvmArgsEdit->setText(s->get("JvmArgs").toString());
    } else {
        m_nameEdit->clear();
        m_autoRestartCheck->setChecked(false);
        m_javaPathEdit->clear();
        m_jvmArgsEdit->clear();
    }
    m_loading = false;
    updateProfile();
    updateState();
}

void ServerSettingsPage::updateState()
{
    const bool hasServer = m_server != nullptr;
    const bool active = hasServer && m_server->process() && m_server->process()->isActive();
    m_runningNote->setVisible(active);
    m_nameEdit->setEnabled(hasServer);
    m_autoRestartCheck->setEnabled(hasServer);
    m_javaGroup->setEnabled(hasServer && !active);
    m_profileGroup->setEnabled(hasServer && !active);
    m_openFolderButton->setEnabled(hasServer);
    m_editPropertiesButton->setEnabled(hasServer);
}

void ServerSettingsPage::updateProfile()
{
    if (!m_server) {
        for (auto* label : { m_kindValue, m_targetValue, m_detectedFromValue, m_jvmArgsValue, m_programArgsValue, m_javaVersionValue })
            label->clear();
        m_profileWarning->hide();
        return;
    }
    const auto profile = m_server->launchProfile();
    const QString none = tr("(none)");
    m_profileWarning->setVisible(!profile.isValid());
    m_kindValue->setText(kindText(profile.kind));
    m_targetValue->setText(profile.target.isEmpty() ? none : profile.target);
    m_detectedFromValue->setText(profile.detectedFrom.isEmpty() ? none : profile.detectedFrom);
    m_jvmArgsValue->setText(profile.jvmArgs.isEmpty() ? none : profile.jvmArgs.join(' '));
    m_programArgsValue->setText(profile.programArgs.isEmpty() ? none : profile.programArgs.join(' '));
    m_javaVersionValue->setText(profile.requiredJavaMajor > 0 ? tr("Java %1").arg(profile.requiredJavaMajor) : tr("unknown"));
}

void ServerSettingsPage::saveName()
{
    if (m_loading || !m_server)
        return;
    const QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty()) {
        m_nameEdit->setText(m_server->name());
        return;
    }
    if (name != m_server->name())
        m_server->setName(name);
}

void ServerSettingsPage::saveJavaPath()
{
    if (m_loading || !m_server)
        return;
    m_server->settings()->set("JavaPath", m_javaPathEdit->text().trimmed());
}

void ServerSettingsPage::saveMemory()
{
    if (m_loading || !m_server)
        return;
    m_server->settings()->set("MaxMemAlloc", m_maxMemSpin->value());
    m_server->settings()->set("MinMemAlloc", m_minMemSpin->value());
    if (m_minMemSpin->value() > m_maxMemSpin->value())
        m_memNote->setText(tr("The minimum is above the maximum; Java will refuse to start like this."));
    else
        m_memNote->setText(memoryNote());
}

void ServerSettingsPage::saveJvmArgs()
{
    if (m_loading || !m_server)
        return;
    m_server->settings()->set("JvmArgs", m_jvmArgsEdit->text().trimmed());
}

void ServerSettingsPage::browseJava()
{
    if (!m_server)
        return;
#ifdef Q_OS_WIN
    const QString filter = tr("Java executable (java.exe javaw.exe)");
#else
    const QString filter = tr("Java executable (java)");
#endif
    QString start = m_javaPathEdit->text();
    if (start.isEmpty())
        start = APPLICATION->settings()->get("JavaPath").toString();
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose Java for this server"), QFileInfo(start).absolutePath(), filter);
    if (path.isEmpty())
        return;
    m_javaPathEdit->setText(QDir::toNativeSeparators(path));
    saveJavaPath();
}

void ServerSettingsPage::detectAgain()
{
    if (!m_server)
        return;
    const auto profile = m_server->redetectLaunchProfile();
    updateProfile();
    if (!profile.isValid()) {
        QMessageBox::warning(this, tr("Start command not found"),
                             tr("Elytra still could not work out how to start this server. Use \"Choose jar...\" to pick the server jar."));
    }
}

void ServerSettingsPage::chooseJar()
{
    if (!m_server)
        return;
    const QDir serverDir(m_server->serverDir());
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose the server jar"), serverDir.absolutePath(), tr("Jar files (*.jar)"));
    if (path.isEmpty())
        return;

    QString relative = serverDir.relativeFilePath(path);
    if (relative.startsWith("..") || QDir::isAbsolutePath(relative)) {
        QMessageBox::warning(this, tr("Jar outside the server folder"),
                             tr("The jar has to be inside the server folder:\n%1").arg(QDir::toNativeSeparators(serverDir.absolutePath())));
        return;
    }
    relative = QDir::fromNativeSeparators(relative);

    ServerLaunchProfile profile = m_server->launchProfile();
    profile.kind = ServerLaunchProfile::Kind::Jar;
    profile.target = relative;
    profile.detectedFrom = tr("chosen by you");
    if (profile.programArgs.isEmpty())
        profile.programArgs = QStringList{ "nogui" };
    m_server->setLaunchProfile(profile);
    updateProfile();
}

void ServerSettingsPage::openServerFolder()
{
    if (m_server)
        DesktopServices::openPath(m_server->serverDir(), true);
}

void ServerSettingsPage::editServerProperties()
{
    if (!m_server)
        return;
    const QString file = QDir(m_server->serverDir()).filePath("server.properties");
    if (!QFileInfo(file).isFile()) {
        QMessageBox::information(this, tr("No server.properties yet"),
                                 tr("This server has no server.properties yet. Minecraft creates it the first time the server starts."));
        return;
    }
    DesktopServices::openUrl(QUrl::fromLocalFile(file));
}
