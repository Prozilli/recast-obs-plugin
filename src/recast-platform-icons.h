#pragma once

#include <QIcon>
#include <QString>

/* Detect platform from a streaming URL hostname */
QString recast_detect_platform(const QString &url);

/* Get an icon for a detected platform ID */
QIcon recast_platform_icon(const QString &platform_id);

/* Get a colored circle icon with a letter (fallback) */
QIcon recast_letter_icon(QChar letter, const QColor &bg_color);
