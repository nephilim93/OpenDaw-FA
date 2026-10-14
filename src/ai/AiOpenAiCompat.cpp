#include "AiOpenAiCompat.h"
#include <QJsonDocument>
#include <QUuid>

namespace OpenDaw::OpenAiCompat {

QJsonArray toMessages(const QString& systemPrompt, const QVector<AiMessage>& history)
{
    QJsonArray messages;
    messages.append(QJsonObject{ { "role", "system" }, { "content", systemPrompt } });

    for (const auto& msg : history) {
        if (msg.role == AiRole::Assistant) {
            QString text;
            QJsonArray toolCalls;
            for (const auto& block : msg.content) {
                if (block.type == "text") {
                    text += block.text;
                } else if (block.type == "tool_use") {
                    QJsonObject fn{
                        { "name", block.toolCall.name },
                        { "arguments", QString::fromUtf8(
                              QJsonDocument(block.toolCall.input).toJson(QJsonDocument::Compact)) },
                    };
                    toolCalls.append(QJsonObject{
                        { "id", block.toolCall.id }, { "type", "function" }, { "function", fn } });
                }
            }
            QJsonObject out{ { "role", "assistant" }, { "content", text } };
            if (!toolCalls.isEmpty())
                out["tool_calls"] = toolCalls;
            messages.append(out);
            continue;
        }

        // User turns: tool results become their own "tool" messages, and the
        // remaining text (including hidden project snapshots) one user message.
        QString text;
        for (const auto& block : msg.content) {
            if (block.type == "tool_result") {
                QString content = block.toolResult.content;
                if (block.toolResult.isError)
                    content = "Error: " + content;
                messages.append(QJsonObject{ { "role", "tool" },
                                             { "tool_call_id", block.toolResult.toolUseId },
                                             { "content", content } });
            } else if (block.type == "text" || block.type == "context") {
                if (!text.isEmpty()) text += "\n\n";
                text += block.text;
            }
        }
        if (!text.isEmpty())
            messages.append(QJsonObject{ { "role", "user" }, { "content", text } });
    }
    return messages;
}

QJsonArray toTools(const QJsonArray& anthropicTools)
{
    QJsonArray tools;
    for (const auto& val : anthropicTools) {
        const auto tool = val.toObject();
        QJsonObject fn{
            { "name", tool["name"] },
            { "description", tool["description"] },
            { "parameters", tool["input_schema"] },
        };
        tools.append(QJsonObject{ { "type", "function" }, { "function", fn } });
    }
    return tools;
}

QString applyChunk(const QJsonObject& chunk, StreamState& state)
{
    const auto choices = chunk["choices"].toArray();
    if (choices.isEmpty()) return {};
    const auto choice = choices[0].toObject();
    const auto delta = choice["delta"].toObject();

    const QString text = delta["content"].toString();
    state.text += text;

    // Tool calls arrive in pieces: the first piece of each carries its id and
    // name, later pieces append to the arguments string.
    for (const auto& val : delta["tool_calls"].toArray()) {
        const auto tc = val.toObject();
        auto& call = state.toolCalls[tc["index"].toInt(state.toolCalls.size())];
        if (tc.contains("id") && !tc["id"].toString().isEmpty())
            call.id = tc["id"].toString();
        const auto fn = tc["function"].toObject();
        if (fn.contains("name") && !fn["name"].toString().isEmpty())
            call.name = fn["name"].toString();
        call.arguments += fn["arguments"].toString();
    }

    if (choice.contains("finish_reason") && !choice["finish_reason"].isNull())
        state.finishReason = choice["finish_reason"].toString();
    return text;
}

QVector<AiContentBlock> toContentBlocks(const StreamState& state)
{
    QVector<AiContentBlock> blocks;
    if (!state.text.isEmpty()) {
        AiContentBlock block;
        block.type = "text";
        block.text = state.text;
        blocks.append(block);
    }
    for (const auto& call : state.toolCalls) {
        if (call.name.isEmpty()) continue;
        AiContentBlock block;
        block.type = "tool_use";
        // Some servers omit ids; the tool loop needs one to match results.
        block.toolCall.id = call.id.isEmpty()
            ? "call_" + QUuid::createUuid().toString(QUuid::Id128)
            : call.id;
        block.toolCall.name = call.name;
        block.toolCall.input = QJsonDocument::fromJson(call.arguments.toUtf8()).object();
        blocks.append(block);
    }
    return blocks;
}

} // namespace OpenDaw::OpenAiCompat
