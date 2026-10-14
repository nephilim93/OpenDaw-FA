#pragma once

#include "AiTypes.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

namespace OpenDaw {

// Conversion between the assistant's Anthropic-style conversation and the
// OpenAI-compatible chat API that local servers (Ollama, LM Studio,
// llama.cpp server, Hugging Face TGI) implement.
namespace OpenAiCompat {

// History -> OpenAI "messages" (system first; tool results become "tool"
// messages; thinking, fallback markers and other Claude-only blocks are
// dropped).
QJsonArray toMessages(const QString& systemPrompt, const QVector<AiMessage>& history);

// Anthropic tool definitions ({name, description, input_schema}) -> OpenAI
// function tools.
QJsonArray toTools(const QJsonArray& anthropicTools);

// Accumulates one streamed reply.
struct StreamState {
    struct ToolCall { QString id, name, arguments; };
    QString text;
    QMap<int, ToolCall> toolCalls;  // by stream index
    QString finishReason;
};

// Applies one parsed "data:" chunk; returns any new reply text.
QString applyChunk(const QJsonObject& chunk, StreamState& state);

// The finished reply as content blocks (text, then tool_use).
QVector<AiContentBlock> toContentBlocks(const StreamState& state);

} // namespace OpenAiCompat
} // namespace OpenDaw
