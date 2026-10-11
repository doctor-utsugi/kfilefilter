// Opens a real KDE dialog, types into the injected filter and saves a
// screenshot. Run it with LD_PRELOAD pointing at kfilefilter.so; it exits 1
// when the filter box is missing.
//
//   smoke file <start dir> <text> <png>   KFileWidget in detail tree view
//   smoke dir  <start dir> <text> <png>   KDirSelectDialog, through QFileDialog

#include <KDirOperator>
#include <KFileWidget>
#include <QApplication>
#include <QFileDialog>
#include <QLineEdit>
#include <QTimer>
#include <QUrl>

static QLineEdit *filterBox(QWidget *dialog)
{
    for (QLineEdit *edit : dialog->findChildren<QLineEdit *>()) {
        if (edit->placeholderText() == QLatin1String("Filter (Ctrl+F)")) {
            return edit;
        }
    }
    return nullptr;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (argc != 5) {
        qCritical("usage: smoke file|dir <start dir> <text> <png>");
        return 2;
    }
    const bool dirMode = qstrcmp(argv[1], "dir") == 0;
    const QString start = QString::fromLocal8Bit(argv[2]);
    const QString text = QString::fromLocal8Bit(argv[3]);
    const QString shot = QString::fromLocal8Bit(argv[4]);

    KFileWidget fileWidget(QUrl::fromLocalFile(start));
    QFileDialog folderDialog;
    QWidget *dialog = nullptr;

    if (dirMode) {
        folderDialog.setFileMode(QFileDialog::Directory);
        folderDialog.setOption(QFileDialog::ShowDirsOnly);
        folderDialog.setDirectory(start);
        folderDialog.show();
    } else {
        fileWidget.setMode(KFile::Directory | KFile::ExistingOnly | KFile::LocalOnly);
        fileWidget.resize(1000, 800);
        fileWidget.show();
        // After show(): KFileWidget applies the saved view mode while showing.
        QTimer::singleShot(500, [&] {
            fileWidget.dirOperator()->setViewMode(KFile::FileView(KFile::Detail | KFile::Tree));
        });
    }

    QTimer::singleShot(1500, [&] {
        if (dirMode) {
            for (QWidget *top : QApplication::topLevelWidgets()) {
                if (top->inherits("KDirSelectDialog") && top->isVisible()) {
                    dialog = top;
                }
            }
        } else {
            dialog = &fileWidget;
        }
        QLineEdit *filter = dialog ? filterBox(dialog) : nullptr;
        if (!filter) {
            qCritical("filter box not found");
            app.exit(1);
            return;
        }
        filter->setText(text);
        QTimer::singleShot(4000, [&] {
            dialog->grab().save(shot);
            app.exit(0);
        });
    });
    return app.exec();
}
