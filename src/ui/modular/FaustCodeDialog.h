#pragma once

#include <QDialog>
#include <functional>

class QLabel;
class QPlainTextEdit;

namespace OpenDaw {

// Writes or edits a Faust code module's source. Compile checks the code
// without touching the patch; Save hands it to `save`, which applies it as
// one undo step and returns an error to show (the dialog then stays open
// with the user's text) or an empty string to close.
class FaustCodeDialog : public QDialog {
    Q_OBJECT
public:
    using SaveFn = std::function<QString(const QString& code)>;

    FaustCodeDialog(const QString& moduleId, const QString& code, SaveFn save,
                    QWidget* parent = nullptr);

    void setSaveFunction(SaveFn save) { save_ = std::move(save); }
    QString code() const;
    // Compiles the current text and shows the result; true if it compiled.
    bool compile();

    // A small working module to start from.
    static QString templateCode();

private:
    void trySave();
    void showStatus(const QString& text, bool error);

    QString moduleId_;
    SaveFn save_;
    QPlainTextEdit* editor_ = nullptr;
    QLabel* status_ = nullptr;
};

} // namespace OpenDaw
