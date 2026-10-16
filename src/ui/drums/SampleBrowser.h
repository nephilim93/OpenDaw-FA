#pragma once

#include <QWidget>

class QFileSystemModel;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTreeView;

namespace OpenDaw {

// The samples folder (Settings > System) as a tree beside the pads: drag a
// file onto a pad to load it, or double-click it to load it on the selected
// pad; right-click a folder to build a kit from it. Follows the folder when
// Settings changes it.
class SampleBrowser : public QWidget {
    Q_OBJECT

public:
    explicit SampleBrowser(QWidget* parent = nullptr);

    QString folder() const { return folder_; }
    QTreeView* tree() const { return tree_; }

public slots:
    void refresh();

signals:
    void sampleActivated(const QString& path);
    // Right-click > Build Kit from This Folder.
    void folderKitRequested(const QString& folder);

private:
    QString folder_;
    QStackedWidget* stack_ = nullptr;
    QLabel* folderLabel_ = nullptr;
    QLabel* missingLabel_ = nullptr;
    QFileSystemModel* model_ = nullptr;
    QTreeView* tree_ = nullptr;
};

} // namespace OpenDaw
