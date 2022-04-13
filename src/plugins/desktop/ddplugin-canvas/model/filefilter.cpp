/*
 * Copyright (C) 2022 Uniontech Software Technology Co., Ltd.
 *
 * Author:     zhangyu<zhangyub@uniontech.com>
 *
 * Maintainer: zhangyu<zhangyub@uniontech.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "filefilter.h"

#include <dfm-base/utils/fileutils.h>
#include <base/standardpaths.h>

#include <QGSettings>
#include <QDebug>

DFMBASE_USE_NAMESPACE
DDP_CANVAS_USE_NAMESPACE

FileFilter::FileFilter()
{

}

bool FileFilter::fileTraversalFilter(QList<QUrl> &urls)
{
    return false;
}

bool FileFilter::fileDeletedFilter(const QUrl &url)
{
    Q_UNUSED(url)
    return false;
}

bool FileFilter::fileCreatedFilter(const QUrl &url)
{
    Q_UNUSED(url)
    return false;
}

bool FileFilter::fileRenameFilter(const QUrl &oldUrl, const QUrl &newUrl)
{
    Q_UNUSED(oldUrl)
    Q_UNUSED(newUrl)
    return false;
}

bool FileFilter::fileUpdatedFilter(const QUrl &url)
{
    Q_UNUSED(url)
    return false;
}

