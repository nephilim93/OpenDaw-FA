#include "FaustCodeDialog.h"
#include "modular/ModuleRegistry.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace OpenDaw {

FaustCodeDialog::FaustCodeDialog(const QString& moduleId, const QString& code, SaveFn save,
                                 QWidget* parent)
    : QDialog(parent), moduleId_(moduleId), save_(std::move(save))
{
    setWindowTitle(QString("Faust Code — %1").arg(moduleId));
    setAccessibleName("Faust Code Dialog");
    resize(640, 480);

    auto* layout = new QVBoxLayout(this);
    auto* hint = new QLabel(
        "Write Faust with a 'process = ...;' line; import(\"stdfaust.lib\") gives os., fi., "
        "ef., re., ba. and the rest. One audio input is 'in' (several: in1..inN), outputs "
        "likewise; each hslider, vslider, nentry, button and checkbox becomes a knob named "
        "by its label. Audio is ±1.", this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    editor_ = new QPlainTextEdit(this);
    editor_->setAccessibleName("Faust Source");
    editor_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor_->setTabStopDistance(4 * editor_->fontMetrics().horizontalAdvance(' '));
    editor_->setPlainText(code);
    layout->addWidget(editor_, 1);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    auto* compileBtn = buttons->addButton("Compile", QDialogButtonBox::ActionRole);
    connect(compileBtn, &QPushButton::clicked, this, [this]() { compile(); });
    connect(buttons, &QDialogButtonBox::accepted, this, &FaustCodeDialog::trySave);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    if (!Modular::codeModulesAvailable())
        showStatus("This build of OpenDaw has no Faust support, so the code can't be compiled.", true);
}

QString FaustCodeDialog::code() const
{
    return editor_->toPlainText();
}

bool FaustCodeDialog::compile()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto result = Modular::compileCodeModule(moduleId_.toStdString(), code().toStdString());
    QApplication::restoreOverrideCursor();
    if (!result.def) {
        showStatus(QString::fromStdString(result.error), true);
        return false;
    }
    const auto& d = *result.def;
    showStatus(QString("ok: %1 in, %2 out, %3 params")
                   .arg(d.inputs.size()).arg(d.outputs.size()).arg(d.params.size()), false);
    return true;
}

void FaustCodeDialog::trySave()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QString error = save_ ? save_(code()) : QString();
    QApplication::restoreOverrideCursor();
    if (!error.isEmpty()) {
        showStatus(error, true);
        return;
    }
    accept();
}

void FaustCodeDialog::showStatus(const QString& text, bool error)
{
    status_->setStyleSheet(error ? "color: #e57373;" : "color: #81c784;");
    status_->setText(text);
}

QString FaustCodeDialog::templateCode()
{
    return "import(\"stdfaust.lib\");\n"
           "\n"
           "// A gain with a soft clip. Edit freely: ports and knobs follow the code.\n"
           "gain  = hslider(\"gain\", 1, 0, 4, 0.01);\n"
           "drive = hslider(\"drive\", 0, 0, 1, 0.01);\n"
           "\n"
           "process = *(gain) <: ma.tanh, _ : *(drive), *(1 - drive) :> _;\n";
}

} // namespace OpenDaw
