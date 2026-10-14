#include "AiService.h"
#include "engine/EditManager.h"
#include "engine/AudioEngine.h"
#include <QDebug>
#include <QNetworkRequest>
#include <QUrl>

namespace OpenDaw {

static const QString kApiUrl = "https://api.anthropic.com/v1/messages";
static const QString kDefaultModel = "claude-opus-5-5";
// Thinking counts toward max_tokens, so leave room for it as well as the reply.
static const int kMaxTokens = 32000;
// Upper bound for one local reply, thinking included. Local servers have no
// limit by default, so a model that keeps reasoning holds the GPU until it
// stops on its own.
static const int kMaxLocalTokens = 16384;

namespace {

// What each model family accepts. Unknown (custom) IDs get no optional
// parameters, which every model accepts.
struct ModelTraits {
    bool effort = false;          // output_config.effort
    bool xhighEffort = false;     // the "xhigh" level (Opus 4.7 and later)
    bool fallbackDefault = false; // fallbacks: "default" (Claude API)
    bool preservedThinking = false; // thinking blocks bound to the conversation
};

ModelTraits traitsFor(const QString& model)
{
    ModelTraits t;
    auto is = [&](const char* prefix) { return model.startsWith(prefix); };

    if (is("claude-fable-5-1") || is("claude-opus-5-5") || is("claude-sonnet-5-5")) {
        t = { true, true, true, true };
    } else if (is("claude-haiku-5-5")) {
        t = { true, true, false, true };  // no server-side fallback for Haiku
    } else if (is("claude-fable-5") || is("claude-opus-5") || is("claude-sonnet-5")) {
        t = { true, true, is("claude-sonnet-5") ? false : true, false };
    } else if (is("claude-opus-4-8") || is("claude-opus-4-7")) {
        t = { true, true, false, false };
    } else if (is("claude-opus-4-6") || is("claude-sonnet-4-6")) {
        t = { true, false, false, false };
    }
    return t;
}

} // namespace

AiService::AiService(EditManager* editMgr, AudioEngine* audioEngine,
                     PluginScanner* pluginScanner, QObject* parent)
    : QObject(parent), editMgr_(editMgr), audioEngine_(audioEngine)
{
    toolExecutor_ = new AiToolExecutor(editMgr, audioEngine, pluginScanner, this);
    nam_ = new QNetworkAccessManager(this);
}

// ── Settings ────────────────────────────────────────────────────────────────

QString AiService::apiKey()
{
    QSettings s;
    return s.value("ai/apiKey").toString();
}

void AiService::setApiKey(const QString& key)
{
    QSettings s;
    s.setValue("ai/apiKey", key);
}

QString AiService::model()
{
    QSettings s;
    return s.value("ai/model", kDefaultModel).toString();
}

void AiService::setModel(const QString& m)
{
    QSettings s;
    s.setValue("ai/model", m);
}

bool AiService::confirmDestructive()
{
    QSettings s;
    return s.value("ai/confirmDestructive", true).toBool();
}

void AiService::setConfirmDestructive(bool confirm)
{
    QSettings s;
    s.setValue("ai/confirmDestructive", confirm);
}

bool AiService::previewMixPlanMode()
{
    QSettings s;
    return s.value("ai/previewMixPlanMode", false).toBool();
}

void AiService::setPreviewMixPlanMode(bool enabled)
{
    QSettings s;
    s.setValue("ai/previewMixPlanMode", enabled);
}

QString AiService::provider()
{
    return QSettings().value("ai/provider", "anthropic").toString();
}
void AiService::setProvider(const QString& provider) { QSettings().setValue("ai/provider", provider); }

QString AiService::localBaseUrl()
{
    return QSettings().value("ai/localBaseUrl", "http://localhost:11434/v1").toString();
}
void AiService::setLocalBaseUrl(const QString& url) { QSettings().setValue("ai/localBaseUrl", url); }

QString AiService::localModel() { return QSettings().value("ai/localModel").toString(); }
void AiService::setLocalModel(const QString& model) { QSettings().setValue("ai/localModel", model); }

QString AiService::localApiKey() { return QSettings().value("ai/localApiKey").toString(); }
void AiService::setLocalApiKey(const QString& key) { QSettings().setValue("ai/localApiKey", key); }

bool AiService::localUseTools() { return QSettings().value("ai/localUseTools", true).toBool(); }
void AiService::setLocalUseTools(bool enabled) { QSettings().setValue("ai/localUseTools", enabled); }

// Off by default: with tools, local models tend to think for thousands of
// tokens before each call (minutes per step on a consumer GPU).
QString AiService::localReasoningEffort() { return QSettings().value("ai/localReasoningEffort", "none").toString(); }
void AiService::setLocalReasoningEffort(const QString& effort) { QSettings().setValue("ai/localReasoningEffort", effort); }

int AiService::recommendedLocalContext(bool withTools)
{
    // About 4 characters per token, plus 4096 tokens for the system prompt
    // and the conversation, rounded up to a power of two as servers expect.
    int needed = 4096;
    if (withTools) {
        const auto tools = OpenAiCompat::toTools(AiToolDefs::allTools());
        needed += int(QJsonDocument(tools).toJson(QJsonDocument::Compact).size() / 4);
    }
    int context = 4096;
    while (context < needed)
        context *= 2;
    return context;
}

QString AiService::effort()
{
    return QSettings().value("ai/effort", "high").toString();
}

void AiService::setEffort(const QString& effort)
{
    QSettings().setValue("ai/effort", effort);
}

// Adds the per-model options: effort, the default server-side fallback for
// refusals, and preserved-thinking handling. The beta headers they need are
// combined into one anthropic-beta header.
void AiService::applyModelOptions(QJsonObject& body, QNetworkRequest& request,
                                  const QString& effortLevel, bool allowFallback) const
{
    const auto traits = traitsFor(body["model"].toString());
    QStringList betas;

    if (traits.effort) {
        QString level = effortLevel;
        if (level == "xhigh" && !traits.xhighEffort)
            level = "high";
        QJsonObject outputConfig;
        outputConfig["effort"] = level;
        body["output_config"] = outputConfig;
    }

    if (traits.fallbackDefault && allowFallback) {
        body["fallbacks"] = "default";
        betas << "server-side-fallback-2026-07-01";
    }

    // The history is append-only, so replayed thinking blocks should always
    // match; drop_block keeps an unexpected mismatch (e.g. a chat saved by an
    // older version) from failing the request.
    if (traits.preservedThinking) {
        QJsonObject binding;
        binding["prefix_mismatch_behavior"] = "drop_block";
        QJsonObject thinking;
        thinking["type"] = "adaptive";
        thinking["block_binding"] = binding;
        body["thinking"] = thinking;
        betas << "thinking-binding-controls-2026-08-01";
    }

    if (!betas.isEmpty())
        request.setRawHeader("anthropic-beta", betas.join(',').toUtf8());
}

void AiService::setBusy(bool b)
{
    if (busy_ != b) {
        busy_ = b;
        emit busyChanged(b);
    }
}

// ── System prompt ───────────────────────────────────────────────────────────

QString AiService::buildSystemPrompt() const
{
    QString prompt =
        "You are an AI assistant integrated into OpenDaw, a digital audio workstation. "
        "You help users manage their music projects by manipulating tracks, effects, "
        "routing, and transport controls.\n\n";

    prompt += "Rules:\n"
              "- Use the provided tools to manipulate the DAW. Call tools as needed.\n"
              "- For bulk operations, call multiple tools in sequence.\n"
              "- The latest project state arrives in a <project_state> block on the most recent "
              "user turn, and again after each round of tool results. It is a LIVE snapshot that "
              "includes your own changes. "
              "If you just created tracks or clips, they will appear in the snapshot — do NOT treat "
              "them as pre-existing or comment on them as if seeing them for the first time. "
              "Only reference things the USER mentioned or that existed BEFORE your first action.\n";
    return prompt + kSystemPromptRules + (previewMixPlanMode()
        ? QString("- Preview mode is ON: call preview_mix_plan before mutating tools and wait for user confirmation intent.\n")
        : QString());
}

// The live project state, sent as a hidden text block. It is appended to the
// conversation (never edited in place) so earlier turns stay byte-identical,
// which preserved thinking and prompt caching both rely on.
AiContentBlock AiService::buildProjectSnapshot() const
{
    QString prompt = "<project_state>\n";

    if (editMgr_ && editMgr_->edit()) {
        prompt += "Current project state (live snapshot — includes any changes you have already made in this conversation):\n";
        prompt += QString("- Tempo: %1 BPM\n").arg(editMgr_->getBpm(), 0, 'f', 1);
        prompt += QString("- Time signature: %1/%2\n")
                      .arg(editMgr_->getTimeSigNumerator())
                      .arg(editMgr_->getTimeSigDenominator());

        auto& transport = editMgr_->transport();
        QString state = transport.isRecording() ? "Recording" :
                        transport.isPlaying() ? "Playing" : "Stopped";
        prompt += QString("- Transport: %1 at %2s\n")
                      .arg(state)
                      .arg(transport.getPosition().inSeconds(), 0, 'f', 2);

        auto tracks = editMgr_->getAudioTracks();
        prompt += QString("- Tracks (%1):\n").arg(tracks.size());
        for (int i = 0; i < tracks.size(); ++i) {
            auto* t = tracks[i];
            QString type = editMgr_->isBusTrack(t) ? "bus" :
                          editMgr_->isMidiTrack(t) ? "midi" : "audio";
            QString name = QString::fromStdString(t->getName().toStdString());
            prompt += QString("  [%1] \"%2\" (%3)%4%5\n")
                          .arg(i).arg(name, type,
                               t->isMuted(false) ? " MUTED" : "",
                               t->isSolo(false) ? " SOLO" : "");
        }
    } else {
        prompt += "No project is open.\n";
    }
    prompt += "</project_state>";

    AiContentBlock block;
    block.type = "context";
    block.text = prompt;
    return block;
}

const char* const AiService::kSystemPromptRules =
              "- After completing actions, briefly confirm what you did.\n"
              "- You can create MIDI clips and write notes into them. Use create_midi_clip to make a clip, "
              "then add_midi_notes to populate it. Note positions are in beats relative to clip start. "
              "Batch as many notes as possible into a single add_midi_notes call for efficiency. "
              "For arpeggios, broken chords, block-chord comping and other repeating chord-based "
              "parts, use generate_midi_pattern with the chord progression instead of listing notes. "
              "To tighten timing, add swing or fix out-of-key notes in an existing clip, use "
              "quantize_midi_notes. "
              "Middle C is MIDI note 60. A bar in 4/4 time is 4 beats.\n"
              "- When users ask about effects parameters, use get_track_effects to see "
              "current values before modifying them.\n"
              "- Parameter values are normalized 0.0 to 1.0.\n"
              "- For mix/master requests, use staged workflow: analyze -> apply -> verify.\n"
              "- Before major processing, call analysis tools first.\n"
              "- Stop when targets are met or no meaningful improvement remains.\n"
              "\nMIDI & Music Theory (CRITICAL — follow strictly):\n"
              "- ALWAYS establish the key and scale BEFORE writing any notes. If the user specifies "
              "a key, use it. If not, ask or assume C major. State the key in your response.\n"
              "- Every note you write MUST belong to the scale of the stated key unless it is an "
              "intentional chromatic passing tone, approach note, or borrowed chord tone. "
              "Random out-of-key notes like Db in C major are WRONG.\n"
              "- Scale reference (use these and their relative modes):\n"
              "  Major (Ionian): W-W-H-W-W-W-H  (C major = C D E F G A B)\n"
              "  Natural minor (Aeolian): W-H-W-W-H-W-W  (A minor = A B C D E F G)\n"
              "  Harmonic minor: raise 7th  (A harm. minor = A B C D E F G#)\n"
              "  Melodic minor (ascending): raise 6th and 7th\n"
              "- Chromatic notes are ONLY acceptable when:\n"
              "  (a) They are passing tones connecting two diatonic notes stepwise (e.g. F-F#-G in C major).\n"
              "  (b) They are part of a secondary dominant or borrowed chord (e.g. E/G# resolving to Am in C major).\n"
              "  (c) The user explicitly requests chromatic or atonal writing.\n"
              "  (d) They are approach notes (half-step below a chord tone, on a weak beat, resolving immediately).\n"
              "- For chords, use diatonic triads/sevenths of the key unless a chromatic chord is musically justified:\n"
              "  C major diatonic triads: C Dm Em F G Am Bdim\n"
              "  A minor diatonic triads: Am Bdim C Dm Em F G\n"
              "- Double-check every note number against the key before submitting. If in doubt, stay diatonic.\n"
              "- When writing drums (channel 10 / GM percussion), key constraints do not apply.\n";

// ── Conversation ────────────────────────────────────────────────────────────

void AiService::clearConversation()
{
    if (currentReply_) {
        currentReply_->abort();
        currentReply_->deleteLater();
        currentReply_ = nullptr;
    }
    conversationHistory_.clear();
    toolRoundCount_ = 0;
    executedToolIds_.clear();
    setBusy(false);
}

void AiService::loadConversation(const QVector<AiMessage>& messages)
{
    clearConversation();
    conversationHistory_ = messages;
}

void AiService::requestSessionTitle(const QString& userMessage)
{
    if (provider() == "local") {
        QStringList words = userMessage.simplified().split(' ', Qt::SkipEmptyParts);
        QString title = words.mid(0, 6).join(' ');
        if (words.size() > 6) title += "...";
        if (!title.isEmpty())
            emit sessionTitleGenerated(title.left(60));
        return;
    }

    QString key = apiKey();
    if (key.isEmpty()) return;

    QJsonObject body;
    body["model"] = model();
    body["max_tokens"] = 1024;
    body["system"] = QString("Generate a concise 3-6 word title for this chat. "
                             "Reply with ONLY the title, no quotes or punctuation.");
    QJsonArray messages;
    QJsonObject userMsg;
    userMsg["role"] = "user";
    QJsonArray contentArr;
    QJsonObject textBlock;
    textBlock["type"] = "text";
    textBlock["text"] = userMessage;
    contentArr.append(textBlock);
    userMsg["content"] = contentArr;
    messages.append(userMsg);
    body["messages"] = messages;

    QUrl apiUrl("https://api.anthropic.com/v1/messages");
    QNetworkRequest request{apiUrl};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("x-api-key", key.toUtf8());
    request.setRawHeader("anthropic-version", "2023-06-01");
    request.setTransferTimeout(30000);
    applyModelOptions(body, request, "low", false);

    auto* reply = nam_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) return;

        auto doc = QJsonDocument::fromJson(reply->readAll());
        if (doc.isNull()) return;
        auto obj = doc.object();
        // Responses can start with thinking blocks; take the first text block.
        QString title;
        for (const auto& val : obj["content"].toArray()) {
            if (val.toObject()["type"].toString() == "text") {
                title = val.toObject()["text"].toString().trimmed();
                break;
            }
        }
        if (title.isEmpty()) return;

        if (title.length() > 60)
            title = title.left(57) + "...";

        emit sessionTitleGenerated(title);
    });
}

void AiService::sendMessage(const QString& userText)
{
    if (busy_) return;

    if (provider() == "local") {
        if (localModel().isEmpty()) {
            emit errorOccurred("No local model selected. Open Settings > AI Assistant to choose one.");
            return;
        }
    } else if (apiKey().isEmpty()) {
        emit errorOccurred("No API key set. Open Settings > AI Assistant to add your Anthropic API key.");
        return;
    }

    AiMessage userMsg = AiMessage::userMessage(userText);
    userMsg.content.prepend(buildProjectSnapshot());
    conversationHistory_.append(userMsg);
    toolRoundCount_ = 0;

    sendToApi();
}

// ── API call ────────────────────────────────────────────────────────────────

QJsonObject AiService::messageToJson(const AiMessage& msg) const
{
    QJsonObject obj;
    obj["role"] = (msg.role == AiRole::User) ? "user" : "assistant";

    QJsonArray contentArr;
    for (auto& block : msg.content) {
        QJsonObject blockObj;
        if (block.type == "text" || block.type == "context") {
            blockObj["type"] = "text";
            blockObj["text"] = block.text;
        } else if (block.type == "thinking") {
            blockObj["type"] = "thinking";
            blockObj["thinking"] = block.thinking;
            blockObj["signature"] = block.signature;
        } else if (block.type == "redacted_thinking") {
            blockObj["type"] = "redacted_thinking";
            blockObj["data"] = block.data;
        } else if (block.type == "tool_use") {
            blockObj["type"] = "tool_use";
            blockObj["id"] = block.toolCall.id;
            blockObj["name"] = block.toolCall.name;
            blockObj["input"] = block.toolCall.input;
        } else if (block.type == "tool_result") {
            blockObj["type"] = "tool_result";
            blockObj["tool_use_id"] = block.toolResult.toolUseId;
            blockObj["content"] = block.toolResult.content;
            if (block.toolResult.isError)
                blockObj["is_error"] = true;
        } else {
            continue;  // e.g. "fallback": an audit marker, not sent back
        }
        contentArr.append(blockObj);
    }
    obj["content"] = contentArr;
    return obj;
}

void AiService::sendToApi()
{
    setBusy(true);
    streamBuffer_.clear();
    currentAssistantText_.clear();
    currentContentBlocks_.clear();
    currentToolInputStr_.clear();
    inToolInput_ = false;
    currentBlockIndex_ = -1;
    currentStopReason_.clear();
    currentRefusalCategory_.clear();
    localRequest_ = provider() == "local";

    if (localRequest_) {
        sendToLocalServer();
        return;
    }

    QJsonObject body;
    body["model"] = model();
    body["max_tokens"] = kMaxTokens;
    body["stream"] = true;
    body["system"] = buildSystemPrompt();
    body["tools"] = AiToolDefs::allTools();

    QJsonArray messages;
    for (auto& msg : conversationHistory_)
        messages.append(messageToJson(msg));
    body["messages"] = messages;

    QUrl apiUrl(kApiUrl);
    QNetworkRequest request{apiUrl};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("x-api-key", apiKey().toUtf8());
    request.setRawHeader("anthropic-version", "2023-06-01");
    applyModelOptions(body, request, effort(), true);

    request.setTransferTimeout(60000);

    QByteArray bodyData = QJsonDocument(body).toJson(QJsonDocument::Compact);
    currentReply_ = nam_->post(request, bodyData);

    connect(currentReply_, &QNetworkReply::readyRead,
            this, &AiService::handleStreamChunk);
    connect(currentReply_, &QNetworkReply::finished,
            this, &AiService::handleStreamFinished);
}

// ── Local (OpenAI-compatible) server ────────────────────────────────────────

void AiService::sendToLocalServer()
{
    localStream_ = {};

    QJsonObject body;
    body["model"] = localModel();
    body["stream"] = true;
    body["max_tokens"] = kMaxLocalTokens;
    // Servers without reasoning support ignore this field.
    if (!localReasoningEffort().isEmpty())
        body["reasoning_effort"] = localReasoningEffort();
    body["messages"] = OpenAiCompat::toMessages(buildSystemPrompt(), conversationHistory_);
    if (localUseTools())
        body["tools"] = OpenAiCompat::toTools(AiToolDefs::allTools());

    QString base = localBaseUrl().trimmed();
    while (base.endsWith('/')) base.chop(1);
    QNetworkRequest request{QUrl(base + "/chat/completions")};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!localApiKey().isEmpty())
        request.setRawHeader("Authorization", ("Bearer " + localApiKey()).toUtf8());
    // Local models can take a while to load before the first token.
    request.setTransferTimeout(300000);

    localTimer_.start();
    currentReply_ = nam_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(currentReply_, &QNetworkReply::readyRead,
            this, &AiService::handleLocalStreamChunk);
    connect(currentReply_, &QNetworkReply::finished,
            this, &AiService::handleStreamFinished);
}

void AiService::handleLocalStreamChunk()
{
    if (!currentReply_) return;
    streamBuffer_.append(currentReply_->readAll());

    while (true) {
        int nlPos = streamBuffer_.indexOf('\n');
        if (nlPos < 0) break;

        QByteArray line = streamBuffer_.left(nlPos).trimmed();
        streamBuffer_.remove(0, nlPos + 1);
        if (!line.startsWith("data:")) continue;

        QByteArray jsonData = line.mid(5).trimmed();
        if (jsonData == "[DONE]") continue;

        auto doc = QJsonDocument::fromJson(jsonData);
        if (!doc.isObject()) continue;

        const int reasoningBefore = localStream_.reasoningChunks;
        const QString text = OpenAiCompat::applyChunk(doc.object(), localStream_);
        if (!text.isEmpty())
            emit tokenReceived(text);
        else if (localStream_.reasoningChunks != reasoningBefore)
            emit thinkingProgress(localStream_.reasoningChunks);
    }
}

// ── SSE stream parsing ──────────────────────────────────────────────────────

void AiService::handleStreamChunk()
{
    if (!currentReply_) return;
    streamBuffer_.append(currentReply_->readAll());

    while (true) {
        int nlPos = streamBuffer_.indexOf('\n');
        if (nlPos < 0) break;

        QByteArray line = streamBuffer_.left(nlPos).trimmed();
        streamBuffer_.remove(0, nlPos + 1);

        if (line.isEmpty() || line.startsWith("event:")) continue;
        if (!line.startsWith("data: ")) continue;

        QByteArray jsonData = line.mid(6);
        if (jsonData == "[DONE]") continue;

        QJsonParseError parseErr;
        auto doc = QJsonDocument::fromJson(jsonData, &parseErr);
        if (parseErr.error != QJsonParseError::NoError) continue;

        auto obj = doc.object();
        QString type = obj["type"].toString();

        if (type == "content_block_start") {
            auto blockObj = obj["content_block"].toObject();
            QString blockType = blockObj["type"].toString();
            currentBlockIndex_ = obj["index"].toInt();

            AiContentBlock block;
            block.type = blockType;

            if (blockType == "thinking") {
                block.thinking = blockObj["thinking"].toString();
                block.signature = blockObj["signature"].toString();
            } else if (blockType == "redacted_thinking") {
                block.data = blockObj["data"].toString();
            } else if (blockType == "tool_use") {
                block.toolCall.id = blockObj["id"].toString();
                block.toolCall.name = blockObj["name"].toString();
                inToolInput_ = true;
                currentToolInputStr_.clear();
                emit toolCallStarted(block.toolCall.name, block.toolCall.id);
            }

            currentContentBlocks_.append(block);
        }
        else if (type == "content_block_delta") {
            auto delta = obj["delta"].toObject();
            QString deltaType = delta["type"].toString();

            if (deltaType == "text_delta") {
                QString text = delta["text"].toString();
                currentAssistantText_ += text;
                if (currentBlockIndex_ >= 0 && currentBlockIndex_ < currentContentBlocks_.size())
                    currentContentBlocks_[currentBlockIndex_].text += text;
                emit tokenReceived(text);
            }
            else if (deltaType == "input_json_delta") {
                currentToolInputStr_ += delta["partial_json"].toString();
            }
            else if (currentBlockIndex_ >= 0 && currentBlockIndex_ < currentContentBlocks_.size()) {
                auto& block = currentContentBlocks_[currentBlockIndex_];
                if (deltaType == "thinking_delta")
                    block.thinking += delta["thinking"].toString();
                else if (deltaType == "signature_delta")
                    block.signature = delta["signature"].toString();
            }
        }
        else if (type == "message_delta") {
            auto delta = obj["delta"].toObject();
            if (delta.contains("stop_reason"))
                currentStopReason_ = delta["stop_reason"].toString();
            auto details = delta["stop_details"].toObject();
            if (!details.isEmpty())
                currentRefusalCategory_ = details["category"].toString();
        }
        else if (type == "content_block_stop") {
            if (inToolInput_ && currentBlockIndex_ >= 0
                && currentBlockIndex_ < currentContentBlocks_.size()) {
                auto& block = currentContentBlocks_[currentBlockIndex_];
                if (block.type == "tool_use") {
                    auto inputDoc = QJsonDocument::fromJson(currentToolInputStr_.toUtf8());
                    block.toolCall.input = inputDoc.object();
                }
                inToolInput_ = false;
                currentToolInputStr_.clear();
            }
            currentBlockIndex_ = -1;
        }
        else if (type == "message_stop") {
            // handled in handleStreamFinished
        }
        else if (type == "error") {
            auto errObj = obj["error"].toObject();
            emit errorOccurred(errObj["message"].toString());
            setBusy(false);
        }
    }
}

void AiService::handleStreamFinished()
{
    if (!currentReply_) return;

    if (localRequest_) {
        QStringList calls;
        for (const auto& call : localStream_.toolCalls)
            calls << QString("%1 (%2 chars)").arg(call.name).arg(call.arguments.size());
        qInfo().noquote() << QString("[AI local] %1 s, finish=%2, thinking=%3 chunks, "
                                     "text=%4 chars, tools=[%5], network=%6")
            .arg(localTimer_.elapsed() / 1000.0, 0, 'f', 1)
            .arg(localStream_.finishReason.isEmpty() ? "none" : localStream_.finishReason)
            .arg(localStream_.reasoningChunks).arg(localStream_.text.size())
            .arg(calls.join(", "))
            .arg(currentReply_->error() == QNetworkReply::NoError ? "ok"
                                                                 : currentReply_->errorString());
    }

    if (currentReply_->error() != QNetworkReply::NoError
        && currentReply_->error() != QNetworkReply::OperationCanceledError) {
        QByteArray responseBody = currentReply_->readAll();
        QString errorMsg = currentReply_->errorString();

        auto doc = QJsonDocument::fromJson(responseBody);
        if (doc.isObject()) {
            const auto errVal = doc.object()["error"];
            if (errVal.isObject() && !errVal.toObject()["message"].toString().isEmpty())
                errorMsg = errVal.toObject()["message"].toString();
            else if (errVal.isString())
                errorMsg = errVal.toString();  // e.g. Ollama
        }
        if (localRequest_ && currentReply_->error() == QNetworkReply::ConnectionRefusedError)
            errorMsg = QString("Could not reach the local server at %1. Is it running?")
                           .arg(localBaseUrl());

        emit errorOccurred(errorMsg);
        setBusy(false);
        currentReply_->deleteLater();
        currentReply_ = nullptr;
        return;
    }

    currentReply_->deleteLater();
    currentReply_ = nullptr;

    if (localRequest_) {
        QStringList badToolCalls;
        currentContentBlocks_ = OpenAiCompat::toContentBlocks(localStream_, &badToolCalls);
        if (localStream_.finishReason == "length")
            emit errorOccurred(QString("The model reached the %1-token output limit before "
                                       "finishing, so its last step may be incomplete. Try a "
                                       "smaller request, or a model that thinks less.")
                                   .arg(kMaxLocalTokens));
        if (!badToolCalls.isEmpty())
            emit errorOccurred(QString("The model sent invalid arguments for %1, so that "
                                       "step did nothing.").arg(badToolCalls.join(", ")));
        // An empty assistant turn would be rejected if the chat later moves to
        // Claude, so record that nothing came back.
        if (currentContentBlocks_.isEmpty()) {
            AiContentBlock block;
            block.type = "text";
            block.text = "(The model returned an empty response.)";
            currentContentBlocks_.append(block);
        }
    }

    processAssistantResponse();
}

// ── Process response & agentic loop ─────────────────────────────────────────

void AiService::processAssistantResponse()
{
    // A refusal (from the whole fallback chain) leaves no usable answer:
    // discard any partial output instead of treating it as complete.
    if (currentStopReason_ == "refusal") {
        QString msg = "Claude declined this request.";
        if (!currentRefusalCategory_.isEmpty())
            msg += QString(" (category: %1)").arg(currentRefusalCategory_);
        emit errorOccurred(msg);
        setBusy(false);
        return;
    }

    // After a mid-output fallback, the declined model's thinking and tool
    // calls before the last switch point must not be run or sent back.
    QVector<AiContentBlock> content = currentContentBlocks_;
    int lastFallback = -1;
    for (int i = 0; i < content.size(); ++i)
        if (content[i].type == "fallback") lastFallback = i;
    if (lastFallback >= 0) {
        QVector<AiContentBlock> kept;
        for (int i = 0; i < content.size(); ++i) {
            const auto& t = content[i].type;
            if (i < lastFallback && (t == "thinking" || t == "redacted_thinking" || t == "tool_use"))
                continue;
            kept.append(content[i]);
        }
        content = kept;
    }

    AiMessage assistantMsg;
    assistantMsg.role = AiRole::Assistant;
    assistantMsg.timestamp = QDateTime::currentDateTime();
    assistantMsg.content = content;

    conversationHistory_.append(assistantMsg);

    if (assistantMsg.hasToolUse() && toolRoundCount_ < MAX_TOOL_ROUNDS) {
        toolRoundCount_++;
        QVector<AiToolCall> calls;
        for (auto& block : assistantMsg.content) {
            if (block.type == "tool_use")
                calls.append(block.toolCall);
        }
        executeToolCalls(calls);
    } else {
        emit responseComplete(assistantMsg);
        setBusy(false);
    }
}

void AiService::executeToolCalls(const QVector<AiToolCall>& calls)
{
    executedToolIds_.clear();
    QVector<AiToolResult> results;

    toolExecutor_->beginBatch();

    for (auto& call : calls) {
        if (confirmDestructive() && toolExecutor_->isDestructive(call.name)) {
            toolExecutor_->endBatch();
            emit confirmDestructiveAction(call.name, call);
            return;
        }

        auto result = toolExecutor_->execute(call);
        results.append(result);
        executedToolIds_.insert(call.id);

        emit toolCallFinished(call.name, call.id, result.content, result.isError);
    }

    toolExecutor_->endBatch();

    AiMessage assistantMsg = conversationHistory_.last();
    continueAfterToolResults(results, assistantMsg);
}

void AiService::executeConfirmedTool(const AiToolCall& call)
{
    auto result = toolExecutor_->execute(call);
    emit toolCallFinished(call.name, call.id, result.content, result.isError);
    executedToolIds_.insert(call.id);

    AiMessage assistantMsg = conversationHistory_.last();
    QVector<AiToolResult> allResults;

    for (auto& block : assistantMsg.content) {
        if (block.type == "tool_use") {
            if (block.toolCall.id == call.id) {
                allResults.append(result);
            } else if (executedToolIds_.contains(block.toolCall.id)) {
                continue;
            } else if (confirmDestructive() && toolExecutor_->isDestructive(block.toolCall.name)) {
                emit confirmDestructiveAction(block.toolCall.name, block.toolCall);
                return;
            } else {
                auto existingResult = toolExecutor_->execute(block.toolCall);
                allResults.append(existingResult);
                executedToolIds_.insert(block.toolCall.id);
                emit toolCallFinished(block.toolCall.name, block.toolCall.id,
                                     existingResult.content, existingResult.isError);
            }
        }
    }

    continueAfterToolResults(allResults, assistantMsg);
}

void AiService::cancelPendingTool(const AiToolCall& call)
{
    AiToolResult cancelResult;
    cancelResult.toolUseId = call.id;
    cancelResult.content = "User cancelled this action.";
    cancelResult.isError = true;

    AiMessage assistantMsg = conversationHistory_.last();
    QVector<AiToolResult> results;
    for (auto& block : assistantMsg.content) {
        if (block.type == "tool_use") {
            if (block.toolCall.id == call.id)
                results.append(cancelResult);
            else {
                AiToolResult skipResult;
                skipResult.toolUseId = block.toolCall.id;
                skipResult.content = "Skipped because another tool was cancelled.";
                skipResult.isError = true;
                results.append(skipResult);
            }
        }
    }
    continueAfterToolResults(results, assistantMsg);
}

void AiService::continueAfterToolResults(const QVector<AiToolResult>& results,
                                          const AiMessage& /*assistantMsg*/)
{
    AiMessage toolResultMsg;
    toolResultMsg.role = AiRole::User;
    toolResultMsg.timestamp = QDateTime::currentDateTime();

    for (auto& result : results) {
        AiContentBlock block;
        block.type = "tool_result";
        block.toolResult = result;
        toolResultMsg.content.append(block);
    }
    toolResultMsg.content.append(buildProjectSnapshot());

    conversationHistory_.append(toolResultMsg);

    sendToApi();
}

} // namespace OpenDaw
