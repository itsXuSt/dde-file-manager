// SPDX-FileCopyrightText: 2021 - 2023 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "docopyfileworker.h"
#include "utils/fileutils.h"

#include <dfm-io/dfmio_utils.h>

#include <QDebug>
#include <QTime>
#include <QWaitCondition>
#include <QMutex>
#include <QThread>

#include <fcntl.h>
#include <zlib.h>
#include <sys/mman.h>

static const quint32 kMaxBufferLength { 1024 * 1024 * 1 };

DPFILEOPERATIONS_USE_NAMESPACE
USING_IO_NAMESPACE
DFMBASE_USE_NAMESPACE

DoCopyFileWorker::DoCopyFileWorker(const QSharedPointer<WorkerData> &data, QObject *parent)
    : QObject(parent), workData(data)
{
    waitCondition.reset(new QWaitCondition);
    mutex.reset(new QMutex);
    localFileHandler.reset(new LocalFileHandler);
}

DoCopyFileWorker::~DoCopyFileWorker()
{
}
// main thread using
void DoCopyFileWorker::pause()
{
    state = kPasued;
}
// main thread using
void DoCopyFileWorker::resume()
{
    state = kNormal;
    waitCondition->wakeAll();
}
// main thread using// main thread using
void DoCopyFileWorker::stop()
{
    state = kStoped;
    waitCondition->wakeAll();
}

void DoCopyFileWorker::skipMemcpyBigFile(const QUrl url)
{
    memcpySkipUrl = url;
}
// main thread using
void DoCopyFileWorker::operateAction(const AbstractJobHandler::SupportAction action)
{
    retry = !workData->signalThread && AbstractJobHandler::SupportAction::kRetryAction == action;
    currentAction = action;
    resume();
}
void DoCopyFileWorker::doFileCopy(const AbstractFileInfoPointer fromInfo, const AbstractFileInfoPointer toInfo)
{
    doCopyFilePractically(fromInfo, toInfo, nullptr);
    workData->completeFileCount++;
}

void DoCopyFileWorker::writeExblockFile()
{
    while (true) {
        if (isStopped())
            return;
        if (workData->blockCopyInfoQueue.size() <= 0) {
            QThread::msleep(10);
        } else if (!doWriteBlockFileCopy(workData->blockCopyInfoQueue.takeByLock())) {
            return;
        }
    }
}

void DoCopyFileWorker::doMemcpyLocalBigFile(const AbstractFileInfoPointer fromInfo, const AbstractFileInfoPointer toInfo, char *dest, char *source, size_t size)
{
    size_t copySize = size;
    char *destStart = dest;
    char *sourceStart = source;
    size_t everyCopySize = kMaxBufferLength;
    while (copySize > 0 && !isStopped()) {
        if (Q_UNLIKELY(!stateCheck())) {
            break;
        }
        everyCopySize = copySize >= everyCopySize ? everyCopySize : copySize;

        AbstractJobHandler::SupportAction action { AbstractJobHandler::SupportAction::kNoAction };

        do {
            if (!memcpy(destStart, sourceStart, everyCopySize)) {
                auto lastError = strerror(errno);
                qWarning() << "file memcpy error, url from: " << fromInfo->urlOf(UrlInfoType::kUrl)
                           << " url to: " << toInfo->urlOf(UrlInfoType::kUrl)
                           << " error code: " << errno << " error msg: " << lastError;

                action = doHandleErrorAndWait(fromInfo->urlOf(UrlInfoType::kUrl), toInfo->urlOf(UrlInfoType::kUrl),
                                              AbstractJobHandler::JobErrorType::kWriteError,
                                              true, lastError);
            }
        } while (action == AbstractJobHandler::SupportAction::kRetryAction && !isStopped());

        checkRetry();

        if (!actionOperating(action, static_cast<qint64>(copySize), nullptr)) {
            if (action == AbstractJobHandler::SupportAction::kSkipAction)
                emit skipCopyLocalBigFile(fromInfo->urlOf(UrlInfoType::kUrl));
            return;
        }

        copySize -= everyCopySize;
        destStart += everyCopySize;
        sourceStart += everyCopySize;

        if (memcpySkipUrl.isValid() && memcpySkipUrl == fromInfo->urlOf(UrlInfoType::kUrl))
            return;

        workData->currentWriteSize += static_cast<int64_t>(everyCopySize);
    }
}

bool DoCopyFileWorker::doWriteBlockFileCopy(const BlockFileCopyInfoPointer blockFileInfo)
{
    // write over flags
    if (!blockFileInfo->frominfo && !blockFileInfo->toinfo)
        return false;
    if (skipUrls.contains(blockFileInfo->frominfo->urlOf(UrlInfoType::kUrl)))
        return true;

    if (Q_UNLIKELY(!stateCheck())) {
        releaseCopyInfo(blockFileInfo);
        return false;
    }
    //检查info的有效性
    if (!blockFileInfo->frominfo || !blockFileInfo->toinfo) {
        releaseCopyInfo(blockFileInfo);
        return true;
    }
    //对文件夹加权
    if (blockFileInfo->isdir) {
        localFileHandler->setPermissions(blockFileInfo->toinfo->urlOf(UrlInfoType::kUrl), blockFileInfo->permission);
        return true;
    }

    // open file
    bool skip = false;
    if (blockFileFd < 0)
        blockFileFd = doOpenFile(blockFileInfo->frominfo, blockFileInfo->toinfo, true, blockFileInfo->openFlag, &skip);
    if (blockFileFd < 0) {
        if (skip)
            skipUrls.append(blockFileInfo->frominfo->urlOf(UrlInfoType::kUrl));
        return true;
    }
    // write data to block file
    if (!writeBlockFile(blockFileInfo, &skip)) {
        if (blockFileFd >= 0)
            close(blockFileFd);
        if (skip)
            skipUrls.append(blockFileInfo->frominfo->urlOf(UrlInfoType::kUrl));
        return true;
    }
    // sync to block file
    syncBlockFile(blockFileInfo);

    return true;
}

int DoCopyFileWorker::doOpenFile(const AbstractFileInfoPointer fromInfo, const AbstractFileInfoPointer toInfo, const bool isTo, const int openFlag, bool *skip)
{
    AbstractJobHandler::SupportAction action { AbstractJobHandler::SupportAction::kNoAction };
    emit currentTask(fromInfo->urlOf(UrlInfoType::kUrl), toInfo->urlOf(UrlInfoType::kUrl));
    int fd = -1;
    do {
        QUrl url = isTo ? toInfo->urlOf(UrlInfoType::kUrl) : fromInfo->urlOf(UrlInfoType::kUrl);
        std::string path = url.path().toStdString();
        fd = open(path.c_str(), openFlag, 0777);
        if (fd < 0) {
            auto lastError = strerror(errno);
            qWarning() << "file open error, url from: " << fromInfo->urlOf(UrlInfoType::kUrl)
                       << " url to: " << fromInfo->urlOf(UrlInfoType::kUrl) << " open flag: " << openFlag
                       << " error code: " << errno << " error msg: " << lastError;

            action = doHandleErrorAndWait(fromInfo->urlOf(UrlInfoType::kUrl), toInfo->urlOf(UrlInfoType::kUrl),
                                          AbstractJobHandler::JobErrorType::kOpenError, isTo, lastError);
        }
    } while (action == AbstractJobHandler::SupportAction::kRetryAction && !isStopped());

    checkRetry();

    if (!actionOperating(action, fromInfo->size() <= 0 ? FileUtils::getMemoryPageSize() : fromInfo->size(), skip)) {
        if (fd >= 0)
            close(fd);
        return -1;
    }

    return fd;
}

BlockFileCopyInfoPointer DoCopyFileWorker::doReadExBlockFile(const AbstractFileInfoPointer fromInfo, const AbstractFileInfoPointer toInfo, const int fd, bool *skip)
{
    qint64 sizeBlock = fromInfo->size() > kMaxBufferLength ? kMaxBufferLength : fromInfo->size();
    BlockFileCopyInfoPointer copyinfo(new WorkerData::BlockFileCopyInfo());
    copyinfo->frominfo = fromInfo;
    copyinfo->toinfo = toInfo;
    lseek(fd, 0, SEEK_SET);
    qint64 currentPos = 0;
    qint64 readSize = -1;
    while (true) {
        copyinfo->currentpos = currentPos;

        if (Q_UNLIKELY(!stateCheck())) {
            close(fd);
            return copyinfo;
        }
        char *buffer = new char[static_cast<uint>(sizeBlock + 1)];
        AbstractJobHandler::SupportAction actionForRead = AbstractJobHandler::SupportAction::kNoAction;
        do {
            readSize = read(fd, buffer, static_cast<size_t>(sizeBlock));
            auto laststr = strerror(errno);
            if (Q_UNLIKELY(!stateCheck())) {
                delete[] buffer;
                close(fd);
                return copyinfo;
            }
            if (Q_UNLIKELY(readSize <= 0)) {
                // read over
                if (readSize == 0 && currentPos == fromInfo->size()) {
                    copyinfo->buffer = buffer;
                    copyinfo->size = readSize;
                    close(fd);
                    return copyinfo;
                }

                qWarning() << "read size <=0, size: " << readSize << " from file pos: " << currentPos << " from file info size: " << fromInfo->size();

                const bool fromInfoExist = fromInfo->exists();
                AbstractJobHandler::JobErrorType errortype = fromInfoExist ? AbstractJobHandler::JobErrorType::kReadError : AbstractJobHandler::JobErrorType::kNonexistenceError;
                QString errorstr = fromInfoExist ? laststr : QString();

                actionForRead = doHandleErrorAndWait(fromInfo->urlOf(UrlInfoType::kUrl), toInfo->urlOf(UrlInfoType::kUrl),
                                                     errortype, false, errorstr);

                if (actionForRead == AbstractJobHandler::SupportAction::kRetryAction) {
                    if (!lseek(fd, currentPos, SEEK_SET)) {
                        // 优先处理
                        auto laststr = strerror(errno);
                        AbstractJobHandler::SupportAction actionForReadSeek =
                                doHandleErrorAndWait(fromInfo->urlOf(UrlInfoType::kUrl),
                                                     toInfo->urlOf(UrlInfoType::kUrl),
                                                     AbstractJobHandler::JobErrorType::kSeekError,
                                                     false, laststr);
                        checkRetry();
                        actionOperating(actionForReadSeek, fromInfo->size() <= 0 ? FileUtils::getMemoryPageSize() : fromInfo->size(), skip);

                        close(fd);

                        return copyinfo;
                    }
                }
            }
        } while (actionForRead == AbstractJobHandler::SupportAction::kRetryAction && !isStopped());

        checkRetry();

        if (!actionOperating(actionForRead, fromInfo->size() <= 0 ? FileUtils::getMemoryPageSize() : fromInfo->size(), skip)) {
            close(fd);
            return copyinfo;
        }

        createExBlockFileCopyInfo(copyinfo->frominfo, copyinfo->toinfo, currentPos, false, readSize, buffer);

        currentPos += readSize;
    }
}

void DoCopyFileWorker::createExBlockFileCopyInfo(const AbstractFileInfoPointer fromInfo, const AbstractFileInfoPointer toInfo, const qint64 currentPos, const bool closeFlag, const qint64 size, char *buffer, const bool isDir, const QFileDevice::Permissions permission)
{
    BlockFileCopyInfoPointer tmpinfo(new WorkerData::BlockFileCopyInfo());
    tmpinfo->closeflag = closeFlag;
    tmpinfo->frominfo = fromInfo;
    tmpinfo->toinfo = toInfo;
    tmpinfo->currentpos = currentPos;
    tmpinfo->buffer = buffer;
    tmpinfo->size = size;
    tmpinfo->isdir = isDir;
    tmpinfo->permission = permission;
    workData->blockCopyInfoQueue.push_backByLock(tmpinfo);
    while (workData->blockCopyInfoQueue.count() > 500 && !isStopped())
        QThread::msleep(10);
}

// copy thread using
bool DoCopyFileWorker::doCopyFilePractically(const AbstractFileInfoPointer fromInfo, const AbstractFileInfoPointer toInfo, bool *skip)
{
    if (isStopped())
        return false;
    // emit current task url
    emit currentTask(fromInfo->urlOf(UrlInfoType::kUrl), toInfo->urlOf(UrlInfoType::kUrl));
    // read ahead source file
    readAheadSourceFile(fromInfo);
    // 创建文件的divice
    QSharedPointer<DFMIO::DFile> fromDevice { nullptr }, toDevice { nullptr };
    if (!createFileDevices(fromInfo, toInfo, fromDevice, toDevice, skip))
        return false;
    // 打开文件并创建
    if (!openFiles(fromInfo, toInfo, fromDevice, toDevice, skip))
        return false;
    // 源文件大小如果为0
    if (fromInfo->size() <= 0) {
        // 对文件加权
        setTargetPermissions(fromInfo, toInfo);
        workData->zeroOrlinkOrDirWriteSize += FileUtils::getMemoryPageSize();
        FileUtils::notifyFileChangeManual(DFMBASE_NAMESPACE::Global::FileNotifyType::kFileAdded, toInfo->urlOf(UrlInfoType::kUrl));
        return true;
    }
    // resize target file
    if (workData->jobFlags.testFlag(AbstractJobHandler::JobFlag::kCopyResizeDestinationFile) && !resizeTargetFile(fromInfo, toInfo, toDevice, skip))
        return false;
    // 循环读取和写入文件，拷贝
    qint64 blockSize = fromInfo->size() > kMaxBufferLength ? kMaxBufferLength : fromInfo->size();
    char *data = new char[static_cast<uint>(blockSize + 1)];
    uLong sourceCheckSum = adler32(0L, nullptr, 0);
    qint64 sizeRead = 0;

    do {
        if (!doReadFile(fromInfo, toInfo, fromDevice, data, blockSize, sizeRead, skip)) {
            delete[] data;
            data = nullptr;
            return false;
        }

        if (!doWriteFile(fromInfo, toInfo, toDevice, data, sizeRead, skip)) {
            delete[] data;
            data = nullptr;
            return false;
        }

        if (Q_LIKELY(workData->jobFlags.testFlag(AbstractJobHandler::JobFlag::kCopyIntegrityChecking))) {
            sourceCheckSum = adler32(sourceCheckSum, reinterpret_cast<Bytef *>(data), static_cast<uInt>(sizeRead));
        }

        toInfo->cacheAttribute(DFMIO::DFileInfo::AttributeID::kStandardSize, toDevice->size());

    } while (fromDevice->pos() != fromInfo->size());

    delete[] data;
    data = nullptr;

    // 对文件加权
    setTargetPermissions(fromInfo, toInfo);
    if (!stateCheck())
        return false;

    // 校验文件完整性
    if (skip)
        *skip = verifyFileIntegrity(blockSize, sourceCheckSum, fromInfo, toInfo, toDevice);
    toInfo->refresh();

    if (skip && *skip)
        FileUtils::notifyFileChangeManual(DFMBASE_NAMESPACE::Global::FileNotifyType::kFileAdded, toInfo->urlOf(UrlInfoType::kUrl));

    return true;
}

void DoCopyFileWorker::readExblockFile(const AbstractFileInfoPointer fromInfo, const AbstractFileInfoPointer toInfo)
{
    if (!stateCheck())
        return;
    bool skip { false };
    auto fd = doOpenFile(fromInfo, toInfo, false, O_RDONLY, &skip);
    if (fd < 0)
        return;

    auto info = doReadExBlockFile(fromInfo, toInfo, fd, &skip);

    if (!stateCheck())
        return;

    workData->blockCopyInfoQueue.push_backByLock(info);
}

bool DoCopyFileWorker::stateCheck()
{
    if (state == kPasued)
        workerWait();
    return state == kNormal;
}

void DoCopyFileWorker::workerWait()
{
    waitCondition->wait(mutex.data());
    mutex->unlock();
}

bool DoCopyFileWorker::actionOperating(const AbstractJobHandler::SupportAction action, const qint64 size, bool *skip)
{
    if (isStopped())
        return false;

    if (action != AbstractJobHandler::SupportAction::kNoAction) {
        if (action == AbstractJobHandler::SupportAction::kSkipAction) {
            if (skip)
                *skip = true;
            workData->skipWriteSize += size;
        }
        return false;
    }

    return true;
}
/*!
 * \brief DoCopyFileWorker::doHandleErrorAndWait Handle the error and block waiting for the error handling operation to return
 * \param fromUrl URL of the source file
 * \param toUrl Destination URL
 * \param errorUrl URL of the error file
 * \param error Wrong type
 * \param errorMsg Wrong message
 * \return AbstractJobHandler::SupportAction Current processing operation
 */
AbstractJobHandler::SupportAction DoCopyFileWorker::doHandleErrorAndWait(const QUrl &urlFrom, const QUrl &urlTo,
                                                                         const AbstractJobHandler::JobErrorType &error,
                                                                         const bool isTo,
                                                                         const QString &errorMsg)
{
    if (workData->errorOfAction.contains(error))
        return workData->errorOfAction.value(error);

    if (FileUtils::isSameFile(urlFrom, urlTo, false)) {
        currentAction = AbstractJobHandler::SupportAction::kCoexistAction;
        return currentAction;
    }

    if (isStopped())
        return AbstractJobHandler::SupportAction::kCancelAction;

    // 发送错误处理 阻塞自己
    emit errorNotify(urlFrom, urlTo, error, isTo, quintptr(this), errorMsg, false);
    workerWait();

    if (isStopped())
        return AbstractJobHandler::SupportAction::kCancelAction;

    return currentAction;
}

/*!
 * \brief FileOperateBaseWorker::readAheadSourceFile Pre read source file content
 * \param fileInfo File information of source file
 */
void DoCopyFileWorker::readAheadSourceFile(const AbstractFileInfoPointer &fileInfo)
{
    if (fileInfo->size() <= 0)
        return;
    std::string stdStr = fileInfo->urlOf(UrlInfoType::kUrl).path().toUtf8().toStdString();
    int fromfd = open(stdStr.data(), O_RDONLY);
    if (-1 != fromfd) {
        readahead(fromfd, 0, static_cast<size_t>(fileInfo->size()));
        close(fromfd);
    }
}
/*!
 * \brief FileOperateBaseWorker::createFileDevice Device to create the file
 * \param fromUrl URL of the source file
 * \param toUrl Destination URL
 * \param needOpenInfo file information
 * \param file fromeFile Output parameter: file device
 * \param result result result Output parameter: whether skip
 * \return Is the device of the file created successfully
 */
bool DoCopyFileWorker::createFileDevice(const AbstractFileInfoPointer &fromInfo, const AbstractFileInfoPointer &toInfo,
                                        const AbstractFileInfoPointer &needOpenInfo, QSharedPointer<DFMIO::DFile> &file,
                                        bool *skip)
{
    file.reset();
    QUrl url = needOpenInfo->urlOf(UrlInfoType::kUrl);
    AbstractJobHandler::SupportAction action = AbstractJobHandler::SupportAction::kNoAction;

    do {
        file.reset(new DFile(url));
        if (!file) {
            qCritical() << "create dfm io dfile failed! url = " << url;
            action = doHandleErrorAndWait(fromInfo->urlOf(UrlInfoType::kUrl), toInfo->urlOf(UrlInfoType::kUrl),
                                          AbstractJobHandler::JobErrorType::kProrogramError,
                                          url == toInfo->urlOf(UrlInfoType::kUrl));
        }
    } while (action == AbstractJobHandler::SupportAction::kRetryAction && !isStopped());

    checkRetry();

    if (!actionOperating(action, fromInfo->size() <= 0 ? workData->dirSize : fromInfo->size(), skip))
        return false;

    return true;
}
/*!
 * \brief DoCopyFileWorker::createFileDevices Device for creating source and directory files
 * \param fromInfo File information of source file
 * \param toInfo File information of target file
 * \param fromeFile Output parameter: device of source file
 * \param toFile Output parameter: device of target file
 * \param result result Output parameter: whether skip
 * \return Whether the device of source file and target file is created successfully
 */
bool DoCopyFileWorker::createFileDevices(const AbstractFileInfoPointer &fromInfo, const AbstractFileInfoPointer &toInfo,
                                         QSharedPointer<DFMIO::DFile> &fromeFile, QSharedPointer<DFMIO::DFile> &toFile, bool *skip)
{
    if (!createFileDevice(fromInfo, toInfo, fromInfo, fromeFile, skip))
        return false;
    if (!createFileDevice(fromInfo, toInfo, toInfo, toFile, skip))
        return false;
    return true;
}

/*!
 * \brief FileOperateBaseWorker::openFiles Open source and destination files
 * \param fromInfo File information of source file
 * \param toInfo File information of target file
 * \param fromeFile device of source file
 * \param toFile device of target file
 * \param result result Output parameter: whether skip
 * \return Open source and target files successfully
 */
bool DoCopyFileWorker::openFiles(const AbstractFileInfoPointer &fromInfo, const AbstractFileInfoPointer &toInfo,
                                 const QSharedPointer<DFMIO::DFile> &fromeFile, const QSharedPointer<DFMIO::DFile> &toFile,
                                 bool *skip)
{
    if (fromInfo->size() > 0 && !openFile(fromInfo, toInfo, fromeFile, DFMIO::DFile::OpenFlag::kReadOnly, skip)) {
        return false;
    }

    if (!openFile(fromInfo, toInfo, toFile, DFMIO::DFile::OpenFlag::kWriteOnly | DFMIO::DFile::OpenFlag::kTruncate, skip)) {
        return false;
    }

    return true;
}

/*!
 * \brief FileOperateBaseWorker::openFile
 * \param fromUrl URL of the source file
 * \param toUrl Destination URL
 * \param fileInfo file information
 * \param file file deivce
 * \param flags Flag for opening file
 * \param result result Output parameter: whether skip
 * \return wether open the file successfully
 */
bool DoCopyFileWorker::openFile(const AbstractFileInfoPointer &fromInfo, const AbstractFileInfoPointer &toInfo,
                                const QSharedPointer<DFMIO::DFile> &file, const DFMIO::DFile::OpenFlags &flags,
                                bool *skip)
{
    AbstractJobHandler::SupportAction action = AbstractJobHandler::SupportAction::kNoAction;
    do {
        if (!file->open(flags)) {
            auto lastError = file->lastError();
            qWarning() << "file open error, url from: " << fromInfo->urlOf(UrlInfoType::kUrl)
                       << " url to: " << toInfo->urlOf(UrlInfoType::kUrl) << " open flag: " << flags
                       << " error code: " << lastError.code() << " error msg: " << lastError.errorMsg();

            action = doHandleErrorAndWait(fromInfo->urlOf(UrlInfoType::kUrl), toInfo->urlOf(UrlInfoType::kUrl),
                                          AbstractJobHandler::JobErrorType::kOpenError,
                                          file->uri() != fromInfo->urlOf(UrlInfoType::kUrl), lastError.errorMsg());
        }
    } while (action == AbstractJobHandler::SupportAction::kRetryAction && !isStopped());

    checkRetry();

    if (!actionOperating(action, fromInfo->size() <= 0 ? FileUtils::getMemoryPageSize() : fromInfo->size(), skip))
        return false;
    return true;
}

bool DoCopyFileWorker::resizeTargetFile(const AbstractFileInfoPointer &fromInfo, const AbstractFileInfoPointer &toInfo,
                                        const QSharedPointer<DFMIO::DFile> &file, bool *skip)
{
    AbstractJobHandler::SupportAction action = AbstractJobHandler::SupportAction::kNoAction;
    do {
        if (!file->write(QByteArray())) {
            action = doHandleErrorAndWait(fromInfo->urlOf(UrlInfoType::kUrl), toInfo->urlOf(UrlInfoType::kUrl),
                                          AbstractJobHandler::JobErrorType::kResizeError, true,
                                          file->lastError().errorMsg());
        }
    } while (action == AbstractJobHandler::SupportAction::kRetryAction && !isStopped());
    if (!actionOperating(action, fromInfo->size() <= 0 ? workData->dirSize : fromInfo->size(), skip))
        return false;
    return true;
}

/*!
 * \brief FileOperateBaseWorker::doReadFile  Read file contents
 * \param fromUrl URL of the source file
 * \param toUrl Destination URL
 * \param fileInfo file information
 * \param fromDevice file device
 * \param data Data buffer
 * \param blockSize Data buffer size
 * \param readSize Read size
 * \param result result Output parameter: whether skip
 * \return Read successfully
 */
bool DoCopyFileWorker::doReadFile(const AbstractFileInfoPointer &fromInfo, const AbstractFileInfoPointer &toInfo,
                                  const QSharedPointer<DFMIO::DFile> &fromDevice,
                                  char *data, const qint64 &blockSize,
                                  qint64 &readSize, bool *skip)
{
    readSize = 0;
    qint64 currentPos = fromDevice->pos();
    AbstractJobHandler::SupportAction actionForRead = AbstractJobHandler::SupportAction::kNoAction;

    if (Q_UNLIKELY(!stateCheck())) {
        return false;
    }
    do {
        readSize = fromDevice->read(data, blockSize);
        if (Q_UNLIKELY(!stateCheck())) {
            return false;
        }

        if (Q_UNLIKELY(readSize <= 0)) {

            const qint64 fromFilePos = fromDevice->pos();
            const qint64 fromFileInfoSize = fromInfo->size();
            if (readSize == 0 && fromFilePos == fromFileInfoSize) {
                return true;
            }

            qWarning() << "read size <=0, size: " << readSize << " from file pos: " << fromFilePos << " from file info size: " << fromFileInfoSize;
            const bool fromInfoExist = fromInfo->exists();
            AbstractJobHandler::JobErrorType errortype = fromInfoExist ? AbstractJobHandler::JobErrorType::kReadError : AbstractJobHandler::JobErrorType::kNonexistenceError;
            QString errorstr = fromInfoExist ? fromDevice->lastError().errorMsg() : QString();

            actionForRead = doHandleErrorAndWait(fromInfo->urlOf(UrlInfoType::kUrl),
                                                 toInfo->urlOf(UrlInfoType::kUrl), errortype, false, errorstr);

            if (actionForRead == AbstractJobHandler::SupportAction::kRetryAction) {
                if (!fromDevice->seek(currentPos)) {
                    // 优先处理
                    AbstractJobHandler::SupportAction actionForReadSeek = doHandleErrorAndWait(fromInfo->urlOf(UrlInfoType::kUrl),
                                                                                               toInfo->urlOf(UrlInfoType::kUrl),
                                                                                               AbstractJobHandler::JobErrorType::kSeekError,
                                                                                               false, fromDevice->lastError().errorMsg());
                    checkRetry();
                    if (!actionOperating(actionForReadSeek, fromInfo->size() - currentPos, skip))
                        return false;
                    return false;
                }
            }
        }
    } while (actionForRead == AbstractJobHandler::SupportAction::kRetryAction && !isStopped());
    checkRetry();

    if (!actionOperating(actionForRead, fromInfo->size() - currentPos, skip))
        return false;

    return true;
}

/*!
 * \brief FileOperateBaseWorker::doWriteFile  Write file contents
 * \param fromUrl URL of the source file
 * \param toUrl Destination URL
 * \param fileInfo file information
 * \param fromDevice file device
 * \param data Data buffer
 * \param blockSize Data buffer size
 * \param readSize Write size
 * \param result result Output parameter: whether skip
 * \return Write successfully
 */
bool DoCopyFileWorker::doWriteFile(const AbstractFileInfoPointer &fromInfo, const AbstractFileInfoPointer &toInfo,
                                   const QSharedPointer<DFMIO::DFile> &toDevice,
                                   const char *data, const qint64 readSize, bool *skip)
{
    qint64 currentPos = toDevice->pos();
    AbstractJobHandler::SupportAction actionForWrite { AbstractJobHandler::SupportAction::kNoAction };
    qint64 sizeWrite = 0;
    qint64 surplusSize = readSize;

    do {
        bool writeFinishedOnce = true;
        const char *surplusData = data;
        do {
            if (!writeFinishedOnce)
                qDebug() << "write not finished once, current write size: " << sizeWrite << " remain size: " << surplusSize - sizeWrite << " read size: " << readSize;
            surplusData += sizeWrite;
            surplusSize -= sizeWrite;
            sizeWrite = toDevice->write(surplusData, surplusSize);
            if (sizeWrite > 0)
                workData->currentWriteSize += sizeWrite;
            if (Q_UNLIKELY(!stateCheck()))
                return false;
            writeFinishedOnce = false;
        } while (sizeWrite > 0 && sizeWrite < surplusSize);

        // 表示全部数据写入完成
        if (sizeWrite >= 0)
            break;
        if (sizeWrite == -1 && toDevice->lastError().code() == DFMIOErrorCode::DFM_IO_ERROR_NONE) {
            qWarning() << "write failed, but no error, maybe write empty";
            break;
        }

        actionForWrite = doHandleErrorAndWait(fromInfo->urlOf(UrlInfoType::kUrl), toInfo->urlOf(UrlInfoType::kUrl),
                                              AbstractJobHandler::JobErrorType::kWriteError, true,
                                              toDevice->lastError().errorMsg());
        if (actionForWrite == AbstractJobHandler::SupportAction::kRetryAction) {
            if (!toDevice->seek(currentPos)) {
                AbstractJobHandler::SupportAction actionForWriteSeek = doHandleErrorAndWait(fromInfo->urlOf(UrlInfoType::kUrl), toInfo->urlOf(UrlInfoType::kUrl),
                                                                                            AbstractJobHandler::JobErrorType::kSeekError,
                                                                                            true, toDevice->lastError().errorMsg());
                checkRetry();
                actionOperating(actionForWriteSeek, fromInfo->size() - (currentPos + readSize - surplusSize), skip);
                return false;
            }
        }
    } while (actionForWrite == AbstractJobHandler::SupportAction::kRetryAction && !isStopped());

    checkRetry();

    if (!actionOperating(actionForWrite, fromInfo->size() - (currentPos + readSize - surplusSize), skip))
        return false;

    if (workData->needSyncEveryRW && sizeWrite > 0) {
        if (workData->isFsTypeVfat) {
            // FAT and VFAT file systems ignore the "sync" option
            toDevice->close();
            if (!openFile(fromInfo, toInfo, toDevice, DFMIO::DFile::OpenFlag::kWriteOnly | DFMIO::DFile::OpenFlag::kAppend, skip)) {
                return false;
            }
        } else {
            toDevice->flush();
        }
    }
    return true;
}

bool DoCopyFileWorker::verifyFileIntegrity(const qint64 &blockSize, const ulong &sourceCheckSum,
                                           const AbstractFileInfoPointer &fromInfo, const AbstractFileInfoPointer &toInfo,
                                           QSharedPointer<DFMIO::DFile> &toDevice)
{
    if (!workData->jobFlags.testFlag(AbstractJobHandler::JobFlag::kCopyIntegrityChecking))
        return true;
    char *data = new char[static_cast<uint>(blockSize + 1)];
    QTime t;
    ulong targetCheckSum = adler32(0L, nullptr, 0);
    Q_FOREVER {
        qint64 size = toDevice->read(data, blockSize);

        if (Q_UNLIKELY(size <= 0)) {
            if (size == 0 && toInfo->size() == toDevice->pos()) {
                break;
            }

            AbstractJobHandler::SupportAction actionForCheckRead = doHandleErrorAndWait(fromInfo->urlOf(UrlInfoType::kUrl),
                                                                                        toInfo->urlOf(UrlInfoType::kUrl),
                                                                                        AbstractJobHandler::JobErrorType::kIntegrityCheckingError,
                                                                                        true,
                                                                                        toDevice->lastError().errorMsg());
            if (!isStopped() && AbstractJobHandler::SupportAction::kRetryAction == actionForCheckRead) {
                continue;
            } else {
                checkRetry();
                return actionForCheckRead == AbstractJobHandler::SupportAction::kSkipAction;
            }
        }

        targetCheckSum = adler32(targetCheckSum, reinterpret_cast<Bytef *>(data), static_cast<uInt>(size));

        if (Q_UNLIKELY(!stateCheck())) {
            delete[] data;
            data = nullptr;
            return false;
        }
    }
    delete[] data;

    qDebug("Time spent of integrity check of the file: %d", t.elapsed());

    if (sourceCheckSum != targetCheckSum) {
        qWarning("Failed on file integrity checking, source file: 0x%lx, target file: 0x%lx", sourceCheckSum, targetCheckSum);
        AbstractJobHandler::SupportAction actionForCheck = doHandleErrorAndWait(fromInfo->urlOf(UrlInfoType::kUrl),
                                                                                toInfo->urlOf(UrlInfoType::kUrl),
                                                                                AbstractJobHandler::JobErrorType::kIntegrityCheckingError,
                                                                                true);
        return actionForCheck == AbstractJobHandler::SupportAction::kSkipAction;
    }

    return true;
}

void DoCopyFileWorker::checkRetry()
{
    if (!workData->signalThread && retry && !isStopped()) {
        retry = false;
        emit retryErrSuccess(quintptr(this));
    }
}

bool DoCopyFileWorker::isStopped()
{
    return state == kStoped;
}

void DoCopyFileWorker::releaseCopyInfo(const BlockFileCopyInfoPointer &info)
{
    if (info->buffer) {
        delete[] info->buffer;
        info->buffer = nullptr;
    }
}

bool DoCopyFileWorker::writeBlockFile(const BlockFileCopyInfoPointer &info, bool *skip)
{
    if (blockFileFd < 0)
        return false;

    if (Q_UNLIKELY(!stateCheck())) {
        releaseCopyInfo(info);
        return false;
    }
    if (info->size <= 0)
        return true;
    qint64 currentPos = info->currentpos;
    AbstractJobHandler::SupportAction actionForWrite { AbstractJobHandler::SupportAction::kNoAction };
    qint64 surplusSize = info->size;
    do {
        qint64 sizeWrite = 0;

        if (Q_UNLIKELY(!stateCheck())) {
            releaseCopyInfo(info);
            return false;
        }
        bool writeFinishedOnce = true;
        const char *surplusData = info->buffer;
        do {
            if (!writeFinishedOnce)
                qDebug() << "write not finished once, current write size: " << sizeWrite << " remain size: " << surplusSize - sizeWrite << " read size: " << info->size;
            surplusData += sizeWrite;
            surplusSize -= sizeWrite;
            sizeWrite = write(blockFileFd, surplusData, static_cast<size_t>(info->size));
            if (sizeWrite > 0)
                workData->currentWriteSize += sizeWrite;
            if (Q_UNLIKELY(!stateCheck()))
                return false;
            writeFinishedOnce = false;
        } while (sizeWrite > 0 && sizeWrite < surplusSize);

        // 表示全部数据写入完成
        if (sizeWrite >= 0)
            break;
        if (sizeWrite == -1 && errno == DFMIOErrorCode::DFM_IO_ERROR_NONE) {
            qWarning() << "write failed, but no error, maybe write empty";
            break;
        }
        auto lastError = strerror(errno);
        actionForWrite = doHandleErrorAndWait(info->frominfo->urlOf(UrlInfoType::kUrl),
                                              info->toinfo->urlOf(UrlInfoType::kUrl),
                                              AbstractJobHandler::JobErrorType::kWriteError, true, lastError);
        if (actionForWrite == AbstractJobHandler::SupportAction::kRetryAction) {
            if (!lseek(blockFileFd, currentPos, SEEK_SET)) {
                auto lastError = strerror(errno);
                AbstractJobHandler::SupportAction actionForWriteSeek = doHandleErrorAndWait(info->frominfo->urlOf(UrlInfoType::kUrl),
                                                                                            info->toinfo->urlOf(UrlInfoType::kUrl),
                                                                                            AbstractJobHandler::JobErrorType::kSeekError,
                                                                                            true, lastError);
                checkRetry();
                actionOperating(actionForWriteSeek, info->frominfo->size() - (currentPos + info->size - surplusSize), skip);
                return false;
            }
        }
    } while (actionForWrite == AbstractJobHandler::SupportAction::kRetryAction && !isStopped());

    checkRetry();

    if (!actionOperating(actionForWrite, info->frominfo->size() - (currentPos + info->size - surplusSize), skip))
        return false;

    return true;
}

void DoCopyFileWorker::syncBlockFile(const BlockFileCopyInfoPointer &info)
{
    if (stateCheck())
        syncfs(blockFileFd);

    //关闭文件并加权
    if (info->closeflag) {
        close(blockFileFd);
        // the file is empty
        if (info->frominfo->size() <= 0)
            workData->zeroOrlinkOrDirWriteSize += FileUtils::getMemoryPageSize();

        blockFileFd = -1;
        QFileDevice::Permissions permissions = info->frominfo->permissions();
        QString path = info->frominfo->urlOf(UrlInfoType::kUrl).path();
        if (permissions != 0000)
            localFileHandler->setPermissions(info->toinfo->urlOf(UrlInfoType::kUrl), permissions);
    }
}

/*!
 * \brief FileOperateBaseWorker::setTargetPermissions Set permissions on the target file
 * \param fromInfo File information of source file
 * \param toInfo File information of target file
 */
void DoCopyFileWorker::setTargetPermissions(const AbstractFileInfoPointer &fromInfo, const AbstractFileInfoPointer &toInfo)
{
    // 修改文件修改时间
    localFileHandler->setFileTime(toInfo->urlOf(UrlInfoType::kUrl), fromInfo->timeOf(TimeInfoType::kLastRead).value<QDateTime>(), fromInfo->timeOf(TimeInfoType::kLastModified).value<QDateTime>());
    QFileDevice::Permissions permissions = fromInfo->permissions();
    QString path = fromInfo->urlOf(UrlInfoType::kUrl).path();
    //权限为0000时，源文件已经被删除，无需修改新建的文件的权限为0000
    if (permissions != 0000)
        localFileHandler->setPermissions(toInfo->urlOf(UrlInfoType::kUrl), permissions);
}
