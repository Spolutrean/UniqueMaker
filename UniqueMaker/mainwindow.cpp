#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QtMath>
#include <QDebug>
#include <QThread>
#include <set>
#include <QTimer>
#include <QProgressDialog>

#include "QtConcurrent/qtconcurrentmap.h"


quint8 threadsCount;
quint64 progress = 0, sizeOfAllFiles = 0;
std::map<std::pair<quint64, QByteArray>, QStringList> MainWindow::hashedFiles;
QMutex MainWindow::mutex;
QFutureWatcher<void> MainWindow::watcher;

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    ui->treeWidget->setHeaderLabel("Repeated files:");
    ui->spinBox->setValue(QThread::idealThreadCount());
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::on_pushButton_clicked()
{
    QString dir = QFileDialog::getExistingDirectory(this,
                                                    "Select Directory for Scanning",
                                                    QString(),
                                                    QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    ui->directoryPath->setText(dir);
    ui->startScanning->setEnabled(true);
}

void MainWindow::getHashOfFile(QFile &file, QCryptographicHash &hashMaker)
{
    file.open(QIODevice::ReadOnly);
    quint64 blockSize = qMin(qMax((int)qSqrt(file.size()), 1<<14), 1<<17);
    quint64 countFullBLocks = file.size() / blockSize;
    quint64 partInTheEnd = file.size() - blockSize * countFullBLocks;
    quint64 handled = 0;
    for(size_t i = 0; i < countFullBLocks; ++i)
    {
        file.seek(i * blockSize);
        hashMaker.addData(file.read(blockSize));
        handled += blockSize >> 10;
        if(mutex.try_lock())
        {
            progress += handled;
            mutex.unlock();
            handled = 0;
            emit MainWindow::watcher.progressValueChanged(progress);
        }
    }
    file.seek(blockSize * countFullBLocks);
    hashMaker.addData(file.read(partInTheEnd));
    mutex.lock();
    progress += partInTheEnd >> 10;
    mutex.unlock();
    emit MainWindow::watcher.progressValueChanged(progress);
    file.close();
}

void MainWindow::findAllFilesInDirectory(QString const &dirPath)
{
    QDir curDir(dirPath);
    if(!curDir.exists())
    {
        QMessageBox mb;
        mb.setText("Directory " + dirPath + " doesn't exist");
        mb.exec();
        return;
    }
    QStringList files = curDir.entryList(QDir::Files | QDir::NoSymLinks);
    QStringList dirs = curDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for(QString &file : files)
    {
        QString filePath(dirPath + "/" + file);
        foundedFiles.push_back({QFileInfo(filePath).size(), filePath});
    }
    for(QString &dir : dirs)
    {
        findAllFilesInDirectory(dirPath + "/" + dir);
    }
}

void MainWindow::distributeFilesEvenly()
{
    distributedFiles.resize(threadsCount);
    std::sort(foundedFiles.rbegin(), foundedFiles.rend());
    std::set<std::pair<quint64, quint8> > minCompletedThread;
    for(size_t i = 0; i < threadsCount; ++i)
    {
        minCompletedThread.insert({0, i});
    }
    for(std::pair<quint64, QString> &file : foundedFiles)
    {
        auto it = minCompletedThread.begin();
        std::pair<quint64, quint8> curThread = *it;
        minCompletedThread.erase(it);
        distributedFiles[curThread.second].push_back(file.second);
        minCompletedThread.insert({curThread.first + file.first, curThread.second});
    }
}

void MainWindow::deleteFileWithUniqSize()
{
    std::map<quint64, std::vector<QString> > mp;
    for(auto &file : foundedFiles)
    {
        if(file.first == 0)
        {
            hashedFiles[std::make_pair(0, "")].push_back(file.second);
            continue;
        }
        mp[file.first].push_back(file.second);
    }
    int cntGoodFiles = 0;
    for(auto &filesWithSameSize : mp)
    {
        if(filesWithSameSize.second.size() > 1)
        {
            for(QString& filePath : filesWithSameSize.second)
            {
                foundedFiles[cntGoodFiles++] = {filesWithSameSize.first, filePath};
                sizeOfAllFiles += filesWithSameSize.first;
            }
        }
    }
    foundedFiles.resize(cntGoodFiles);
}

void MainWindow::handleBlockOfFiles(const std::vector<QString>& block)
{
    QCryptographicHash hashMaker(QCryptographicHash::Keccak_224);
    std::vector<std::pair<std::pair<quint64, QByteArray>, QString> > hashedFilesInBlock;

    for(const QString& filePath : block) {
        QFile file(filePath);
        getHashOfFile(file, hashMaker);
        QByteArray hash = hashMaker.result();
        hashedFilesInBlock.push_back(std::make_pair(std::make_pair(file.size(), hash), filePath));
        hashMaker.reset();
    }

    mutex.lock();
    for(auto &files : hashedFilesInBlock) {
        hashedFiles[files.first].push_back(files.second);
    }
    mutex.unlock();
}

void MainWindow::fillTreeWidget()
{
    for(auto &file : hashedFiles)
    {
        if(file.second.size() == 1)
        {
            continue;
        }
        QTreeWidgetItem *rootItem = new QTreeWidgetItem(ui->treeWidget);
        rootItem->setFlags(rootItem->flags() & ~Qt::ItemIsSelectable);
        rootItem->setText(0, QString::number(file.second.size()) + " x (" + QString::number(file.first.first>>20) + ") MB");
        for(QString &path : file.second)
        {
            QTreeWidgetItem *childItem = new QTreeWidgetItem();
            childItem->setText(0, path);
            rootItem->addChild(childItem);
        }
        ui->treeWidget->addTopLevelItem(rootItem);
    }
}

void MainWindow::on_startScanning_clicked()
{
    foundedFiles.clear();
    hashedFiles.clear();
    ui->treeWidget->clear();
    sizeOfAllFiles = 0;
    progress = 0;
    distributedFiles.clear();


    ui->spinBox->setEnabled(false);
    ui->startScanning->setEnabled(false);
    ui->statusBar->showMessage("Hashing files in directory...");
    threadsCount = ui->spinBox->value();
    findAllFilesInDirectory(ui->directoryPath->text());
    deleteFileWithUniqSize();
    threadsCount = qMin(threadsCount, (quint8)foundedFiles.size());
    distributeFilesEvenly();

    if(sizeOfAllFiles != 0)
    {
        QProgressDialog pdialog;
        pdialog.setLabelText("Hashing a files...");
        pdialog.setRange(0, sizeOfAllFiles>>10);
        connect(&pdialog, SIGNAL(canceled()), &watcher, SLOT(cancel()));
        connect(&watcher, SIGNAL(finished()), &pdialog, SLOT(reset()));
        connect(&watcher, SIGNAL(progressValueChanged(int)), &pdialog, SLOT(setValue(int)));

        future = QtConcurrent::map(distributedFiles, &MainWindow::handleBlockOfFiles);
        watcher.setFuture(future);

        pdialog.exec();
        watcher.waitForFinished();

        if(watcher.isCanceled())
        {
            QMessageBox::critical(this, "Canceled", "Hashing has been canceled.");
        }
        else
        {
            ui->statusBar->showMessage("All files has been hashed.");
            fillTreeWidget();
            ui->statusBar->showMessage("Select the files you want to delete.");
        }
    }
    if(ui->treeWidget->topLevelItemCount() == 0)
    {
        ui->statusBar->showMessage("The same files were not found.");
    }
    ui->startScanning->setEnabled(true);
    ui->spinBox->setEnabled(true);
    ui->pushButton_2->setEnabled(true);
}

void MainWindow::on_pushButton_2_clicked()
{
    QList<QTreeWidgetItem *>items = ui->treeWidget->selectedItems(), unremovedFiles, removedFiles;
    if(items.size() == 0)
    {
        QMessageBox mb;
        mb.setText("Select any files to delete.");
        mb.exec();
        return;
    }

    QMessageBox messageBox;
    messageBox.setText("Are you sure you want to delete "
                       + QString::number(items.size())
                       + (items.size() > 1 ? " files?" : " file?"));
    messageBox.setStandardButtons(QMessageBox::Yes);
    messageBox.addButton(QMessageBox::No);
    messageBox.setDefaultButton(QMessageBox::No);
    if(messageBox.exec() == QMessageBox::No)
    {
        return;
    }

    //delete selected files
    for(QTreeWidgetItem* &item : items)
    {
        QFile file(item->text(0));
        if(!file.exists())
        {
            unremovedFiles.push_back(item);
            continue;
        }
        if(file.remove())
        {
            removedFiles.push_back(item);
        }
        else
        {
            unremovedFiles.push_back(item);
        }
    }

    //clear selection
    ui->treeWidget->clearSelection();

    //color green all deleted files
    for(QTreeWidgetItem* &item : removedFiles)
    {
        item->setBackgroundColor(0, QColor(0, 127, 0, 127));//green
    }
    //color red all failed files
    for(QTreeWidgetItem* &item : unremovedFiles)
    {
        item->setBackgroundColor(0, QColor(127, 0, 0, 127));//red
    }

    if(unremovedFiles.size() == 0)
    {
        QMessageBox mb;
        mb.setText("All files were successfully deleted.");
        mb.exec();
    }
    else
    {
        QMessageBox mb;
        mb.setText(QString::number(unremovedFiles.size())
                   + (unremovedFiles.size() > 1 ? " files weren't deleted." : " file wasn't deleted.")
                   + "");
        mb.exec();
    }
}
