#include "SettingsDialog.h"
#include "AudioSettingsPage.h"
#include "ai/AiService.h"
#include "drums/SampleLibrary.h"
#include "engine/AudioEngine.h"
#include "engine/EditManager.h"
#include "engine/PluginScanner.h"
#include "engine/drums/DrumMachinePlugin.h"
#include "engine/midiports/VirtualMidiPorts.h"
#include "ui/drums/DrumsViewLink.h"
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>
#include <memory>

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

// Common local servers and their default OpenAI-compatible endpoints.
struct ServerPreset { const char* label; const char* url; };
const ServerPreset kServerPresets[] = {
    { "Ollama",                 "http://localhost:11434/v1" },
    { "LM Studio",              "http://localhost:1234/v1" },
    { "llama.cpp server",       "http://localhost:8080/v1" },
    { "Hugging Face TGI",       "http://localhost:8080/v1" },
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

    // Pages keep their natural height (wrapped labels included) and scroll
    // vertically when they are taller than the dialog.
    auto* scroll = new QScrollArea(pages_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(page);
    pages_->addWidget(scroll);
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
            if (VirtualMidiPorts::isOwnPortSeenAsDevice(dev->getName(), true)) continue;
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
            if (VirtualMidiPorts::isOwnPortSeenAsDevice(dev->getName(), false)) continue;
            auto* item = new QListWidgetItem(toQ(dev->getName()), outputs);
            item->setCheckState(dev->isEnabled() ? Qt::Checked : Qt::Unchecked);
        }
    }
    if (outputs->count() == 0)
        outputs->addItem("No MIDI outputs found");
    outputsLayout->addWidget(outputs);
    layout->addWidget(outputsGroup);

    // OpenDaw's own ports, for apps on this computer (VCV Rack, midimux, ...).
    auto* virtualGroup = new QGroupBox("Virtual Ports", page);
    auto* virtualLayout = new QVBoxLayout(virtualGroup);
    if (VirtualMidiPorts::supported()) {
        auto* publish = new QCheckBox("Publish \"OpenDaw In\" and \"OpenDaw Out\" for other apps", virtualGroup);
        publish->setAccessibleName("Virtual MIDI ports");
        publish->setChecked(audio_.virtualMidiPorts().isEnabled());
        connect(publish, &QCheckBox::toggled, this, [this](bool on) {
            QSettings().setValue(VirtualMidiPorts::kEnabledKey, on);
            audio_.virtualMidiPorts().setEnabled(on);
        });
        virtualLayout->addWidget(publish);
        auto* clock = new QCheckBox("Send MIDI clock and start/stop on \"OpenDaw Out\"", virtualGroup);
        clock->setAccessibleName("Virtual MIDI clock");
        clock->setChecked(audio_.virtualMidiPorts().clockEnabled());
        clock->setToolTip("Other apps (VCV Rack's MIDI > CV clock, ...) follow OpenDaw's tempo and transport");
        connect(clock, &QCheckBox::toggled, this, [this](bool on) { audio_.virtualMidiPorts().setClockEnabled(on); });
        virtualLayout->addWidget(clock);
        virtualLayout->addWidget(makeNote(
            "Other apps send to \"OpenDaw In\": choose it as a MIDI track's input. To play another "
            "app from OpenDaw, give a track the MIDI Out instrument and choose \"OpenDaw Out\" "
            "as the input in that app.", virtualGroup));
    } else {
        virtualLayout->addWidget(makeNote(
            "Windows has no virtual MIDI ports for apps. Install loopMIDI (free, from "
            "tobias-erichsen.de) and create a port: it appears here like any device. Choose it as a "
            "track's input to receive from another app, or as a MIDI Out track's destination to "
            "send to one.", virtualGroup));
    }
    layout->addWidget(virtualGroup);

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

    auto* providerRow = new QFormLayout();
    auto* providerCombo = new QComboBox(page);
    providerCombo->setAccessibleName("AI provider");
    providerCombo->addItem("Claude (Anthropic API)", QString("anthropic"));
    providerCombo->addItem("Local server (Ollama, LM Studio, ...)", QString("local"));
    providerCombo->setCurrentIndex(std::max(0, providerCombo->findData(AiService::provider())));
    providerRow->addRow("Provider:", providerCombo);
    layout->addLayout(providerRow);

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

    form->addRow(makeNote(
        "Get an API key at console.anthropic.com. The key is stored in this "
        "computer's OpenDaw settings.", group));
    layout->addWidget(group);

    // ── Local server ──
    auto* localGroup = new QGroupBox("Local Server", page);
    auto* localForm = new QFormLayout(localGroup);

    auto* presetCombo = new QComboBox(localGroup);
    presetCombo->setAccessibleName("Local server preset");
    for (const auto& preset : kServerPresets)
        presetCombo->addItem(preset.label, QString(preset.url));
    presetCombo->addItem("Custom", QString());
    localForm->addRow("Server:", presetCombo);

    auto* urlEdit = new QLineEdit(AiService::localBaseUrl(), localGroup);
    urlEdit->setAccessibleName("Local server address");
    urlEdit->setPlaceholderText("http://localhost:11434/v1");
    localForm->addRow("Address:", urlEdit);
    const int presetIdx = presetCombo->findData(AiService::localBaseUrl());
    presetCombo->setCurrentIndex(presetIdx >= 0 ? presetIdx : presetCombo->count() - 1);

    auto* localModel = new QComboBox(localGroup);
    localModel->setAccessibleName("Local model");
    localModel->setEditable(true);
    localModel->setCurrentText(AiService::localModel());
    localModel->lineEdit()->setPlaceholderText("e.g. qwen3:8b");
    auto* refreshBtn = new QPushButton("Refresh", localGroup);
    refreshBtn->setToolTip("List the models installed on the server");
    auto* modelRow = new QHBoxLayout();
    modelRow->addWidget(localModel, 1);
    modelRow->addWidget(refreshBtn);
    localForm->addRow("Model:", modelRow);

    auto* localKey = new QLineEdit(AiService::localApiKey(), localGroup);
    localKey->setAccessibleName("Local server API key");
    localKey->setEchoMode(QLineEdit::Password);
    localKey->setPlaceholderText("Usually not needed");
    localForm->addRow("API Key:", localKey);

    auto* toolsCheck = new QCheckBox("Let the model edit the project (DAW tools)", localGroup);
    toolsCheck->setAccessibleName("Use DAW tools with local models");
    toolsCheck->setChecked(AiService::localUseTools());
    toolsCheck->setToolTip("Turn off for models without tool-calling support; "
                           "the assistant is then chat-only.");
    localForm->addRow("", toolsCheck);

    auto* thinkingCombo = new QComboBox(localGroup);
    thinkingCombo->setAccessibleName("Local model thinking");
    thinkingCombo->addItem("Off (fastest)", QString("none"));
    thinkingCombo->addItem("Low", QString("low"));
    thinkingCombo->addItem("Medium", QString("medium"));
    thinkingCombo->addItem("High", QString("high"));
    thinkingCombo->addItem("Model default", QString());
    thinkingCombo->setToolTip("How much reasoning models (gemma4, qwen3, ...) think before "
                              "answering. More thinking can plan better but takes minutes "
                              "per step on a local GPU. Models without thinking ignore this.");
    thinkingCombo->setCurrentIndex(qMax(0, thinkingCombo->findData(AiService::localReasoningEffort())));
    localForm->addRow("Thinking:", thinkingCombo);
    connect(thinkingCombo, &QComboBox::activated, this, [thinkingCombo]() {
        AiService::setLocalReasoningEffort(thinkingCombo->currentData().toString());
    });

    auto* localStatus = new QLabel(localGroup);
    localStatus->setAccessibleName("Local server status");
    localStatus->setWordWrap(true);
    localForm->addRow("", localStatus);

    auto* contextWarning = new QLabel(localGroup);
    contextWarning->setAccessibleName("Local model context warning");
    contextWarning->setWordWrap(true);
    contextWarning->setTextInteractionFlags(Qt::TextSelectableByMouse);
    contextWarning->hide();
    localForm->addRow("", contextWarning);
    localForm->addRow(makeNote(
        "Works with any server that speaks the OpenAI chat API. Hugging Face "
        "models can be run through these, for example "
        "\"ollama pull hf.co/<user>/<repo>\" for GGUF models. Small models may "
        "handle the DAW tools poorly; larger models with tool-calling support "
        "work best.", localGroup));
    layout->addWidget(localGroup);

    connect(presetCombo, &QComboBox::activated, this, [presetCombo, urlEdit]() {
        const QString url = presetCombo->currentData().toString();
        if (url.isEmpty()) {
            urlEdit->setFocus();
            return;
        }
        urlEdit->setText(url);
        AiService::setLocalBaseUrl(url);
    });
    connect(urlEdit, &QLineEdit::editingFinished, this, [presetCombo, urlEdit]() {
        AiService::setLocalBaseUrl(urlEdit->text().trimmed());
        const int idx = presetCombo->findData(urlEdit->text().trimmed());
        presetCombo->setCurrentIndex(idx >= 0 ? idx : presetCombo->count() - 1);
    });
    connect(localModel, &QComboBox::currentTextChanged, this, [](const QString& text) {
        AiService::setLocalModel(text.trimmed());
    });
    connect(localKey, &QLineEdit::editingFinished, this, [localKey]() {
        AiService::setLocalApiKey(localKey->text().trimmed());
    });

    auto* nam = new QNetworkAccessManager(page);

    // Ollama serves the OpenAI API under /v1 and reports each loaded model's
    // context window at /api/ps. A prompt longer than that window is cut
    // silently, and the model loses tools and instructions. Other servers,
    // and models that are not loaded yet, are skipped without a warning.
    // Several checks can be in flight (quick Refresh clicks); only the
    // latest one may update the label.
    auto latestCheck = std::make_shared<int>(0);
    auto checkContext = [nam, urlEdit, localModel, toolsCheck, contextWarning, latestCheck]() {
        QString base = urlEdit->text().trimmed();
        while (base.endsWith('/')) base.chop(1);
        if (base.endsWith("/v1")) base.chop(3);
        const QString model = localModel->currentText().trimmed();
        const int check = ++*latestCheck;
        if (model.isEmpty()) {
            contextWarning->hide();
            return;
        }

        QNetworkRequest request{QUrl(base + "/api/ps")};
        request.setTransferTimeout(5000);
        auto* reply = nam->get(request);
        QObject::connect(reply, &QNetworkReply::finished, contextWarning,
                         [reply, model, toolsCheck, contextWarning, latestCheck, check]() {
            reply->deleteLater();
            if (check != *latestCheck)
                return;
            // Not Ollama, or unreachable: nothing to check.
            if (reply->error() != QNetworkReply::NoError) {
                contextWarning->hide();
                return;
            }

            const int recommended = AiService::recommendedLocalContext(toolsCheck->isChecked());
            for (const auto& val : QJsonDocument::fromJson(reply->readAll())
                                       .object()["models"].toArray()) {
                const auto entry = val.toObject();
                if (entry["name"].toString() != model && entry["model"].toString() != model)
                    continue;

                const int context = entry["context_length"].toInt();
                if (context <= 0 || context >= recommended) {
                    contextWarning->hide();
                    return;
                }

                contextWarning->setStyleSheet("color: #e0a030;");
                contextWarning->setText(QString(
                    "%1 is loaded with a %2-token context, but OpenDaw needs about %3. "
                    "Longer prompts are cut off, so the model may ignore tools or "
                    "instructions. Restart Ollama with OLLAMA_CONTEXT_LENGTH=%3 "
                    "(on Linux: sudo systemctl edit ollama).")
                    .arg(model).arg(context).arg(recommended));
                contextWarning->show();
                return;
            }

            // Ollama only reports the context of loaded models.
            contextWarning->setStyleSheet("color: gray;");
            contextWarning->setText(QString(
                "%1 is not loaded yet, so its context size can't be checked. "
                "OpenDaw needs about %2 tokens; click Refresh after the first "
                "message to check.").arg(model).arg(recommended));
            contextWarning->show();
        });
    };

    connect(localModel, &QComboBox::activated, this, checkContext);
    connect(toolsCheck, &QCheckBox::toggled, this, [checkContext](bool enabled) {
        AiService::setLocalUseTools(enabled);
        checkContext();
    });

    // Lists the server's models via GET <address>/models.
    connect(refreshBtn, &QPushButton::clicked, this,
            [nam, urlEdit, localKey, localModel, localStatus, checkContext]() {
        QString base = urlEdit->text().trimmed();
        while (base.endsWith('/')) base.chop(1);
        QNetworkRequest request{QUrl(base + "/models")};
        if (!localKey->text().trimmed().isEmpty())
            request.setRawHeader("Authorization", ("Bearer " + localKey->text().trimmed()).toUtf8());
        request.setTransferTimeout(5000);
        localStatus->setText("Connecting...");

        auto* reply = nam->get(request);
        QObject::connect(reply, &QNetworkReply::finished, localStatus,
                         [reply, localModel, localStatus, base, checkContext]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                localStatus->setText(QString("Could not reach %1 (%2). Is the server running?")
                                         .arg(base, reply->errorString()));
                return;
            }
            QStringList models;
            for (const auto& val : QJsonDocument::fromJson(reply->readAll())
                                       .object()["data"].toArray())
                models << val.toObject()["id"].toString();
            models.removeAll(QString());
            models.sort(Qt::CaseInsensitive);

            const QString current = localModel->currentText();
            QSignalBlocker block(localModel);
            localModel->clear();
            localModel->addItems(models);
            localModel->setCurrentText(current.isEmpty() && !models.isEmpty() ? models.first()
                                                                             : current);
            AiService::setLocalModel(localModel->currentText().trimmed());
            localStatus->setText(models.isEmpty()
                ? QString("Connected, but the server reports no models.")
                : QString("Connected: %1 model(s) available.").arg(models.size()));
            checkContext();
        });
    });

    // ── Behaviour (both providers) ──
    auto* behaviourGroup = new QGroupBox("Behaviour", page);
    auto* behaviourForm = new QFormLayout(behaviourGroup);

    auto* confirmCheck = new QCheckBox("Confirm destructive actions", behaviourGroup);
    confirmCheck->setAccessibleName("Confirm destructive actions");
    confirmCheck->setChecked(AiService::confirmDestructive());
    behaviourForm->addRow("", confirmCheck);
    connect(confirmCheck, &QCheckBox::toggled, this, &AiService::setConfirmDestructive);

    auto* previewCheck = new QCheckBox("Preview mix/master plan before applying changes", behaviourGroup);
    previewCheck->setAccessibleName("Preview mix master plan");
    previewCheck->setChecked(AiService::previewMixPlanMode());
    behaviourForm->addRow("", previewCheck);
    connect(previewCheck, &QCheckBox::toggled, this, &AiService::setPreviewMixPlanMode);
    layout->addWidget(behaviourGroup);
    layout->addStretch();

    auto showProvider = [group, localGroup, providerCombo, refreshBtn]() {
        const bool local = providerCombo->currentData().toString() == "local";
        group->setVisible(!local);
        localGroup->setVisible(local);
        if (local) refreshBtn->click();
    };
    connect(providerCombo, &QComboBox::activated, this, [providerCombo, showProvider]() {
        AiService::setProvider(providerCombo->currentData().toString());
        showProvider();
    });
    showProvider();

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

    auto* samplesGroup = new QGroupBox("Samples", page);
    auto* samplesForm = new QFormLayout(samplesGroup);
    auto* samplesDir = new QLineEdit(Drums::samplesFolder(), samplesGroup);
    samplesDir->setAccessibleName("Samples folder");
    samplesDir->setPlaceholderText(Drums::defaultSamplesFolder());
    auto* samplesBrowse = new QPushButton("Browse...", samplesGroup);
    auto* samplesRow = new QHBoxLayout();
    samplesRow->addWidget(samplesDir, 1);
    samplesRow->addWidget(samplesBrowse);
    samplesForm->addRow("Samples Folder:", samplesRow);

    // Pads keep paths relative to this folder, so every Drum Machine reads
    // its files again from the new one.
    auto apply = [this](const QString& dir) {
        if (QDir::cleanPath(dir.trimmed()) == QDir::cleanPath(Drums::samplesFolder())) return;
        Drums::setSamplesFolder(dir);
        if (auto* edit = editMgr_.edit())
            for (auto* plugin : te::getAllPlugins(*edit, false))
                if (auto* drums = dynamic_cast<DrumMachinePlugin*>(plugin))
                    drums->reloadSamples();
        DrumsViewLink::instance().notifySamplesFolderChanged();
    };
    connect(samplesDir, &QLineEdit::editingFinished, this, [samplesDir, apply]() {
        apply(samplesDir->text());
    });
    connect(samplesBrowse, &QPushButton::clicked, this, [this, samplesDir, apply]() {
        const QString dir = QFileDialog::getExistingDirectory(this, "Samples Folder", samplesDir->text());
        if (dir.isEmpty()) return;
        samplesDir->setText(dir);
        apply(dir);
    });
    layout->addWidget(samplesGroup);
    layout->addWidget(makeNote(
        "Drag samples from this folder onto Drum Machine pads (the Drums tab lists it). "
        "Kits store these samples relative to the folder, so they keep working when you "
        "move the folder or open the project on another computer.", page));

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
