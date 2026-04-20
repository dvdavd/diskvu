// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include <QByteArray>
#include <QStorageInfo>
#include <QStringView>

bool isLocalFilesystem(QStringView fileSystemType, const QByteArray& device, QStringView rootPath);
bool isLocalFilesystem(const QStorageInfo& storageInfo);
bool isLocalFilesystemPath(QStringView path);
