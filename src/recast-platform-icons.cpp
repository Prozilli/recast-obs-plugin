/*
 * recast-platform-icons.cpp -- Platform detection and icon generation.
 *
 * Detects streaming platform from URL hostname and generates colored
 * circle icons with the platform's initial letter (no external PNGs needed).
 */

#include "recast-platform-icons.h"

#include <QColor>
#include <QPainter>
#include <QPixmap>
#include <QUrl>

struct PlatformDef {
	const char *hostname_contains;
	const char *platform_id;
	QChar letter;
	QColor color;
};

static const PlatformDef platforms[] = {
	{"twitch.tv",     "twitch",   'T', QColor(145, 70, 255)},
	{"youtube.com",   "youtube",  'Y', QColor(255, 0, 0)},
	{"youtu.be",      "youtube",  'Y', QColor(255, 0, 0)},
	{"tiktok",        "tiktok",   'K', QColor(0, 0, 0)},
	{"kick.com",      "kick",     'K', QColor(83, 252, 24)},
	{"a]]kick",       "kick",     'K', QColor(83, 252, 24)},
	{"facebook.com",  "facebook", 'F', QColor(24, 119, 242)},
	{"fb.gg",         "facebook", 'F', QColor(24, 119, 242)},
	{"pscp.tv",       "x",        'X', QColor(0, 0, 0)},
	{"twitter.com",   "x",        'X', QColor(0, 0, 0)},
	{"x.com",         "x",        'X', QColor(0, 0, 0)},
	{"trovo.live",    "trovo",    'V', QColor(25, 202, 147)},
	{"instagram.com", "instagram",'I', QColor(225, 48, 108)},
	{nullptr, nullptr, ' ', QColor()}
};

QString recast_detect_platform(const QString &url)
{
	QString host = QUrl(url).host().toLower();
	if (host.isEmpty()) {
		/* Try raw string match for non-standard URLs */
		QString lower = url.toLower();
		for (const PlatformDef *p = platforms; p->hostname_contains; p++) {
			if (lower.contains(QString::fromUtf8(p->hostname_contains)))
				return QString::fromUtf8(p->platform_id);
		}
		return QString();
	}

	for (const PlatformDef *p = platforms; p->hostname_contains; p++) {
		if (host.contains(QString::fromUtf8(p->hostname_contains)))
			return QString::fromUtf8(p->platform_id);
	}

	return QString();
}

QIcon recast_letter_icon(QChar letter, const QColor &bg_color)
{
	int size = 20;
	QPixmap pm(size, size);
	pm.fill(Qt::transparent);

	QPainter painter(&pm);
	painter.setRenderHint(QPainter::Antialiasing, true);

	/* Draw filled circle */
	painter.setBrush(bg_color);
	painter.setPen(Qt::NoPen);
	painter.drawEllipse(0, 0, size, size);

	/* Draw letter */
	painter.setPen(Qt::white);
	QFont font = painter.font();
	font.setPixelSize(12);
	font.setBold(true);
	painter.setFont(font);
	painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter,
			 QString(letter));

	painter.end();
	return QIcon(pm);
}

QIcon recast_platform_icon(const QString &platform_id)
{
	for (const PlatformDef *p = platforms; p->hostname_contains; p++) {
		if (QString::fromUtf8(p->platform_id) == platform_id)
			return recast_letter_icon(p->letter, p->color);
	}

	/* Unknown platform: gray circle with '?' */
	return recast_letter_icon('?', QColor(128, 128, 128));
}
