/*
 * Copyright (C) 2020 ~ 2021 Uniontech Software Technology Co., Ltd.
 *
 * Author:     yanghao<yanghao@uniontech.com>
 *
 * Maintainer: zhengyouge<zhengyouge@uniontech.com>
 *             yanghao<yanghao@uniontech.com>
 *             hujianzhong<hujianzhong@uniontech.com>
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
#ifndef PLUGINCONTEXT_H
#define PLUGINCONTEXT_H

#include "dfm-framework/service/pluginserviceglobal.h"
#include "dfm-framework/definitions/globaldefinitions.h"

#include <QMetaType>
#include <QMetaObject>
#include <QMetaClassInfo>
#include <QObject>
#include <QQueue>

DPF_BEGIN_NAMESPACE

class PluginContext : public QSharedData
{

public:
    explicit PluginContext(){}

    virtual ~PluginContext(){}

    template<class T>
    T *service(const QString &serviceName)
    {
        return GlobalPrivate::PluginServiceGlobal::findService<T>(serviceName);
    }

    QStringList services()
    {
        return GlobalPrivate::PluginServiceGlobal::getServices();
    }
};

DPF_END_NAMESPACE

#endif // PLUGINCONTEXT_H