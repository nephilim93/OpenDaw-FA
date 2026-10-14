#include "SettingsDialog.h"
#include "AudioSettingsPage.h"
#include "ai/AiService.h"
#include "engine/AudioEngine.h"
#include "engine/EditManager.h"
#include "engine/PluginScanner.h"
#include "utils/AppPaths.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>

namespace OpenDaw {

namespace {

QString toQ(const juce::String& s) { return QString::fromUtf8(s.toRawUTF8()); }

QLabel* makeNote(const QString& text, QWidget* parent)
{
    auto* note = new QLabel(text, parent);
    note->setWordWrap(true);
    note->setStyleSheet("font-size: 10px; color: gray; padding: 4px 8px;");
    return note;
}

struct ModelChoice { const char* label; const char* id; };
const ModelChoice kModels[] = {
    { "Claude Opus 5.5 (recommended)",       "claude-opus-5-5" },
    { "Claude Sonnet 5.5 (faster)",          "claude-sonnet-5-5" },
    { "Claude Haiku 5.5 (fastest, cheapest)", "claude-haiku-5-5" },
    { "Claude Fable 5.1 (most capable)",     "claude-fable-5-1" },
    { "Claude Opus 4.8",                     "claude-opus-4-8" },
    { "Claude Sonnet 4.6",                   "claude-sonnet-4-6" },
    { "Claude Haiku 4.5",                    "claude-haiku-4-5" },
};

struct EffortChoice { const char* label; const char* id; };
const EffortChoice kEfforts[] = {
    { "Low (fastest)",  "low" },
    { "Medium",         "medium" },
    { "High",           "high" },
    { "Extra high",     "xhigh" },
    { "Max (slowest)",  "max" },
};

} // namespace

SettingsDialog::SettingsDialog(AudioEngine& audio, EditManager& editMgr,
                               PluginScanner& scanner,
                               std::function<void()> rescanPlugins, QWidget* parent)
    : QDialog(parent), audio_(audio), editMgr_(editMgr), scanner_(scanner),
      rescanPlugins_(std::move(rescanPlugins))
{
    setWindowTitle("Settings");
    setAccessibleName("Settings");
    resize(720, 480);

    auto* layout = new QVBoxLayout(this);
    auto* body = new QHBoxLayout();
    layout->addLayout(body, 1);

    pageList_ = new QListWidget(this);
    pageList_->setAccessibleName("Settings pages");
    pageList_->setFixedWidth(150);
    body->addWidget(pageList_);

    pages_ = new QStackedWidget(this);
    body->addWidget(pages_, 1);

    auto* audioPage = new AudioSettingsPage(audio_, this);
    audioPage->setDeviceChangedCallback([this]() { editMgr_.enableAllWaveInputDevices(); });
    addPage("Audio", audioPage);
    addPage("MIDI", createMidiPage());
    addPage("AI Assistant", createAiPage());
    addPage("Plugins", createPluginsPage());
    addPage("System", createSystemPage());

    connect(pageList_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
    pageList_->setCurrentRow(0);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    layout->addWidget(buttons);
}

void SettingsDialog::addPage(const QString& title, QWidget* page)
{
    pageList_->addItem(title);
    pages_->addWidget(page);
}

void SettingsDialog::showPage(Page page)
{
    pageList_->setCurrentRow(static_cast<int>(page));
}

// ── MIDI ────────────────────────────────────────────────────────────────────

QWidget* SettingsDialog::createMidiPage()
{
    auto* page = new QWidget(this);
    page->setAccessibleName("MIDI Settings");
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);

    auto& dm = audio_.deviceManager();

    auto* inputsGroup = new QGroupBox("MIDI Inputs", page);
    auto* inputsLayout = new QVBoxLayout(inputsGroup);
    auto* inputs = new QListWidget(inputsGroup);
    inputs->setAccessibleName("MIDI input devices");
    for (int i = 0; i < dm.getNumMidiInDevices(); ++i) {
        if (auto dev = dm.getMidiInDevice(i)) {
            auto* item = new QListWidgetItem(toQ(dev->getName()), inputs);
            item->setCheckState(dev->isEnabled() ? Qt::Checked : Qt::Unchecked);
        }
    }
    if (inputs->count() == 0)
        inputs->addItem("No MIDI inputs found");
    inputsLayout->addWidget(inputs);
    inputsLayout->addWidget(makeNote(
        "Enabled inputs can be chosen as a MIDI track's input. "
        "New devices are enabled automatically.", inputsGroup));
    layout->addWidget(inputsGroup);

    auto* outputsGroup = new QGroupBox("MIDI Outputs", page);
    auto* outputsLayout = new QVBoxLayout(outputsGroup);
    auto* outputs = new QListWidget(outputsGroup);
    outputs->setAccessibleName("MIDI output devices");
    for (int i = 0; i < dm.getNumMidiOutDevices(); ++i) {
        if (auto* dev = dm.getMidiOutDevice(i)) {
            auto* item = new QListWidgetItem(toQ(dev->getName()), outputs);
            item->setCheckState(dev->isEnabled() ? Qt::Checked : Qt::Unchecked);
        }
    }
    if (outputs->count() == 0)
        outputs->addItem("No MIDI outputs found");
    outputsLayout->addWidget(outputs);
    layout->addWidget(outputsGroup);

    // Inputs are stored as the ones switched off (so new devices start on);
    // outputs as the ones switched on.
    connect(inputs, &QListWidget::itemChanged, this, [this, inputs]() {
        QStringList disabled;
        for (int i = 0; i < inputs->count(); ++i)
            if (inputs->item(i)->checkState() == Qt::Unchecked)
                disabled << inputs->item(i)->text();
        QSettings().setValue("midi/disabledInputs", disabled);
        audio_.applyMidiDeviceSettings();
    });
    connect(outputs, &QListWidget::itemChanged, this, [this, outputs]() {
        QStringList enabled;
        for (int i = 0; i < outputs->count(); ++i)
            if (outputs->item(i)->checkState() == Qt::Checked)
                enabled << outputs->item(i)->text();
        QSettings().setValue("midi/enabledOutputs", enabled);
        audio_.applyMidiDeviceSettings();
    });

    return page;
}

// ── AI Assistant ────────────────────────────────────────────────────────────

QWidget* SettingsDialog::createAiPage()
{
    auto* page = new QWidget(this);
    page->setAccessibleName("AI Assistant Settings");
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* group = new QGroupBox("Claude", page);
    auto* form = new QFormLayout(group);

    auto* apiKeyEdit = new QLineEdit(AiService::apiKey(), group);
    apiKeyEdit->setAccessibleName("Anthropic API Key");
    apiKeyEdit->setEchoMode(QLineEdit::Password);
    apiKeyEdit->setPlaceholderText("sk-ant-...");
    auto* showKey = new QCheckBox("Show", group);
    auto* keyRow = new QHBoxLayout();
    keyRow->addWidget(apiKeyEdit, 1);
    keyRow->addWidget(showKey);
    form->addRow("API Key:", keyRow);
    connect(showKey, &QCheckBox::toggled, apiKeyEdit, [apiKeyEdit](bool show) {
        apiKeyEdit->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
    });
    connect(apiKeyEdit, &QLineEdit::editingFinished, this, [apiKeyEdit]() {
        AiService::setApiKey(apiKeyEdit->text().trimmed());
    });

    auto* modelCombo = new QComboBox(group);
    modelCombo->setAccessibleName("Model");
    for (const auto& m : kModels)
        modelCombo->addItem(m.label, QString(m.id));
    modelCombo->addItem("Custom model ID...", QString());
    form->addRow("Model:", modelCombo);

    auto* customModel = new QLineEdit(group);
    customModel->setAccessibleName("Custom model ID");
    customModel->setPlaceholderText("claude-...");
    form->addRow("", customModel);

    const QString current = AiService::model();
    const int known = modelCombo->findData(current);
    modelCombo->setCurrentIndex(known >= 0 ? known : modelCombo->count() - 1);
    customModel->setText(known >= 0 ? QString() : current);
    customModel->setVisible(known < 0);

    connect(modelCombo, &QComboBox::activated, this, [modelCombo, customModel]() {
        const QString id = modelCombo->currentData().toString();
        customModel->setVisible(id.isEmpty());
        if (!id.isEmpty())
            AiService::setModel(id);
        else
            customModel->setFocus();
    });
    connect(customModel, &QLineEdit::editingFinished, this, [customModel]() {
        if (!customModel->text().trimmed().isEmpty())
            AiService::setModel(customModel->text().trimmed());
    });

    auto* effortCombo = new QComboBox(group);
    effortCombo->setAccessibleName("Effort");
    effortCombo->setToolTip("How much the model thinks before acting. Higher is more "
                            "thorough but slower and uses more tokens. Older models "
                            "may ignore it or use the nearest level.");
    for (const auto& e : kEfforts)
        effortCombo->addItem(e.label, QString(e.id));
    effortCombo->setCurrentIndex(std::max(0, effortCombo->findData(AiService::effort())));
    form->addRow("Effort:", effortCombo);
    connect(effortCombo, &QComboBox::activated, this, [effortCombo]() {
        AiService::setEffort(effortCombo->currentData().toString());
    });

    auto* confirmCheck = new QCheckBox("Confirm destructive actions", group);
    confirmCheck->setAccessibleName("Confirm destructive actions");
    confirmCheck->setChecked(AiService::confirmDestructive());
    form->addRow("", confirmCheck);
    connect(confirmCheck, &QCheckBox::toggled, this, &AiService::setConfirmDestructive);

    auto* previewCheck = new QCheckBox("Preview mix/master plan before applying changes", group);
    previewCheck->setAccessibleName("Preview mix master plan");
    previewCheck->setChecked(AiService::previewMixPlanMode());
    form->addRow("", previewCheck);
    connect(previewCheck, &QCheckBox::toggled, this, &AiService::setPreviewMixPlanMode);

    layout->addWidget(group);
    layout->addWidget(makeNote(
        "Get an API key at console.anthropic.com. The key is stored in this "
        "computer's OpenDaw settings.", page));
    layout->addStretch();
    return page;
}

// ── Plugins ─────────────────────────────────────────────────────────────────

QWidget* SettingsDialog::createPluginsPage()
{
    auto* page = new QWidget(this);
    page->setAccessibleName("Plugin Settings");
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* foldersGroup = new QGroupBox("Plugin Folders", page);
    auto* foldersLayout = new QVBoxLayout(foldersGroup);
    foldersLayout->addWidget(makeNote(
        "Always scanned: " + toQ(scanner_.defaultFolders().joinIntoString(", ")),
        foldersGroup));

    auto* folders = new QListWidget(foldersGroup);
    folders->setAccessibleName("Extra plugin folders");
    folders->addItems(PluginScanner::extraFolders());
    foldersLayout->addWidget(folders);

    auto* folderButtons = new QHBoxLayout();
    auto* addBtn = new QPushButton("Add Folder...", foldersGroup);
    auto* removeBtn = new QPushButton("Remove", foldersGroup);
    folderButtons->addWidget(addBtn);
    folderButtons->addWidget(removeBtn);
    folderButtons->addStretch();
    foldersLayout->addLayout(folderButtons);
    layout->addWidget(foldersGroup);

    auto saveFolders = [folders]() {
        QStringList list;
        for (int i = 0; i < folders->count(); ++i)
            list << folders->item(i)->text();
        PluginScanner::setExtraFolders(list);
    };
    connect(addBtn, &QPushButton::clicked, this, [this, folders, saveFolders]() {
        const QString dir = QFileDialog::getExistingDirectory(this, "Add Plugin Folder");
        if (dir.isEmpty() || !folders->findItems(dir, Qt::MatchExactly).isEmpty()) return;
        folders->addItem(dir);
        saveFolders();
    });
    connect(removeBtn, &QPushButton::clicked, this, [folders, saveFolders]() {
        delete folders->currentItem();
        saveFolders();
    });

    auto* scanGroup = new QGroupBox("Scanning", page);
    auto* scanLayout = new QVBoxLayout(scanGroup);
    auto* status = new QLabel(scanGroup);
    status->setAccessibleName("Plugin scan status");
    scanLayout->addWidget(status);

    auto* scanButtons = new QHBoxLayout();
    auto* rescanBtn = new QPushButton("Scan Now", scanGroup);
    auto* clearBtn = new QPushButton("Clear Blacklist", scanGroup);
    clearBtn->setToolTip("Plugins that crashed during a scan are skipped. "
                         "Clear the list to try them again on the next scan.");
    scanButtons->addWidget(rescanBtn);
    scanButtons->addWidget(clearBtn);
    scanButtons->addStretch();
    scanLayout->addLayout(scanButtons);
    layout->addWidget(scanGroup);
    layout->addStretch();

    auto refresh = [this, status, rescanBtn, clearBtn]() {
        auto& list = scanner_.getPluginList();
        const int blacklisted = list.getBlacklistedFiles().size();
        status->setText(scanner_.isScanning()
            ? QString("Scanning...")
            : QString("%1 plugins found, %2 skipped after crashing during a scan.")
                  .arg(list.getNumTypes()).arg(blacklisted));
        rescanBtn->setEnabled(!scanner_.isScanning());
        clearBtn->setEnabled(blacklisted > 0);
    };
    refresh();

    connect(&scanner_, &PluginScanner::scanFinished, page, refresh);
    connect(rescanBtn, &QPushButton::clicked, this, [this, refresh]() {
        if (rescanPlugins_) rescanPlugins_();
        refresh();
    });
    connect(clearBtn, &QPushButton::clicked, this, [this, refresh]() {
        scanner_.clearBlacklist();
        refresh();
    });

    return page;
}

// ── System ──────────────────────────────────────────────────────────────────

QWidget* SettingsDialog::createSystemPage()
{
    auto* page = new QWidget(this);
    page->setAccessibleName("System Settings");
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* group = new QGroupBox("Projects", page);
    auto* form = new QFormLayout(group);

    auto* autosave = new QComboBox(group);
    autosave->setAccessibleName("Autosave interval");
    for (int minutes : { 0, 1, 2, 5, 10 })
        autosave->addItem(minutes == 0 ? QString("Off")
                                       : QString("Every %1 min").arg(minutes), minutes);
    autosave->setCurrentIndex(
        std::max(0, autosave->findData(QSettings().value("system/autosaveMinutes", 1).toInt())));
    form->addRow("Autosave:", autosave);
    connect(autosave, &QComboBox::activated, this, [this, autosave]() {
        QSettings().setValue("system/autosaveMinutes", autosave->currentData().toInt());
        editMgr_.startAutosave();
    });

    auto* projectDir = new QLineEdit(QSettings().value("paths/defaultProjectDir").toString(), group);
    projectDir->setAccessibleName("Default project folder");
    projectDir->setPlaceholderText("Home folder");
    auto* browse = new QPushButton("Browse...", group);
    auto* dirRow = new QHBoxLayout();
    dirRow->addWidget(projectDir, 1);
    dirRow->addWidget(browse);
    form->addRow("Project Folder:", dirRow);

    connect(projectDir, &QLineEdit::editingFinished, this, [projectDir]() {
        QSettings().setValue("paths/defaultProjectDir", projectDir->text().trimmed());
    });
    connect(browse, &QPushButton::clicked, this, [this, projectDir]() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, "Default Project Folder", projectDir->text());
        if (dir.isEmpty()) return;
        projectDir->setText(dir);
        QSettings().setValue("paths/defaultProjectDir", dir);
    });
    layout->addWidget(group);
    layout->addWidget(makeNote(
        "New projects are saved to the project folder unless you pick another one.", page));

    auto* logGroup = new QGroupBox("Diagnostics", page);
    auto* logLayout = new QHBoxLayout(logGroup);
    auto* logPath = new QLabel(QDir::toNativeSeparators(logDirectory()), logGroup);
    logPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* openLogs = new QPushButton("Open Log Folder", logGroup);
    logLayout->addWidget(new QLabel("Log:", logGroup));
    logLayout->addWidget(logPath, 1);
    logLayout->addWidget(openLogs);
    connect(openLogs, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(logDirectory()));
    });
    layout->addWidget(logGroup);
    layout->addStretch();
    return page;
}

} // namespace OpenDaw
