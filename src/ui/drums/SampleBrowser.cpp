#include "SampleBrowser.h"
#include "DrumsViewLink.h"
#include "drums/SampleLibrary.h"
#include "utils/ThemeManager.h"
#include <QDesktopServices>
#include <QDir>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace OpenDaw {

SampleBrowser::SampleBrowser(QWidget* parent) : QWidget(parent)
{
    setAccessibleName("Sample Browser");
    auto& theme = ThemeManager::instance().current();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 4, 6);
    layout->setSpacing(4);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel("Samples", this);
    title->setStyleSheet(QString("color: %1; font-weight: bold;").arg(theme.text.name()));
    header->addWidget(title);
    header->addStretch();
    auto* open = new QPushButton("Open Folder", this);
    open->setAccessibleName("Open Samples Folder");
    open->setToolTip("Show the samples folder, to add files to it (change it in Settings > System)");
    connect(open, &QPushButton::clicked, this, [this]() {
        QDir().mkpath(folder_);
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder_));
        refresh();
    });
    header->addWidget(open);
    layout->addLayout(header);

    folderLabel_ = new QLabel(this);
    folderLabel_->setStyleSheet(QString("color: %1; font-size: 10px;").arg(theme.textDim.name()));
    folderLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(folderLabel_);

    stack_ = new QStackedWidget(this);
    model_ = new QFileSystemModel(this);
    model_->setNameFilters({ "*.wav", "*.aiff", "*.aif", "*.flac", "*.ogg" });
    model_->setNameFilterDisables(false);
    tree_ = new QTreeView(stack_);
    tree_->setAccessibleName("Sample Files");
    tree_->setModel(model_);
    tree_->setIndentation(10);
    tree_->setDragEnabled(true);
    tree_->setDragDropMode(QAbstractItemView::DragOnly);
    tree_->setHeaderHidden(true);
    for (int c = 1; c < model_->columnCount(); ++c) tree_->hideColumn(c);
    tree_->setToolTip("Drag a sample onto a pad, or double-click it to load it on the selected pad");
    connect(tree_, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
        if (!model_->isDir(index)) emit sampleActivated(model_->filePath(index));
    });
    stack_->addWidget(tree_);

    missingLabel_ = new QLabel(stack_);
    missingLabel_->setWordWrap(true);
    missingLabel_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    missingLabel_->setStyleSheet(QString("color: %1; font-size: 11px;").arg(theme.textDim.name()));
    stack_->addWidget(missingLabel_);
    layout->addWidget(stack_, 1);

    connect(&DrumsViewLink::instance(), &DrumsViewLink::samplesFolderChanged, this, &SampleBrowser::refresh);
    refresh();
}

void SampleBrowser::refresh()
{
    folder_ = Drums::samplesFolder();
    folderLabel_->setText(QDir::toNativeSeparators(folder_));
    folderLabel_->setToolTip(folderLabel_->text());
    if (!QDir(folder_).exists()) {
        missingLabel_->setText("The samples folder doesn't exist yet. Click Open Folder to create it "
                               "and add samples, or choose another one in Settings > System.");
        stack_->setCurrentWidget(missingLabel_);
        return;
    }
    model_->setRootPath(folder_);
    tree_->setRootIndex(model_->index(folder_));
    stack_->setCurrentWidget(tree_);
}

} // namespace OpenDaw
