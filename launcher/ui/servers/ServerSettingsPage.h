// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QPointer>
#include <QWidget>

class ServerInstance;
class QCheckBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

/// Settings of one server. Every edit is saved immediately to the server's server.cfg.
class ServerSettingsPage : public QWidget {
    Q_OBJECT
   public:
    explicit ServerSettingsPage(QWidget* parent = nullptr);
    ~ServerSettingsPage() override = default;

    void setServer(ServerInstance* server);
    /// re-reads everything from the server (e.g. after it was re-detected elsewhere)
    void reload();

   protected:
    void showEvent(QShowEvent* event) override;

   private:
    void updateState();
    void updateProfile();
    void saveName();
    void saveJavaPath();
    void saveMemory();
    void saveJvmArgs();
    void browseJava();
    void detectAgain();
    void chooseJar();
    void openServerFolder();
    void editServerProperties();

   private:
    QPointer<ServerInstance> m_server;
    QList<QMetaObject::Connection> m_connections;
    bool m_loading = false;

    QLabel* m_runningNote = nullptr;

    QLineEdit* m_nameEdit = nullptr;
    QCheckBox* m_autoRestartCheck = nullptr;

    QGroupBox* m_javaGroup = nullptr;
    QLineEdit* m_javaPathEdit = nullptr;
    QSpinBox* m_maxMemSpin = nullptr;
    QSpinBox* m_minMemSpin = nullptr;
    QLabel* m_memNote = nullptr;
    QLineEdit* m_jvmArgsEdit = nullptr;

    QGroupBox* m_profileGroup = nullptr;
    QLabel* m_profileWarning = nullptr;
    QLabel* m_kindValue = nullptr;
    QLabel* m_targetValue = nullptr;
    QLabel* m_detectedFromValue = nullptr;
    QLabel* m_jvmArgsValue = nullptr;
    QLabel* m_programArgsValue = nullptr;
    QLabel* m_javaVersionValue = nullptr;
    QPushButton* m_detectButton = nullptr;
    QPushButton* m_chooseJarButton = nullptr;

    QPushButton* m_openFolderButton = nullptr;
    QPushButton* m_editPropertiesButton = nullptr;
};
