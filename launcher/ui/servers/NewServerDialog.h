// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDialog>

#include "server/ServerInstallTask.h"

class ServerInstance;
class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

/// Creates a new server from a server pack zip, a download link to one, or a copy of an existing server folder.
class NewServerDialog : public QDialog {
    Q_OBJECT
   public:
    explicit NewServerDialog(QWidget* parent = nullptr);
    ~NewServerDialog() override = default;

    /// the installed server (valid after the dialog was accepted)
    ServerInstance* createdServer() const { return m_created; }

   public slots:
    void accept() override;
    void reject() override;

   private:
    ServerInstallTask::SourceKind sourceKind() const;
    QString sourceText() const;
    QString suggestedName() const;
    void sourceChanged();
    void updateState();
    void browseZip();
    void browseFolder();
    void saveGeometrySetting();

   private:
    QLineEdit* m_nameEdit = nullptr;
    bool m_nameEdited = false;  // the user typed a name: stop suggesting one

    QRadioButton* m_zipRadio = nullptr;
    QLineEdit* m_zipEdit = nullptr;
    QPushButton* m_zipBrowse = nullptr;
    QRadioButton* m_urlRadio = nullptr;
    QLineEdit* m_urlEdit = nullptr;
    QRadioButton* m_folderRadio = nullptr;
    QLineEdit* m_folderEdit = nullptr;
    QPushButton* m_folderBrowse = nullptr;

    QLabel* m_problemLabel = nullptr;
    QCheckBox* m_eulaCheck = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
    QPushButton* m_createButton = nullptr;

    ServerInstance* m_created = nullptr;
};
