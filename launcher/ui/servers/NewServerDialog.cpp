// SPDX-License-Identifier: GPL-3.0-only
#include "NewServerDialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>

#include "Application.h"
#include "BuildConfig.h"
#include "server/ServerInstance.h"
#include "server/ServerList.h"
#include "settings/SettingsObject.h"
#include "ui/dialogs/CustomMessageBox.h"
#include "ui/dialogs/ProgressDialog.h"
#include "ui/servers/ServerUiCommon.h"

namespace {
const char* geometrySetting = "NewServerDialogGeometry";

bool isHttpUrl(const QString& text)
{
    const QUrl url(text.trimmed(), QUrl::StrictMode);
    return url.isValid() && (url.scheme() == "http" || url.scheme() == "https") && !url.host().isEmpty();
}
}  // namespace

NewServerDialog::NewServerDialog(QWidget* parent) : QDialog(parent)
{
    setObjectName("newServerDialog");
    setWindowTitle(tr("New server"));
    setWindowModality(Qt::WindowModal);
    setMinimumWidth(720);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(16);

    auto* title = new QLabel(tr("Create a dedicated server"), this);
    title->setObjectName("serverDialogTitle");
    layout->addWidget(title);

    auto* intro = new QLabel(tr("Elytra copies the server into its own servers folder and works out how to start it. "
                                "Ports and networking are left to you."),
                             this);
    intro->setObjectName("serverHintLabel");
    intro->setWordWrap(true);
    layout->addWidget(intro);

    // source
    {
        auto* group = new QGroupBox(tr("Install from"), this);
        auto* grid = new QVBoxLayout(group);
        grid->setSpacing(8);
        auto* buttonGroup = new QButtonGroup(this);

        m_zipRadio = new QRadioButton(tr("Server pack (.zip file)"), group);
        buttonGroup->addButton(m_zipRadio);
        grid->addWidget(m_zipRadio);
        {
            auto* row = new QHBoxLayout();
            row->setContentsMargins(28, 0, 0, 6);
            m_zipEdit = new QLineEdit(group);
            m_zipEdit->setPlaceholderText(tr("e.g. GT_New_Horizons_2.7.0_Server_Java_17-21.zip"));
            row->addWidget(m_zipEdit, 1);
            m_zipBrowse = new QPushButton(tr("Browse..."), group);
            row->addWidget(m_zipBrowse);
            grid->addLayout(row);
        }

        m_urlRadio = new QRadioButton(tr("Download link to a server pack zip"), group);
        buttonGroup->addButton(m_urlRadio);
        grid->addWidget(m_urlRadio);
        {
            auto* row = new QHBoxLayout();
            row->setContentsMargins(28, 0, 0, 6);
            m_urlEdit = new QLineEdit(group);
            m_urlEdit->setPlaceholderText(tr("https://..."));
            row->addWidget(m_urlEdit, 1);
            grid->addLayout(row);
        }

        m_folderRadio = new QRadioButton(tr("Existing server folder (copied, the original is left untouched)"), group);
        buttonGroup->addButton(m_folderRadio);
        grid->addWidget(m_folderRadio);
        {
            auto* row = new QHBoxLayout();
            row->setContentsMargins(28, 0, 0, 0);
            m_folderEdit = new QLineEdit(group);
            m_folderEdit->setPlaceholderText(tr("Folder containing the server (mods, config, server jar...)"));
            row->addWidget(m_folderEdit, 1);
            m_folderBrowse = new QPushButton(tr("Browse..."), group);
            row->addWidget(m_folderBrowse);
            grid->addLayout(row);
        }

        m_zipRadio->setChecked(true);
        layout->addWidget(group);

        connect(buttonGroup, &QButtonGroup::buttonToggled, this, [this](QAbstractButton*, bool checked) {
            if (checked)
                sourceChanged();
        });
        connect(m_zipEdit, &QLineEdit::textChanged, this, &NewServerDialog::sourceChanged);
        connect(m_urlEdit, &QLineEdit::textChanged, this, &NewServerDialog::sourceChanged);
        connect(m_folderEdit, &QLineEdit::textChanged, this, &NewServerDialog::sourceChanged);
        connect(m_zipBrowse, &QPushButton::clicked, this, &NewServerDialog::browseZip);
        connect(m_folderBrowse, &QPushButton::clicked, this, &NewServerDialog::browseFolder);
    }

    // name
    {
        auto* form = new QFormLayout();
        form->setSpacing(10);
        m_nameEdit = new QLineEdit(this);
        m_nameEdit->setPlaceholderText(tr("My server"));
        connect(m_nameEdit, &QLineEdit::textEdited, this, [this] {
            m_nameEdited = !m_nameEdit->text().trimmed().isEmpty();
            updateState();
        });
        form->addRow(tr("Name:"), m_nameEdit);
        layout->addLayout(form);
    }

    m_problemLabel = new QLabel(this);
    m_problemLabel->setObjectName("serverHintLabel");
    m_problemLabel->setWordWrap(true);
    layout->addWidget(m_problemLabel);

    // EULA
    {
        auto* row = new QHBoxLayout();
        row->setSpacing(6);
        m_eulaCheck = new QCheckBox(tr("I accept the Minecraft EULA"), this);
        m_eulaCheck->setObjectName("serverEulaCheck");
        connect(m_eulaCheck, &QCheckBox::toggled, this, &NewServerDialog::updateState);
        row->addWidget(m_eulaCheck);
        auto* link = new QLabel(QString("(<a href=\"https://aka.ms/MinecraftEULA\">%1</a>)").arg(tr("read the EULA")), this);
        link->setOpenExternalLinks(true);
        link->setTextInteractionFlags(Qt::TextBrowserInteraction);
        row->addWidget(link);
        row->addStretch(1);
        layout->addLayout(row);
    }

    layout->addStretch(1);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_createButton = m_buttons->addButton(tr("Create"), QDialogButtonBox::AcceptRole);
    m_createButton->setObjectName("serverCreateButton");
    m_createButton->setDefault(true);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &NewServerDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &NewServerDialog::reject);
    layout->addWidget(m_buttons);

    ServerUi::ensureLauncherSetting(geometrySetting);
    const QByteArray geometry = QByteArray::fromBase64(APPLICATION->settings()->get(geometrySetting).toString().toUtf8());
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    else
        resize(820, 560);

    sourceChanged();
}

ServerInstallTask::SourceKind NewServerDialog::sourceKind() const
{
    if (m_urlRadio->isChecked())
        return ServerInstallTask::SourceKind::Url;
    if (m_folderRadio->isChecked())
        return ServerInstallTask::SourceKind::Folder;
    return ServerInstallTask::SourceKind::ZipFile;
}

QString NewServerDialog::sourceText() const
{
    switch (sourceKind()) {
        case ServerInstallTask::SourceKind::Url:
            return m_urlEdit->text().trimmed();
        case ServerInstallTask::SourceKind::Folder:
            return QDir::fromNativeSeparators(m_folderEdit->text().trimmed());
        case ServerInstallTask::SourceKind::ZipFile:
            break;
    }
    return QDir::fromNativeSeparators(m_zipEdit->text().trimmed());
}

QString NewServerDialog::suggestedName() const
{
    const QString source = sourceText();
    if (source.isEmpty())
        return {};
    QString name;
    switch (sourceKind()) {
        case ServerInstallTask::SourceKind::ZipFile:
            name = QFileInfo(source).completeBaseName();
            break;
        case ServerInstallTask::SourceKind::Folder:
            name = QFileInfo(source).fileName();
            break;
        case ServerInstallTask::SourceKind::Url: {
            const QUrl url(source);
            name = QFileInfo(url.path()).completeBaseName();
            break;
        }
    }
    name.replace('_', ' ');
    return name.simplified();
}

void NewServerDialog::sourceChanged()
{
    m_zipEdit->setEnabled(m_zipRadio->isChecked());
    m_zipBrowse->setEnabled(m_zipRadio->isChecked());
    m_urlEdit->setEnabled(m_urlRadio->isChecked());
    m_folderEdit->setEnabled(m_folderRadio->isChecked());
    m_folderBrowse->setEnabled(m_folderRadio->isChecked());

    if (!m_nameEdited)
        m_nameEdit->setText(suggestedName());
    updateState();
}

void NewServerDialog::updateState()
{
    const QString source = sourceText();
    QString problem;
    if (source.isEmpty()) {
        switch (sourceKind()) {
            case ServerInstallTask::SourceKind::ZipFile:
                problem = tr("Choose the server pack zip.");
                break;
            case ServerInstallTask::SourceKind::Url:
                problem = tr("Paste a download link to the server pack zip.");
                break;
            case ServerInstallTask::SourceKind::Folder:
                problem = tr("Choose the server folder to copy.");
                break;
        }
    } else {
        switch (sourceKind()) {
            case ServerInstallTask::SourceKind::ZipFile: {
                const QFileInfo info(source);
                if (!info.isFile())
                    problem = tr("That file doesn't exist.");
                else if (info.suffix().compare("zip", Qt::CaseInsensitive) != 0)
                    problem = tr("Only .zip server packs are supported.");
                break;
            }
            case ServerInstallTask::SourceKind::Url:
                if (!isHttpUrl(source))
                    problem = tr("That doesn't look like an http(s) link.");
                break;
            case ServerInstallTask::SourceKind::Folder:
                if (!QFileInfo(source).isDir())
                    problem = tr("That folder doesn't exist.");
                break;
        }
    }
    if (problem.isEmpty() && m_nameEdit->text().trimmed().isEmpty())
        problem = tr("Give the server a name.");
    if (problem.isEmpty() && !m_eulaCheck->isChecked())
        problem = tr("To run a Minecraft server you have to accept the Minecraft EULA.");

    m_problemLabel->setText(problem);
    m_problemLabel->setVisible(!problem.isEmpty());
    m_createButton->setEnabled(problem.isEmpty());
}

void NewServerDialog::browseZip()
{
    QString start = QFileInfo(m_zipEdit->text()).absolutePath();
    if (m_zipEdit->text().isEmpty())
        start = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose the server pack"), start, tr("Zip files (*.zip)"));
    if (!path.isEmpty())
        m_zipEdit->setText(QDir::toNativeSeparators(path));
}

void NewServerDialog::browseFolder()
{
    QString start = m_folderEdit->text();
    if (start.isEmpty())
        start = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    const QString path = QFileDialog::getExistingDirectory(this, tr("Choose the server folder to copy"), start);
    if (!path.isEmpty())
        m_folderEdit->setText(QDir::toNativeSeparators(path));
}

void NewServerDialog::saveGeometrySetting()
{
    APPLICATION->settings()->set(geometrySetting, QString::fromUtf8(saveGeometry().toBase64()));
}

void NewServerDialog::accept()
{
    updateState();
    if (!m_createButton->isEnabled())
        return;

    auto task = std::make_unique<ServerInstallTask>(APPLICATION->servers(), m_nameEdit->text().trimmed(), sourceKind(), sourceText());
    {
        ProgressDialog progress(this);
        progress.setSkipButton(true, tr("Abort"));
        progress.execWithTask(task.get());
    }

    if (!task->wasSuccessful() || !task->result()) {
        if (task->getState() == Task::State::AbortedByUser)
            return;
        QString reason = task->failReason();
        if (reason.isEmpty())
            reason = tr("Unknown error.");
        CustomMessageBox::selectable(this, tr("Could not create the server"), reason, QMessageBox::Critical)->exec();
        return;
    }

    m_created = task->result();
    m_created->acceptEula();
    saveGeometrySetting();
    QDialog::accept();
}

void NewServerDialog::reject()
{
    saveGeometrySetting();
    QDialog::reject();
}
