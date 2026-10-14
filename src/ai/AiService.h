#pragma once

#include "AiTypes.h"
#include "AiToolDefs.h"
#include "AiToolExecutor.h"
#include "AiOpenAiCompat.h"
#include <QElapsedTimer>
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>

namespace OpenDaw {

class EditManager;
class AudioEngine;

class AiService : public QObject {
    Q_OBJECT
public:
    explicit AiService(EditManager* editMgr, AudioEngine* audioEngine,
                       PluginScanner* pluginScanner, QObject* parent = nullptr);

    void sendMessage(const QString& userText);
    void clearConversation();

    const QVector<AiMessage>& conversationHistory() const { return conversationHistory_; }
    void loadConversation(const QVector<AiMessage>& messages);

    void requestSessionTitle(const QString& userMessage);

    // Settings (stored in QSettings; edited from the Settings window)
    static QString apiKey();
    static void setApiKey(const QString& key);

    static QString model();
    static void setModel(const QString& model);

    static bool confirmDestructive();
    static void setConfirmDestructive(bool confirm);
    static bool previewMixPlanMode();
    static void setPreviewMixPlanMode(bool enabled);
    // "low", "medium", "high", "xhigh" or "max"; sent to models that take it
    static QString effort();
    static void setEffort(const QString& effort);

    // Provider: "anthropic" (Claude) or "local" (an OpenAI-compatible server
    // such as Ollama, LM Studio, llama.cpp server or Hugging Face TGI).
    static QString provider();
    static void setProvider(const QString& provider);
    static QString localBaseUrl();
    static void setLocalBaseUrl(const QString& url);
    static QString localModel();
    static void setLocalModel(const QString& model);
    static QString localApiKey();
    static void setLocalApiKey(const QString& key);
    static bool localUseTools();
    static void setLocalUseTools(bool enabled);
    // Context window, in tokens, a local model needs for OpenDaw's system
    // prompt, the conversation and, when enabled, the DAW tool definitions.
    static int recommendedLocalContext(bool withTools);

    bool isBusy() const { return busy_; }

signals:
    void tokenReceived(const QString& token);
    // Local reasoning models: thinking so far, in streamed chunks (about one
    // token each). Lets the chat show progress while no reply text arrives.
    void thinkingProgress(int chunks);
    void responseComplete(const AiMessage& message);
    void toolCallStarted(const QString& toolName, const QString& toolId);
    void toolCallFinished(const QString& toolName, const QString& toolId,
                          const QString& result, bool isError);
    void errorOccurred(const QString& error);
    void busyChanged(bool busy);
    void confirmDestructiveAction(const QString& toolName, const AiToolCall& call);
    void sessionTitleGenerated(const QString& title);

public slots:
    void executeConfirmedTool(const AiToolCall& call);
    void cancelPendingTool(const AiToolCall& call);

private:
    void sendToApi();
    void handleStreamChunk();
    void sendToLocalServer();
    void handleLocalStreamChunk();
    void handleStreamFinished();
    void processAssistantResponse();
    void executeToolCalls(const QVector<AiToolCall>& calls);
    void continueAfterToolResults(const QVector<AiToolResult>& results,
                                  const AiMessage& assistantMsg);
    QString buildSystemPrompt() const;
    static const char* const kSystemPromptRules;
    AiContentBlock buildProjectSnapshot() const;
    void applyModelOptions(QJsonObject& body, QNetworkRequest& request,
                           const QString& effortLevel, bool allowFallback) const;
    QJsonObject messageToJson(const AiMessage& msg) const;
    void setBusy(bool busy);

    EditManager* editMgr_;
    AudioEngine* audioEngine_;
    AiToolExecutor* toolExecutor_;
    QNetworkAccessManager* nam_;

    QVector<AiMessage> conversationHistory_;
    QByteArray streamBuffer_;
    QNetworkReply* currentReply_ = nullptr;

    QString currentAssistantText_;
    QVector<AiContentBlock> currentContentBlocks_;
    QJsonObject currentToolInput_;
    QString currentToolInputStr_;
    bool inToolInput_ = false;
    int currentBlockIndex_ = -1;
    QString currentStopReason_;
    bool localRequest_ = false;
    OpenAiCompat::StreamState localStream_;
    QElapsedTimer localTimer_;
    QString currentRefusalCategory_;

    bool busy_ = false;

    static constexpr int MAX_TOOL_ROUNDS = 20;
    int toolRoundCount_ = 0;

    QSet<QString> executedToolIds_;
};

} // namespace OpenDaw
