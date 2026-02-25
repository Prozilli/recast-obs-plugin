/*
 * recast-chat.cpp -- Unified multi-platform chat for Recast plugin.
 *
 * Provides Twitch (IRC/WS), YouTube (polling), and Kick (Pusher/WS)
 * chat providers, plus a combined dock widget that displays messages
 * from all connected platforms.
 */

#include "recast-chat.h"
#include "recast-auth.h"
#include "recast-platform-icons.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QUrlQuery>
#include <QScrollBar>
#include <QStyle>

extern "C" {
#include <obs-module.h>
}

/* ====================================================================
 * RecastTwitchChat -- IRC over WebSocket
 * ==================================================================== */

RecastTwitchChat::RecastTwitchChat(QObject *parent)
	: RecastChatProvider(parent)
{
	ws_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest,
			     this);
	reconnect_timer_ = new QTimer(this);
	reconnect_timer_->setSingleShot(true);

	connect(ws_, &QWebSocket::connected,
		this, &RecastTwitchChat::onConnected);
	connect(ws_, &QWebSocket::disconnected,
		this, &RecastTwitchChat::onDisconnected);
	connect(ws_, &QWebSocket::textMessageReceived,
		this, &RecastTwitchChat::onTextMessageReceived);
	connect(reconnect_timer_, &QTimer::timeout,
		this, &RecastTwitchChat::onReconnectTimer);
}

RecastTwitchChat::~RecastTwitchChat()
{
	disconnect();
}

void RecastTwitchChat::connectToChat(const QString &channel)
{
	channel_ = channel.toLower();
	if (channel_.startsWith('#'))
		channel_ = channel_.mid(1);

	blog(LOG_INFO, "[Recast Chat] Twitch connecting to #%s",
	     channel_.toUtf8().constData());

	ws_->open(QUrl(QStringLiteral(
		"wss://irc-ws.chat.twitch.tv:443")));
}

void RecastTwitchChat::disconnect()
{
	reconnect_timer_->stop();

	if (ws_->state() != QAbstractSocket::UnconnectedState) {
		ws_->close();
	}

	if (connected_) {
		connected_ = false;
		emit connectionStateChanged(false);
	}
}

void RecastTwitchChat::sendMessage(const QString &msg)
{
	if (!connected_ || channel_.isEmpty())
		return;

	QString line = QStringLiteral("PRIVMSG #%1 :%2")
		.arg(channel_, msg);
	ws_->sendTextMessage(line);
}

bool RecastTwitchChat::isConnected() const
{
	return connected_;
}

void RecastTwitchChat::onConnected()
{
	blog(LOG_INFO, "[Recast Chat] Twitch WebSocket connected");

	auto *auth = RecastAuthManager::instance();

	/* Request IRCv3 capabilities for tags, membership, commands */
	ws_->sendTextMessage(QStringLiteral(
		"CAP REQ :twitch.tv/membership twitch.tv/tags "
		"twitch.tv/commands"));

	/* Authenticate */
	QString token = auth->accessToken(QStringLiteral("twitch"));
	QString nick = auth->userName(QStringLiteral("twitch"));

	if (!token.isEmpty()) {
		ws_->sendTextMessage(
			QStringLiteral("PASS oauth:%1").arg(token));
	}

	if (!nick.isEmpty()) {
		ws_->sendTextMessage(
			QStringLiteral("NICK %1").arg(nick.toLower()));
	} else {
		/* Anonymous connection */
		ws_->sendTextMessage(
			QStringLiteral("NICK justinfan12345"));
	}

	/* Join channel */
	ws_->sendTextMessage(
		QStringLiteral("JOIN #%1").arg(channel_));

	connected_ = true;
	emit connectionStateChanged(true);
}

void RecastTwitchChat::onDisconnected()
{
	blog(LOG_INFO, "[Recast Chat] Twitch WebSocket disconnected");

	if (connected_) {
		connected_ = false;
		emit connectionStateChanged(false);
	}

	/* Schedule reconnect */
	if (!channel_.isEmpty()) {
		reconnect_timer_->start(5000);
	}
}

void RecastTwitchChat::onTextMessageReceived(const QString &raw)
{
	/* Twitch may send multiple lines in one message */
	QStringList lines = raw.split(QStringLiteral("\r\n"),
				      Qt::SkipEmptyParts);
	for (const QString &line : lines)
		parseLine(line);
}

void RecastTwitchChat::onReconnectTimer()
{
	if (!channel_.isEmpty() && !connected_) {
		blog(LOG_INFO, "[Recast Chat] Twitch reconnecting...");
		connectToChat(channel_);
	}
}

void RecastTwitchChat::parseLine(const QString &line)
{
	/* Reject absurdly long lines to prevent memory issues */
	if (line.length() > 16384)
		return;

	/* Handle PING keepalive */
	if (line.startsWith(QStringLiteral("PING"))) {
		QString pong = line;
		pong.replace(0, 4, QStringLiteral("PONG"));
		ws_->sendTextMessage(pong);
		return;
	}

	/*
	 * IRC message format with tags:
	 * @tags :prefix COMMAND params :trailing
	 *
	 * Example PRIVMSG:
	 * @badge-info=;badges=broadcaster/1;color=#FF0000;
	 *  display-name=User;emotes=;...;user-type=
	 *  :user!user@user.tmi.twitch.tv PRIVMSG #channel :Hello World
	 */

	QString tags_str;
	QString remainder = line;

	/* Extract tags (starts with @) */
	if (remainder.startsWith('@')) {
		int space = remainder.indexOf(' ');
		if (space < 0)
			return;
		tags_str = remainder.mid(1, space - 1);
		remainder = remainder.mid(space + 1);
	}

	/* Extract prefix (starts with :) */
	QString prefix;
	if (remainder.startsWith(':')) {
		int space = remainder.indexOf(' ');
		if (space < 0)
			return;
		prefix = remainder.mid(1, space - 1);
		remainder = remainder.mid(space + 1);
	}

	/* Extract command */
	int space = remainder.indexOf(' ');
	QString command;
	QString params;
	if (space >= 0) {
		command = remainder.left(space);
		params = remainder.mid(space + 1);
	} else {
		command = remainder;
	}

	if (command == QStringLiteral("PRIVMSG")) {
		/* Extract trailing message after the second ':' */
		int colon = params.indexOf(':');
		if (colon < 0)
			return;
		QString trailing = params.mid(colon + 1);

		RecastChatMessage msg =
			parsePrivmsg(tags_str, prefix, trailing);
		emit messageReceived(msg);
	}
}

RecastChatMessage RecastTwitchChat::parsePrivmsg(const QString &tags,
						 const QString &prefix,
						 const QString &trailing)
{
	RecastChatMessage msg;
	msg.platform = QStringLiteral("twitch");
	msg.message = trailing;
	msg.timestamp = QDateTime::currentMSecsSinceEpoch();

	/* Extract username from prefix (user!user@user.tmi.twitch.tv) */
	int excl = prefix.indexOf('!');
	if (excl > 0)
		msg.username = prefix.left(excl);

	/* Parse IRCv3 tags: semicolon-separated key=value pairs */
	QStringList tag_list = tags.split(';', Qt::SkipEmptyParts);
	for (const QString &tag : tag_list) {
		int eq = tag.indexOf('=');
		if (eq < 0)
			continue;

		QString key = tag.left(eq);
		QString value = tag.mid(eq + 1);

		if (key == QStringLiteral("display-name")) {
			msg.displayName = value;
		} else if (key == QStringLiteral("color")) {
			if (!value.isEmpty())
				msg.nameColor = QColor(value);
		} else if (key == QStringLiteral("mod")) {
			msg.isMod = (value == QStringLiteral("1"));
		} else if (key == QStringLiteral("subscriber")) {
			msg.isSub = (value == QStringLiteral("1"));
		} else if (key == QStringLiteral("badges")) {
			if (value.contains(QStringLiteral("broadcaster")))
				msg.isOwner = true;
		}
	}

	/* Fallback: use username if display-name was empty */
	if (msg.displayName.isEmpty())
		msg.displayName = msg.username;

	/* Default color if none was set */
	if (!msg.nameColor.isValid())
		msg.nameColor = QColor(145, 70, 255);

	return msg;
}

/* ====================================================================
 * RecastYouTubeChat -- REST polling
 * ==================================================================== */

RecastYouTubeChat::RecastYouTubeChat(QObject *parent)
	: RecastChatProvider(parent)
{
	net_ = new QNetworkAccessManager(this);
	poll_timer_ = new QTimer(this);
	reconnect_timer_ = new QTimer(this);
	reconnect_timer_->setSingleShot(true);

	connect(poll_timer_, &QTimer::timeout,
		this, &RecastYouTubeChat::onPollTimer);
	connect(reconnect_timer_, &QTimer::timeout,
		this, &RecastYouTubeChat::onReconnectTimer);
}

RecastYouTubeChat::~RecastYouTubeChat()
{
	disconnect();
}

void RecastYouTubeChat::connectToChat(const QString &channel)
{
	/* channel parameter is unused for YouTube -- we use the
	 * authenticated user's active broadcast */
	Q_UNUSED(channel);

	blog(LOG_INFO, "[Recast Chat] YouTube connecting...");
	fetchLiveChatId();
}

void RecastYouTubeChat::disconnect()
{
	poll_timer_->stop();
	reconnect_timer_->stop();
	live_chat_id_.clear();
	next_page_token_.clear();

	if (connected_) {
		connected_ = false;
		emit connectionStateChanged(false);
	}
}

void RecastYouTubeChat::sendMessage(const QString &msg)
{
	if (!connected_ || live_chat_id_.isEmpty())
		return;

	auto *auth = RecastAuthManager::instance();
	QString token = auth->accessToken(QStringLiteral("youtube"));
	if (token.isEmpty())
		return;

	/* POST liveChatMessages.insert */
	QUrl url(QStringLiteral(
		"https://www.googleapis.com/youtube/v3/liveChat/messages"
		"?part=snippet"));

	QNetworkRequest req(url);
	req.setTransferTimeout(15000);
	req.setHeader(QNetworkRequest::ContentTypeHeader,
		      QStringLiteral("application/json"));
	req.setRawHeader("Authorization",
			 QStringLiteral("Bearer %1").arg(token).toUtf8());

	QJsonObject snippet;
	snippet[QStringLiteral("liveChatId")] = live_chat_id_;
	snippet[QStringLiteral("type")] =
		QStringLiteral("textMessageEvent");

	QJsonObject textDetails;
	textDetails[QStringLiteral("messageText")] = msg;
	snippet[QStringLiteral("textMessageDetails")] = textDetails;

	QJsonObject body;
	body[QStringLiteral("snippet")] = snippet;

	QNetworkReply *reply = net_->post(
		req, QJsonDocument(body).toJson(QJsonDocument::Compact));

	connect(reply, &QNetworkReply::finished, reply, [reply]() {
		if (reply->error() != QNetworkReply::NoError) {
			blog(LOG_ERROR,
			     "[Recast Chat] YouTube send error: %s",
			     reply->errorString().toUtf8().constData());
		}
		reply->deleteLater();
	});
}

bool RecastYouTubeChat::isConnected() const
{
	return connected_;
}

void RecastYouTubeChat::onPollTimer()
{
	if (connected_)
		pollMessages();
}

void RecastYouTubeChat::onReconnectTimer()
{
	if (!connected_) {
		blog(LOG_INFO, "[Recast Chat] YouTube reconnecting...");
		fetchLiveChatId();
	}
}

void RecastYouTubeChat::fetchLiveChatId()
{
	auto *auth = RecastAuthManager::instance();
	QString token = auth->accessToken(QStringLiteral("youtube"));

	if (token.isEmpty()) {
		blog(LOG_ERROR,
		     "[Recast Chat] YouTube: no auth token available");
		reconnect_timer_->start(5000);
		return;
	}

	QUrl url(QStringLiteral(
		"https://www.googleapis.com/youtube/v3/liveBroadcasts"
		"?part=snippet&broadcastStatus=active&mine=true"));

	QNetworkRequest req(url);
	req.setTransferTimeout(15000);
	req.setRawHeader("Authorization",
			 QStringLiteral("Bearer %1").arg(token).toUtf8());

	QNetworkReply *reply = net_->get(req);

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			blog(LOG_ERROR,
			     "[Recast Chat] YouTube liveBroadcasts error: %s",
			     reply->errorString().toUtf8().constData());
			reconnect_timer_->start(5000);
			return;
		}

		QJsonDocument doc =
			QJsonDocument::fromJson(reply->readAll());
		QJsonArray items =
			doc.object().value(QStringLiteral("items"))
				.toArray();

		if (items.isEmpty()) {
			blog(LOG_INFO,
			     "[Recast Chat] YouTube: no active broadcast "
			     "found, retrying...");
			reconnect_timer_->start(5000);
			return;
		}

		QJsonObject broadcast = items.first().toObject();
		live_chat_id_ =
			broadcast.value(QStringLiteral("snippet"))
				.toObject()
				.value(QStringLiteral("liveChatId"))
				.toString();

		if (live_chat_id_.isEmpty()) {
			blog(LOG_ERROR,
			     "[Recast Chat] YouTube: liveChatId missing");
			reconnect_timer_->start(5000);
			return;
		}

		blog(LOG_INFO,
		     "[Recast Chat] YouTube connected, liveChatId=%s",
		     live_chat_id_.toUtf8().constData());

		connected_ = true;
		emit connectionStateChanged(true);

		/* Start polling */
		next_page_token_.clear();
		poll_timer_->start(poll_interval_ms_);

		/* Do first poll immediately */
		pollMessages();
	});
}

void RecastYouTubeChat::pollMessages()
{
	auto *auth = RecastAuthManager::instance();
	QString token = auth->accessToken(QStringLiteral("youtube"));

	if (token.isEmpty() || live_chat_id_.isEmpty())
		return;

	QUrl url(QStringLiteral(
		"https://www.googleapis.com/youtube/v3/liveChat/messages"));

	QUrlQuery query;
	query.addQueryItem(QStringLiteral("liveChatId"), live_chat_id_);
	query.addQueryItem(QStringLiteral("part"),
			   QStringLiteral("snippet,authorDetails"));
	if (!next_page_token_.isEmpty())
		query.addQueryItem(QStringLiteral("pageToken"),
				   next_page_token_);
	url.setQuery(query);

	QNetworkRequest req(url);
	req.setTransferTimeout(15000);
	req.setRawHeader("Authorization",
			 QStringLiteral("Bearer %1").arg(token).toUtf8());

	QNetworkReply *reply = net_->get(req);

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			blog(LOG_ERROR,
			     "[Recast Chat] YouTube poll error: %s",
			     reply->errorString().toUtf8().constData());

			/* If 403 or similar, the broadcast may have ended */
			int status = reply->attribute(
				QNetworkRequest::HttpStatusCodeAttribute)
				.toInt();
			if (status == 403 || status == 404) {
				disconnect();
				reconnect_timer_->start(5000);
			}
			return;
		}

		QJsonDocument doc =
			QJsonDocument::fromJson(reply->readAll());
		QJsonObject root = doc.object();

		/* Update page token */
		next_page_token_ = root.value(
			QStringLiteral("nextPageToken")).toString();

		/* Respect polling interval from server */
		int interval = root.value(
			QStringLiteral("pollingIntervalMillis"))
			.toInt(5000);
		if (interval < 1000)
			interval = 1000;
		if (interval != poll_interval_ms_) {
			poll_interval_ms_ = interval;
			poll_timer_->setInterval(poll_interval_ms_);
		}

		/* Process messages */
		QJsonArray items = root.value(
			QStringLiteral("items")).toArray();

		for (const QJsonValue &val : items) {
			QJsonObject item = val.toObject();
			QJsonObject snippet = item.value(
				QStringLiteral("snippet")).toObject();
			QJsonObject author = item.value(
				QStringLiteral("authorDetails")).toObject();

			/* Only handle text messages */
			QString type = snippet.value(
				QStringLiteral("type")).toString();
			if (type != QStringLiteral("textMessageEvent"))
				continue;

			RecastChatMessage msg;
			msg.platform = QStringLiteral("youtube");
			msg.displayName = author.value(
				QStringLiteral("displayName")).toString();
			msg.username = author.value(
				QStringLiteral("channelId")).toString();
			msg.message = snippet.value(
				QStringLiteral("textMessageDetails"))
				.toObject()
				.value(QStringLiteral("messageText"))
				.toString();
			msg.timestamp =
				QDateTime::currentMSecsSinceEpoch();

			/* Assign color based on role */
			bool isOwner = author.value(
				QStringLiteral("isChatOwner"))
				.toBool();
			bool isMod = author.value(
				QStringLiteral("isChatModerator"))
				.toBool();
			bool isMember = author.value(
				QStringLiteral("isChatSponsor"))
				.toBool();

			msg.isOwner = isOwner;
			msg.isMod = isMod;
			msg.isSub = isMember;

			if (isOwner)
				msg.nameColor = QColor(255, 0, 0);
			else if (isMod)
				msg.nameColor = QColor(90, 90, 255);
			else if (isMember)
				msg.nameColor = QColor(44, 166, 63);
			else
				msg.nameColor = QColor(255, 0, 0);

			emit messageReceived(msg);
		}
	});
}

/* ====================================================================
 * RecastKickChat -- Pusher WebSocket
 * ==================================================================== */

RecastKickChat::RecastKickChat(QObject *parent)
	: RecastChatProvider(parent)
{
	ws_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest,
			     this);
	net_ = new QNetworkAccessManager(this);
	reconnect_timer_ = new QTimer(this);
	reconnect_timer_->setSingleShot(true);

	connect(ws_, &QWebSocket::connected,
		this, &RecastKickChat::onConnected);
	connect(ws_, &QWebSocket::disconnected,
		this, &RecastKickChat::onDisconnected);
	connect(ws_, &QWebSocket::textMessageReceived,
		this, &RecastKickChat::onTextMessageReceived);
	connect(reconnect_timer_, &QTimer::timeout,
		this, &RecastKickChat::onReconnectTimer);
}

RecastKickChat::~RecastKickChat()
{
	disconnect();
}

void RecastKickChat::connectToChat(const QString &channel)
{
	channel_slug_ = channel.toLower();

	blog(LOG_INFO, "[Recast Chat] Kick connecting for channel '%s'",
	     channel_slug_.toUtf8().constData());

	fetchChannelInfo();
}

void RecastKickChat::disconnect()
{
	reconnect_timer_->stop();

	if (ws_->state() != QAbstractSocket::UnconnectedState) {
		ws_->close();
	}

	if (connected_) {
		connected_ = false;
		emit connectionStateChanged(false);
	}
}

void RecastKickChat::sendMessage(const QString &msg)
{
	/* Kick does not provide a public send API -- skip silently */
	Q_UNUSED(msg);
}

bool RecastKickChat::isConnected() const
{
	return connected_;
}

void RecastKickChat::fetchChannelInfo()
{
	QUrl url(QStringLiteral("https://kick.com/api/v2/channels/%1")
		.arg(channel_slug_));

	QNetworkRequest req(url);
	req.setTransferTimeout(15000);
	req.setRawHeader("Accept", "application/json");

	QNetworkReply *reply = net_->get(req);

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			blog(LOG_ERROR,
			     "[Recast Chat] Kick channel info error: %s",
			     reply->errorString().toUtf8().constData());
			reconnect_timer_->start(5000);
			return;
		}

		QJsonDocument doc =
			QJsonDocument::fromJson(reply->readAll());
		QJsonObject root = doc.object();

		chatroom_id_ = root.value(QStringLiteral("chatroom"))
			.toObject()
			.value(QStringLiteral("id"))
			.toInt(0);

		if (chatroom_id_ == 0) {
			blog(LOG_ERROR,
			     "[Recast Chat] Kick: chatroom id not found "
			     "for '%s'",
			     channel_slug_.toUtf8().constData());
			reconnect_timer_->start(5000);
			return;
		}

		blog(LOG_INFO,
		     "[Recast Chat] Kick chatroom_id=%d for '%s'",
		     chatroom_id_,
		     channel_slug_.toUtf8().constData());

		connectPusher();
	});
}

void RecastKickChat::connectPusher()
{
	QUrl url(QStringLiteral(
		"wss://ws-us2.pusher.com/app/"
		"32cbd69e4b950bf97679"
		"?protocol=7&client=js&version=8.4.0-rc2&flash=false"));

	ws_->open(url);
}

void RecastKickChat::onConnected()
{
	blog(LOG_INFO, "[Recast Chat] Kick Pusher WebSocket connected");
	/* Wait for pusher:connection_established before subscribing */
}

void RecastKickChat::onDisconnected()
{
	blog(LOG_INFO, "[Recast Chat] Kick Pusher WebSocket disconnected");

	if (connected_) {
		connected_ = false;
		emit connectionStateChanged(false);
	}

	/* Schedule reconnect */
	if (!channel_slug_.isEmpty()) {
		reconnect_timer_->start(5000);
	}
}

void RecastKickChat::onTextMessageReceived(const QString &raw)
{
	QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
	if (!doc.isObject())
		return;

	QJsonObject obj = doc.object();
	QString event = obj.value(QStringLiteral("event")).toString();

	if (event == QStringLiteral("pusher:connection_established")) {
		/* Connection ready -- subscribe to chatroom channel */
		blog(LOG_INFO,
		     "[Recast Chat] Kick Pusher connection established");
		subscribeToChatroom();

	} else if (event == QStringLiteral("pusher_internal:subscription_succeeded")) {
		blog(LOG_INFO,
		     "[Recast Chat] Kick subscribed to chatroom %d",
		     chatroom_id_);
		connected_ = true;
		emit connectionStateChanged(true);

	} else if (event == QStringLiteral("pusher:ping")) {
		/* Respond to Pusher keepalive ping */
		QJsonObject pong;
		pong[QStringLiteral("event")] =
			QStringLiteral("pusher:pong");
		pong[QStringLiteral("data")] = QJsonObject();
		ws_->sendTextMessage(QString::fromUtf8(
			QJsonDocument(pong).toJson(
				QJsonDocument::Compact)));

	} else if (event == QStringLiteral("App\\Events\\ChatMessageEvent")) {
		QString data_str = obj.value(
			QStringLiteral("data")).toString();
		parseChatMessageEvent(data_str);
	}
}

void RecastKickChat::onReconnectTimer()
{
	if (!channel_slug_.isEmpty() && !connected_) {
		blog(LOG_INFO, "[Recast Chat] Kick reconnecting...");
		fetchChannelInfo();
	}
}

void RecastKickChat::subscribeToChatroom()
{
	QString channel = QStringLiteral("chatrooms.%1.v2")
		.arg(chatroom_id_);

	QJsonObject data;
	data[QStringLiteral("channel")] = channel;

	QJsonObject msg;
	msg[QStringLiteral("event")] =
		QStringLiteral("pusher:subscribe");
	msg[QStringLiteral("data")] = data;

	ws_->sendTextMessage(QString::fromUtf8(
		QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

void RecastKickChat::parseChatMessageEvent(const QString &data_str)
{
	/*
	 * The data field is a JSON-encoded string that must be parsed again.
	 * Contains: id, chatroom_id, content, created_at,
	 *           sender { id, username, slug,
	 *                    identity { color, badges [] } }
	 */
	QJsonDocument doc = QJsonDocument::fromJson(data_str.toUtf8());
	if (!doc.isObject())
		return;

	QJsonObject root = doc.object();

	RecastChatMessage msg;
	msg.platform = QStringLiteral("kick");
	msg.message = root.value(QStringLiteral("content")).toString();
	msg.timestamp = QDateTime::currentMSecsSinceEpoch();

	QJsonObject sender = root.value(
		QStringLiteral("sender")).toObject();
	msg.username = sender.value(
		QStringLiteral("slug")).toString();
	msg.displayName = sender.value(
		QStringLiteral("username")).toString();

	/* Identity provides color and badges */
	QJsonObject identity = sender.value(
		QStringLiteral("identity")).toObject();
	QString color = identity.value(
		QStringLiteral("color")).toString();
	if (!color.isEmpty()) {
		if (!color.startsWith('#'))
			color.prepend('#');
		msg.nameColor = QColor(color);
	}
	if (!msg.nameColor.isValid())
		msg.nameColor = QColor(83, 252, 24);

	/* Check badges for moderator / broadcaster / subscriber */
	QJsonArray badges = identity.value(
		QStringLiteral("badges")).toArray();
	for (const QJsonValue &badge_val : badges) {
		QJsonObject badge = badge_val.toObject();
		QString badge_type = badge.value(
			QStringLiteral("type")).toString();
		if (badge_type == QStringLiteral("moderator"))
			msg.isMod = true;
		else if (badge_type == QStringLiteral("broadcaster"))
			msg.isOwner = true;
		else if (badge_type == QStringLiteral("subscriber"))
			msg.isSub = true;
	}

	emit messageReceived(msg);
}

/* ====================================================================
 * RecastChatDock -- Unified chat display
 * ==================================================================== */

RecastChatDock::RecastChatDock(QWidget *parent)
	: QWidget(parent)
{
	auto *main_layout = new QVBoxLayout(this);
	main_layout->setContentsMargins(0, 0, 0, 0);
	main_layout->setSpacing(0);

	/* ---- Connection indicators bar ---- */
	indicators_widget_ = new QWidget;
	indicators_layout_ = new QHBoxLayout(indicators_widget_);
	indicators_layout_->setContentsMargins(4, 2, 4, 2);
	indicators_layout_->setSpacing(8);
	indicators_layout_->addStretch();
	main_layout->addWidget(indicators_widget_);

	/* ---- Chat display (read-only HTML) ---- */
	chat_display_ = new QTextBrowser;
	chat_display_->setReadOnly(true);
	chat_display_->setOpenExternalLinks(false);
	chat_display_->setStyleSheet(
		"QTextBrowser { background: #1e1e1e; color: #ddd; "
		"border: none; padding: 4px; font-size: 13px; }");
	chat_display_->document()->setDefaultStyleSheet(
		"body { margin: 0; padding: 0; }"
		"a { color: #8ab4f8; }");
	main_layout->addWidget(chat_display_, 1);

	/* ---- Input row ---- */
	auto *input_row = new QHBoxLayout;
	input_row->setContentsMargins(4, 2, 4, 4);
	input_row->setSpacing(4);

	input_ = new QLineEdit;
	input_->setPlaceholderText("Send a message...");
	input_->setStyleSheet(
		"QLineEdit { background: #2a2a2a; color: #ddd; "
		"border: 1px solid #444; border-radius: 3px; "
		"padding: 4px 8px; }");
	connect(input_, &QLineEdit::returnPressed,
		this, &RecastChatDock::onSendClicked);
	input_row->addWidget(input_, 1);

	send_btn_ = new QPushButton("Send");
	send_btn_->setStyleSheet(
		"QPushButton { background: #444; color: white; "
		"border-radius: 3px; padding: 4px 12px; "
		"font-weight: bold; }"
		"QPushButton:hover { background: #555; }");
	connect(send_btn_, &QPushButton::clicked,
		this, &RecastChatDock::onSendClicked);
	input_row->addWidget(send_btn_);

	main_layout->addLayout(input_row);
}

RecastChatDock::~RecastChatDock()
{
	providers_.clear();
}

void RecastChatDock::addProvider(RecastChatProvider *provider)
{
	if (!provider)
		return;

	providers_.push_back(provider);

	connect(provider, &RecastChatProvider::messageReceived,
		this, &RecastChatDock::onMessageReceived);
	connect(provider, &RecastChatProvider::connectionStateChanged,
		this, [this, provider](bool connected) {
			updateIndicator(provider, connected);
		});

	/* Create indicator label for this provider */
	QLabel *indicator = new QLabel;
	indicator->setFixedSize(20, 20);

	/* Get platform icon */
	QIcon icon = recast_platform_icon(provider->platform());
	indicator->setPixmap(icon.pixmap(16, 16));
	indicator->setToolTip(
		QStringLiteral("%1: Disconnected")
			.arg(provider->platform()));
	indicator->setStyleSheet("opacity: 0.3;");

	/* Insert before the stretch */
	indicators_layout_->insertWidget(
		indicators_layout_->count() - 1, indicator);
	indicator_labels_[provider] = indicator;

	blog(LOG_INFO, "[Recast Chat] Added provider: %s",
	     provider->platform().toUtf8().constData());
}

void RecastChatDock::removeProvider(RecastChatProvider *provider)
{
	if (!provider)
		return;

	QObject::disconnect(provider, nullptr, this, nullptr);

	auto it = std::find(providers_.begin(), providers_.end(),
			    provider);
	if (it != providers_.end())
		providers_.erase(it);

	/* Remove indicator */
	auto label_it = indicator_labels_.find(provider);
	if (label_it != indicator_labels_.end()) {
		indicators_layout_->removeWidget(label_it.value());
		label_it.value()->deleteLater();
		indicator_labels_.erase(label_it);
	}

	blog(LOG_INFO, "[Recast Chat] Removed provider: %s",
	     provider->platform().toUtf8().constData());
}

void RecastChatDock::onMessageReceived(const RecastChatMessage &msg)
{
	appendMessage(msg);
}

void RecastChatDock::onConnectionStateChanged(bool connected)
{
	Q_UNUSED(connected);
	/* Handled via lambda in addProvider */
}

void RecastChatDock::onSendClicked()
{
	QString text = input_->text().trimmed();
	if (text.isEmpty())
		return;

	/* Send to all connected providers */
	for (auto *provider : providers_) {
		if (provider->isConnected())
			provider->sendMessage(text);
	}

	input_->clear();
}

void RecastChatDock::appendMessage(const RecastChatMessage &msg)
{
	/* Check if user has scrolled up */
	bool was_at_bottom = isScrolledToBottom();

	/* Build HTML line */
	QString pcolor = platformColor(msg.platform);
	QString pletter = platformLetter(msg.platform);
	QString name_color = msg.nameColor.isValid()
		? msg.nameColor.name()
		: QStringLiteral("#cccccc");

	/* Badge indicators */
	QString badges;
	if (msg.isOwner)
		badges += QStringLiteral(
			"<span style='color:#ff4444;font-size:10px;'>"
			"&#9733;</span> ");
	else if (msg.isMod)
		badges += QStringLiteral(
			"<span style='color:#44ff44;font-size:10px;'>"
			"&#9876;</span> ");

	QString html = QStringLiteral(
		"<span style='color:%1;font-size:10px;font-weight:bold;'>"
		"[%2]</span> "
		"%3"
		"<b style='color:%4;'>%5</b>"
		"<span style='color:#dddddd;'>: %6</span><br>")
		.arg(pcolor,
		     pletter,
		     badges,
		     name_color,
		     escapeHtml(msg.displayName),
		     escapeHtml(msg.message));

	chat_display_->moveCursor(QTextCursor::End);
	chat_display_->insertHtml(html);
	message_count_++;

	/* Trim old messages if over limit */
	if (message_count_ > MAX_MESSAGES) {
		QTextCursor cursor = chat_display_->textCursor();
		cursor.movePosition(QTextCursor::Start);
		cursor.movePosition(QTextCursor::Down,
				    QTextCursor::KeepAnchor,
				    message_count_ - MAX_MESSAGES);
		cursor.removeSelectedText();
		message_count_ = MAX_MESSAGES;
	}

	/* Auto-scroll to bottom if user was already there */
	if (was_at_bottom) {
		QScrollBar *sb = chat_display_->verticalScrollBar();
		sb->setValue(sb->maximum());
	}
}

void RecastChatDock::updateIndicator(RecastChatProvider *provider,
				     bool connected)
{
	auto it = indicator_labels_.find(provider);
	if (it == indicator_labels_.end())
		return;

	QLabel *label = it.value();

	if (connected) {
		label->setToolTip(
			QStringLiteral("%1: Connected")
				.arg(provider->platform()));
		label->setStyleSheet(QString());
	} else {
		label->setToolTip(
			QStringLiteral("%1: Disconnected")
				.arg(provider->platform()));
		label->setStyleSheet("opacity: 0.3;");
	}
}

bool RecastChatDock::isScrolledToBottom() const
{
	QScrollBar *sb = chat_display_->verticalScrollBar();
	return sb->value() >= sb->maximum() - 10;
}

QString RecastChatDock::platformColor(const QString &platform) const
{
	if (platform == QStringLiteral("twitch"))
		return QStringLiteral("#9146FF");
	if (platform == QStringLiteral("youtube"))
		return QStringLiteral("#FF0000");
	if (platform == QStringLiteral("kick"))
		return QStringLiteral("#53FC18");
	return QStringLiteral("#888888");
}

QString RecastChatDock::platformLetter(const QString &platform) const
{
	if (platform == QStringLiteral("twitch"))
		return QStringLiteral("T");
	if (platform == QStringLiteral("youtube"))
		return QStringLiteral("Y");
	if (platform == QStringLiteral("kick"))
		return QStringLiteral("K");
	return QStringLiteral("?");
}

QString RecastChatDock::escapeHtml(const QString &text)
{
	QString escaped = text;
	escaped.replace('&', QStringLiteral("&amp;"));
	escaped.replace('<', QStringLiteral("&lt;"));
	escaped.replace('>', QStringLiteral("&gt;"));
	escaped.replace('"', QStringLiteral("&quot;"));
	return escaped;
}
