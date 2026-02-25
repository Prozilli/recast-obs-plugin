/*
 * recast-events.cpp -- Platform event providers and unified events feed.
 *
 * Handles Twitch EventSub, YouTube live event polling, and Kick Pusher
 * events, displaying them in a unified scrolling feed dock.
 */

#include "recast-events.h"
#include "recast-auth.h"
#include "recast-platform-icons.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollBar>
#include <QStyle>
#include <QUrlQuery>

extern "C" {
#include <obs-module.h>
}

/* ====================================================================
 * RecastTwitchEvents -- Twitch EventSub via WebSocket
 * ==================================================================== */

static const char *TWITCH_EVENTSUB_URL =
	"wss://eventsub.wss.twitch.tv/ws";
static const char *TWITCH_EVENTSUB_API =
	"https://api.twitch.tv/helix/eventsub/subscriptions";

RecastTwitchEvents::RecastTwitchEvents(QObject *parent)
	: RecastEventProvider(parent)
{
	ws_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest,
			     this);
	net_mgr_ = new QNetworkAccessManager(this);

	reconnect_timer_ = new QTimer(this);
	reconnect_timer_->setSingleShot(true);
	connect(reconnect_timer_, &QTimer::timeout,
		this, &RecastTwitchEvents::onReconnectTimer);

	keepalive_timer_ = new QTimer(this);
	keepalive_timer_->setSingleShot(true);
	connect(keepalive_timer_, &QTimer::timeout,
		this, &RecastTwitchEvents::onKeepaliveTimeout);

	connect(ws_, &QWebSocket::connected,
		this, &RecastTwitchEvents::onConnected);
	connect(ws_, &QWebSocket::disconnected,
		this, &RecastTwitchEvents::onDisconnected);
	connect(ws_, &QWebSocket::textMessageReceived,
		this, &RecastTwitchEvents::onTextMessageReceived);
}

RecastTwitchEvents::~RecastTwitchEvents()
{
	disconnect();
}

void RecastTwitchEvents::connectToEvents()
{
	if (connected_)
		return;

	auto *auth = RecastAuthManager::instance();
	if (!auth->isAuthenticated("twitch")) {
		blog(LOG_WARNING,
		     "[Recast Events] Twitch not authenticated, "
		     "cannot connect to EventSub");
		return;
	}

	blog(LOG_INFO, "[Recast Events] Connecting to Twitch EventSub...");
	ws_->open(QUrl(QString::fromUtf8(TWITCH_EVENTSUB_URL)));
}

void RecastTwitchEvents::disconnect()
{
	keepalive_timer_->stop();
	reconnect_timer_->stop();
	session_id_.clear();

	if (reconnect_ws_) {
		reconnect_ws_->close();
		reconnect_ws_->deleteLater();
		reconnect_ws_ = nullptr;
	}

	if (ws_->state() != QAbstractSocket::UnconnectedState)
		ws_->close();

	if (connected_) {
		connected_ = false;
		emit connectionStateChanged(false);
	}
}

bool RecastTwitchEvents::isConnected() const
{
	return connected_;
}

void RecastTwitchEvents::onConnected()
{
	blog(LOG_INFO, "[Recast Events] Twitch EventSub WebSocket connected");
	/* Wait for session_welcome message before creating subscriptions */
}

void RecastTwitchEvents::onDisconnected()
{
	blog(LOG_WARNING, "[Recast Events] Twitch EventSub disconnected");
	keepalive_timer_->stop();
	session_id_.clear();

	if (connected_) {
		connected_ = false;
		emit connectionStateChanged(false);
	}

	/* Schedule reconnect */
	if (!reconnect_timer_->isActive())
		reconnect_timer_->start(5000);
}

void RecastTwitchEvents::onReconnectTimer()
{
	auto *auth = RecastAuthManager::instance();
	if (!auth->isAuthenticated("twitch"))
		return;

	blog(LOG_INFO, "[Recast Events] Attempting Twitch EventSub reconnect");
	connectToEvents();
}

void RecastTwitchEvents::onKeepaliveTimeout()
{
	blog(LOG_WARNING,
	     "[Recast Events] Twitch EventSub keepalive timeout, "
	     "reconnecting...");
	ws_->close();
}

void RecastTwitchEvents::onTextMessageReceived(const QString &raw)
{
	QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
	if (!doc.isObject())
		return;

	QJsonObject root = doc.object();
	QJsonObject metadata = root.value("metadata").toObject();
	QString message_type = metadata.value("message_type").toString();
	QJsonObject payload = root.value("payload").toObject();

	if (message_type == "session_welcome") {
		QJsonObject session = payload.value("session").toObject();
		session_id_ = session.value("id").toString();

		int keepalive_sec =
			session.value("keepalive_timeout_seconds").toInt(10);
		/* Set timer slightly longer than keepalive interval */
		keepalive_timer_->start((keepalive_sec + 5) * 1000);

		blog(LOG_INFO,
		     "[Recast Events] Twitch EventSub session: %s "
		     "(keepalive %ds)",
		     session_id_.toUtf8().constData(), keepalive_sec);

		connected_ = true;
		emit connectionStateChanged(true);

		createAllSubscriptions();

	} else if (message_type == "session_keepalive") {
		/* Reset keepalive timer */
		if (keepalive_timer_->isActive())
			keepalive_timer_->start(keepalive_timer_->interval());

	} else if (message_type == "session_reconnect") {
		QString reconnect_url =
			payload.value("session").toObject()
				.value("reconnect_url").toString();

		blog(LOG_INFO,
		     "[Recast Events] Twitch EventSub reconnect requested");

		/* Abort any pending reconnect attempt */
		if (reconnect_ws_) {
			reconnect_ws_->close();
			reconnect_ws_->deleteLater();
			reconnect_ws_ = nullptr;
		}

		/* Disconnect old socket signals to prevent race */
		QObject::disconnect(ws_, &QWebSocket::disconnected,
				    this, &RecastTwitchEvents::onDisconnected);

		/* Connect to new URL, close old after new connects */
		reconnect_ws_ = new QWebSocket(
			QString(), QWebSocketProtocol::VersionLatest, this);

		connect(reconnect_ws_, &QWebSocket::connected, this,
			[this]() {
				blog(LOG_INFO,
				     "[Recast Events] Twitch EventSub "
				     "reconnected to new endpoint");

				/* Swap sockets */
				QWebSocket *old = ws_;
				ws_ = reconnect_ws_;
				reconnect_ws_ = nullptr;

				/* Rewire signals */
				QObject::disconnect(old, nullptr,
						    this, nullptr);
				connect(ws_, &QWebSocket::connected,
					this,
					&RecastTwitchEvents::onConnected);
				connect(ws_, &QWebSocket::disconnected,
					this,
					&RecastTwitchEvents::onDisconnected);
				connect(ws_,
					&QWebSocket::textMessageReceived,
					this,
					&RecastTwitchEvents::
						onTextMessageReceived);

				old->close();
				old->deleteLater();
			});

		connect(reconnect_ws_, &QWebSocket::disconnected, this,
			[this]() {
				if (reconnect_ws_) {
					blog(LOG_WARNING,
					     "[Recast Events] Twitch EventSub "
					     "reconnect socket disconnected, "
					     "falling back to full reconnect");
					reconnect_ws_->deleteLater();
					reconnect_ws_ = nullptr;
					/* Restore old socket disconnect handler and trigger reconnect */
					connect(ws_, &QWebSocket::disconnected,
						this, &RecastTwitchEvents::onDisconnected);
					reconnect_timer_->start(5000);
				}
			});

		connect(reconnect_ws_, &QWebSocket::textMessageReceived,
			this, &RecastTwitchEvents::onTextMessageReceived);

		reconnect_ws_->open(QUrl(reconnect_url));

		/* Timeout: if reconnect doesn't complete in 30s, fall back */
		QTimer::singleShot(30000, this, [this]() {
			if (reconnect_ws_) {
				blog(LOG_WARNING,
				     "[Recast Events] Twitch EventSub "
				     "reconnect timed out");
				reconnect_ws_->close();
				reconnect_ws_->deleteLater();
				reconnect_ws_ = nullptr;
				connect(ws_, &QWebSocket::disconnected,
					this, &RecastTwitchEvents::onDisconnected);
				reconnect_timer_->start(5000);
			}
		});

	} else if (message_type == "notification") {
		/* Reset keepalive timer on any message */
		if (keepalive_timer_->isActive())
			keepalive_timer_->start(keepalive_timer_->interval());

		QJsonObject subscription =
			payload.value("subscription").toObject();
		QString sub_type = subscription.value("type").toString();
		QJsonObject event_data = payload.value("event").toObject();

		RecastPlatformEvent evt =
			parseNotification(sub_type, event_data);
		if (evt.type != EVENT_UNKNOWN)
			emit eventReceived(evt);
	}
}

void RecastTwitchEvents::createSubscription(const QString &type,
					    const QString &version,
					    const QJsonObject &condition)
{
	auto *auth = RecastAuthManager::instance();
	QString access_token = auth->accessToken("twitch");
	QString client_id = auth->clientId("twitch");

	QJsonObject body;
	body["type"] = type;
	body["version"] = version;
	body["condition"] = condition;

	QJsonObject transport;
	transport["method"] = "websocket";
	transport["session_id"] = session_id_;
	body["transport"] = transport;

	QNetworkRequest req(QUrl(QString::fromUtf8(TWITCH_EVENTSUB_API)));
	req.setTransferTimeout(15000);
	req.setHeader(QNetworkRequest::ContentTypeHeader,
		      "application/json");
	req.setRawHeader("Authorization",
			 ("Bearer " + access_token).toUtf8());
	req.setRawHeader("Client-Id", client_id.toUtf8());

	QNetworkReply *reply =
		net_mgr_->post(req, QJsonDocument(body).toJson());

	connect(reply, &QNetworkReply::finished, this, [reply, type]() {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			blog(LOG_WARNING,
			     "[Recast Events] Failed to create Twitch "
			     "subscription '%s': %s",
			     type.toUtf8().constData(),
			     reply->errorString().toUtf8().constData());
		} else {
			blog(LOG_INFO,
			     "[Recast Events] Twitch subscription "
			     "created: %s",
			     type.toUtf8().constData());
		}
	});
}

void RecastTwitchEvents::createAllSubscriptions()
{
	auto *auth = RecastAuthManager::instance();
	QString user_id = auth->userId("twitch");

	if (user_id.isEmpty()) {
		blog(LOG_WARNING,
		     "[Recast Events] No Twitch user ID, "
		     "cannot create subscriptions");
		return;
	}

	QJsonObject broadcaster_cond;
	broadcaster_cond["broadcaster_user_id"] = user_id;

	QJsonObject follow_cond;
	follow_cond["broadcaster_user_id"] = user_id;
	follow_cond["moderator_user_id"] = user_id;

	QJsonObject raid_cond;
	raid_cond["to_broadcaster_user_id"] = user_id;

	/* channel.follow v2 - requires moderator:read:followers */
	createSubscription("channel.follow", "2", follow_cond);

	/* channel.subscribe v1 - requires channel:read:subscriptions */
	createSubscription("channel.subscribe", "1", broadcaster_cond);

	/* channel.subscription.gift v1 */
	createSubscription("channel.subscription.gift", "1",
			   broadcaster_cond);

	/* channel.subscription.message v1 - resubs */
	createSubscription("channel.subscription.message", "1",
			   broadcaster_cond);

	/* channel.cheer v1 - bits, requires bits:read */
	createSubscription("channel.cheer", "1", broadcaster_cond);

	/* channel.raid v1 - no special scope */
	createSubscription("channel.raid", "1", raid_cond);

	/* channel.channel_points_custom_reward_redemption.add v1 */
	createSubscription(
		"channel.channel_points_custom_reward_redemption.add",
		"1", broadcaster_cond);

	/* channel.hype_train.begin v2 */
	createSubscription("channel.hype_train.begin", "2",
			   broadcaster_cond);

	/* channel.hype_train.progress v2 */
	createSubscription("channel.hype_train.progress", "2",
			   broadcaster_cond);

	/* channel.hype_train.end v2 */
	createSubscription("channel.hype_train.end", "2",
			   broadcaster_cond);

	blog(LOG_INFO,
	     "[Recast Events] Created all Twitch EventSub subscriptions "
	     "for user %s",
	     user_id.toUtf8().constData());
}

RecastPlatformEvent RecastTwitchEvents::parseNotification(
	const QString &sub_type, const QJsonObject &event_data)
{
	RecastPlatformEvent evt;
	evt.platform = QStringLiteral("twitch");
	evt.timestamp = QDateTime::currentMSecsSinceEpoch();

	if (sub_type == "channel.follow") {
		evt.type = EVENT_FOLLOW;
		evt.username = event_data.value("user_login").toString();
		evt.displayName = event_data.value("user_name").toString();

	} else if (sub_type == "channel.subscribe") {
		evt.type = EVENT_SUBSCRIBE;
		evt.username = event_data.value("user_login").toString();
		evt.displayName = event_data.value("user_name").toString();
		evt.tier = event_data.value("tier").toString();
		evt.isAnonymous = event_data.value("is_gift").toBool();

	} else if (sub_type == "channel.subscription.gift") {
		evt.type = EVENT_GIFT_SUB;
		evt.username = event_data.value("user_login").toString();
		evt.displayName = event_data.value("user_name").toString();
		evt.amount = event_data.value("total").toInt();
		evt.tier = event_data.value("tier").toString();
		evt.isAnonymous = event_data.value("is_anonymous").toBool();
		if (evt.isAnonymous) {
			evt.username = "anonymous";
			evt.displayName = "Anonymous";
		}

	} else if (sub_type == "channel.subscription.message") {
		evt.type = EVENT_RESUB;
		evt.username = event_data.value("user_login").toString();
		evt.displayName = event_data.value("user_name").toString();
		evt.amount =
			event_data.value("cumulative_months").toInt();
		evt.tier = event_data.value("tier").toString();
		QJsonObject msg_obj =
			event_data.value("message").toObject();
		evt.message = msg_obj.value("text").toString();

	} else if (sub_type == "channel.cheer") {
		evt.type = EVENT_BITS;
		evt.username = event_data.value("user_login").toString();
		evt.displayName = event_data.value("user_name").toString();
		evt.amount = event_data.value("bits").toInt();
		evt.message = event_data.value("message").toString();
		evt.monetaryValue = evt.amount * 0.01;
		evt.currency = QStringLiteral("USD");
		evt.isAnonymous =
			event_data.value("is_anonymous").toBool();
		if (evt.isAnonymous) {
			evt.username = "anonymous";
			evt.displayName = "Anonymous";
		}

	} else if (sub_type == "channel.raid") {
		evt.type = EVENT_RAID;
		evt.username =
			event_data.value("from_broadcaster_user_login")
				.toString();
		evt.displayName =
			event_data.value("from_broadcaster_user_name")
				.toString();
		evt.amount = event_data.value("viewers").toInt();

	} else if (sub_type ==
		   "channel.channel_points_custom_reward_redemption.add") {
		evt.type = EVENT_CHANNEL_POINTS;
		evt.username = event_data.value("user_login").toString();
		evt.displayName = event_data.value("user_name").toString();
		QJsonObject reward =
			event_data.value("reward").toObject();
		evt.message = reward.value("title").toString();
		evt.amount = reward.value("cost").toInt();
		QString user_input =
			event_data.value("user_input").toString();
		if (!user_input.isEmpty())
			evt.message += " - " + user_input;

	} else if (sub_type == "channel.hype_train.begin" ||
		   sub_type == "channel.hype_train.progress") {
		evt.type = EVENT_HYPE_TRAIN;
		evt.amount = event_data.value("level").toInt();
		evt.displayName = QStringLiteral("Hype Train");
		int total = event_data.value("total").toInt();
		int goal = event_data.value("goal").toInt();
		evt.message = QString("Level %1 - %2/%3")
			.arg(evt.amount).arg(total).arg(goal);

	} else if (sub_type == "channel.hype_train.end") {
		evt.type = EVENT_HYPE_TRAIN;
		evt.amount = event_data.value("level").toInt();
		evt.displayName = QStringLiteral("Hype Train");
		int total = event_data.value("total").toInt();
		evt.message = QString("Ended at Level %1 (Total: %2)")
			.arg(evt.amount).arg(total);

	} else {
		evt.type = EVENT_UNKNOWN;
		blog(LOG_DEBUG,
		     "[Recast Events] Unknown Twitch event type: %s",
		     sub_type.toUtf8().constData());
	}

	return evt;
}

/* ====================================================================
 * RecastYouTubeEvents -- YouTube Live Events via polling
 * ==================================================================== */

RecastYouTubeEvents::RecastYouTubeEvents(QObject *parent)
	: RecastEventProvider(parent)
{
	net_ = new QNetworkAccessManager(this);

	poll_timer_ = new QTimer(this);
	connect(poll_timer_, &QTimer::timeout,
		this, &RecastYouTubeEvents::onPollTimer);

	reconnect_timer_ = new QTimer(this);
	reconnect_timer_->setSingleShot(true);
	connect(reconnect_timer_, &QTimer::timeout,
		this, &RecastYouTubeEvents::onReconnectTimer);
}

RecastYouTubeEvents::~RecastYouTubeEvents()
{
	disconnect();
}

void RecastYouTubeEvents::connectToEvents()
{
	if (connected_)
		return;

	auto *auth = RecastAuthManager::instance();
	if (!auth->isAuthenticated("youtube")) {
		blog(LOG_WARNING,
		     "[Recast Events] YouTube not authenticated, "
		     "cannot poll events");
		return;
	}

	blog(LOG_INFO, "[Recast Events] Connecting to YouTube events...");
	fetchLiveChatId();
}

void RecastYouTubeEvents::disconnect()
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

bool RecastYouTubeEvents::isConnected() const
{
	return connected_;
}

void RecastYouTubeEvents::fetchLiveChatId()
{
	auto *auth = RecastAuthManager::instance();
	QString access_token = auth->accessToken("youtube");

	QUrl url("https://www.googleapis.com/youtube/v3/liveBroadcasts");
	QUrlQuery query;
	query.addQueryItem("part", "snippet");
	query.addQueryItem("broadcastStatus", "active");
	query.addQueryItem("broadcastType", "all");
	url.setQuery(query);

	QNetworkRequest req(url);
	req.setTransferTimeout(15000);
	req.setRawHeader("Authorization",
			 ("Bearer " + access_token).toUtf8());

	QNetworkReply *reply = net_->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			blog(LOG_WARNING,
			     "[Recast Events] YouTube broadcast fetch "
			     "failed: %s",
			     reply->errorString().toUtf8().constData());
			reconnect_timer_->start(5000);
			return;
		}

		QJsonDocument doc =
			QJsonDocument::fromJson(reply->readAll());
		QJsonArray items =
			doc.object().value("items").toArray();

		if (items.isEmpty()) {
			blog(LOG_INFO,
			     "[Recast Events] No active YouTube "
			     "broadcast found, retrying...");
			reconnect_timer_->start(15000);
			return;
		}

		QJsonObject snippet =
			items.first().toObject()
				.value("snippet").toObject();
		live_chat_id_ =
			snippet.value("liveChatId").toString();

		if (live_chat_id_.isEmpty()) {
			blog(LOG_WARNING,
			     "[Recast Events] YouTube liveChatId is empty");
			reconnect_timer_->start(15000);
			return;
		}

		blog(LOG_INFO,
		     "[Recast Events] YouTube liveChatId: %s",
		     live_chat_id_.toUtf8().constData());

		connected_ = true;
		emit connectionStateChanged(true);
		poll_timer_->start(poll_interval_ms_);
	});
}

void RecastYouTubeEvents::onPollTimer()
{
	if (live_chat_id_.isEmpty())
		return;
	pollMessages();
}

void RecastYouTubeEvents::onReconnectTimer()
{
	auto *auth = RecastAuthManager::instance();
	if (!auth->isAuthenticated("youtube"))
		return;

	blog(LOG_INFO,
	     "[Recast Events] Attempting YouTube events reconnect");
	fetchLiveChatId();
}

void RecastYouTubeEvents::pollMessages()
{
	auto *auth = RecastAuthManager::instance();
	QString access_token = auth->accessToken("youtube");

	QUrl url("https://www.googleapis.com/youtube/v3/liveChat/messages");
	QUrlQuery query;
	query.addQueryItem("liveChatId", live_chat_id_);
	query.addQueryItem("part", "snippet,authorDetails");
	if (!next_page_token_.isEmpty())
		query.addQueryItem("pageToken", next_page_token_);
	url.setQuery(query);

	QNetworkRequest req(url);
	req.setTransferTimeout(15000);
	req.setRawHeader("Authorization",
			 ("Bearer " + access_token).toUtf8());

	QNetworkReply *reply = net_->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			blog(LOG_WARNING,
			     "[Recast Events] YouTube event poll "
			     "failed: %s",
			     reply->errorString().toUtf8().constData());

			int code = reply->attribute(
				QNetworkRequest::HttpStatusCodeAttribute)
				.toInt();
			if (code == 403 || code == 404) {
				/* Chat ended or forbidden */
				disconnect();
				reconnect_timer_->start(15000);
			}
			return;
		}

		QJsonDocument doc =
			QJsonDocument::fromJson(reply->readAll());
		QJsonObject root = doc.object();

		next_page_token_ =
			root.value("nextPageToken").toString();
		int interval_ms =
			root.value("pollingIntervalMillis").toInt(5000);
		poll_interval_ms_ = qMax(interval_ms, 2000);
		poll_timer_->setInterval(poll_interval_ms_);

		QJsonArray items = root.value("items").toArray();
		for (const QJsonValue &val : items) {
			QJsonObject item = val.toObject();
			RecastPlatformEvent evt = parseEventItem(item);
			if (evt.type != EVENT_UNKNOWN)
				emit eventReceived(evt);
		}
	});
}

RecastPlatformEvent RecastYouTubeEvents::parseEventItem(
	const QJsonObject &item)
{
	RecastPlatformEvent evt;
	evt.platform = QStringLiteral("youtube");
	evt.timestamp = QDateTime::currentMSecsSinceEpoch();

	QJsonObject snippet = item.value("snippet").toObject();
	QJsonObject author = item.value("authorDetails").toObject();
	QString msg_type = snippet.value("type").toString();

	evt.username = author.value("channelId").toString();
	evt.displayName = author.value("displayName").toString();

	if (msg_type == "superChatEvent") {
		evt.type = EVENT_SUPER_CHAT;
		QJsonObject details =
			snippet.value("superChatDetails").toObject();
		qint64 micros =
			details.value("amountMicros").toVariant()
				.toLongLong();
		evt.monetaryValue = micros / 1000000.0;
		evt.currency = details.value("currency").toString();
		evt.message = details.value("userComment").toString();

	} else if (msg_type == "superStickerEvent") {
		evt.type = EVENT_SUPER_STICKER;
		QJsonObject details =
			snippet.value("superStickerDetails").toObject();
		qint64 micros =
			details.value("amountMicros").toVariant()
				.toLongLong();
		evt.monetaryValue = micros / 1000000.0;
		evt.currency = details.value("currency").toString();

	} else if (msg_type == "newSponsorEvent") {
		evt.type = EVENT_MEMBER;
		QJsonObject details =
			snippet.value("newSponsorDetails").toObject();
		evt.tier = details.value("memberLevelName").toString();

	} else if (msg_type == "memberMilestoneChatEvent") {
		evt.type = EVENT_MEMBER_MILESTONE;
		QJsonObject details =
			snippet.value("memberMilestoneChatDetails")
				.toObject();
		evt.amount = details.value("memberMonth").toInt();
		evt.message =
			details.value("userComment").toString();
		evt.tier = details.value("memberLevelName").toString();

	} else if (msg_type == "membershipGiftingEvent") {
		evt.type = EVENT_MEMBER_GIFT;
		QJsonObject details =
			snippet.value("membershipGiftingDetails")
				.toObject();
		evt.amount =
			details.value("giftMembershipsCount").toInt();
		evt.tier = details.value("memberLevelName").toString();

	} else if (msg_type == "giftMembershipReceivedEvent") {
		/* Received gift -- treat as member event */
		evt.type = EVENT_MEMBER;
		QJsonObject details =
			snippet.value("giftMembershipReceivedDetails")
				.toObject();
		evt.tier = details.value("memberLevelName").toString();
		evt.message = QString("Gift from %1")
			.arg(details.value("gifterChannelId").toString());

	} else {
		/* Not an event type we care about (regular text, etc.) */
		evt.type = EVENT_UNKNOWN;
	}

	return evt;
}

/* ====================================================================
 * RecastKickEvents -- Kick Events via Pusher WebSocket
 * ==================================================================== */

static const char *KICK_PUSHER_URL =
	"wss://ws-us2.pusher.com/app/"
	"32cbd69e4b950bf97679?protocol=7&client=js&version=7.6.0&flash=false";

RecastKickEvents::RecastKickEvents(QObject *parent)
	: RecastEventProvider(parent)
{
	ws_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest,
			     this);
	net_ = new QNetworkAccessManager(this);

	reconnect_timer_ = new QTimer(this);
	reconnect_timer_->setSingleShot(true);
	connect(reconnect_timer_, &QTimer::timeout,
		this, &RecastKickEvents::onReconnectTimer);

	connect(ws_, &QWebSocket::connected,
		this, &RecastKickEvents::onConnected);
	connect(ws_, &QWebSocket::disconnected,
		this, &RecastKickEvents::onDisconnected);
	connect(ws_, &QWebSocket::textMessageReceived,
		this, &RecastKickEvents::onTextMessageReceived);
}

RecastKickEvents::~RecastKickEvents()
{
	disconnect();
}

void RecastKickEvents::connectToEvents()
{
	if (channel_slug_.isEmpty()) {
		blog(LOG_WARNING,
		     "[Recast Events] Kick channel slug not set, "
		     "call connectToEvents(slug) instead");
		return;
	}
	connectToEvents(channel_slug_);
}

void RecastKickEvents::connectToEvents(const QString &channelSlug)
{
	if (connected_)
		return;

	channel_slug_ = channelSlug;
	blog(LOG_INFO,
	     "[Recast Events] Connecting to Kick events for '%s'...",
	     channel_slug_.toUtf8().constData());

	fetchChannelInfo();
}

void RecastKickEvents::disconnect()
{
	reconnect_timer_->stop();
	channel_id_ = 0;
	chatroom_id_ = 0;

	if (ws_->state() != QAbstractSocket::UnconnectedState)
		ws_->close();

	if (connected_) {
		connected_ = false;
		emit connectionStateChanged(false);
	}
}

bool RecastKickEvents::isConnected() const
{
	return connected_;
}

void RecastKickEvents::fetchChannelInfo()
{
	QUrl api_url(QString("https://kick.com/api/v2/channels/%1")
		.arg(channel_slug_));

	QNetworkRequest req(api_url);
	req.setTransferTimeout(15000);
	req.setRawHeader("Accept", "application/json");

	QNetworkReply *reply = net_->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			blog(LOG_WARNING,
			     "[Recast Events] Kick channel info fetch "
			     "failed: %s",
			     reply->errorString().toUtf8().constData());
			reconnect_timer_->start(5000);
			return;
		}

		QJsonDocument doc =
			QJsonDocument::fromJson(reply->readAll());
		QJsonObject root = doc.object();

		channel_id_ = root.value("id").toInt();
		chatroom_id_ =
			root.value("chatroom").toObject()
				.value("id").toInt();

		if (channel_id_ == 0) {
			blog(LOG_WARNING,
			     "[Recast Events] Kick channel ID not found "
			     "for '%s'",
			     channel_slug_.toUtf8().constData());
			reconnect_timer_->start(5000);
			return;
		}

		blog(LOG_INFO,
		     "[Recast Events] Kick channel=%d chatroom=%d",
		     channel_id_, chatroom_id_);

		connectPusher();
	});
}

void RecastKickEvents::connectPusher()
{
	blog(LOG_INFO, "[Recast Events] Connecting to Kick Pusher...");
	ws_->open(QUrl(QString::fromUtf8(KICK_PUSHER_URL)));
}

void RecastKickEvents::onConnected()
{
	blog(LOG_INFO, "[Recast Events] Kick Pusher connected");
	connected_ = true;
	emit connectionStateChanged(true);
	subscribeToChannels();
}

void RecastKickEvents::onDisconnected()
{
	blog(LOG_WARNING, "[Recast Events] Kick Pusher disconnected");

	if (connected_) {
		connected_ = false;
		emit connectionStateChanged(false);
	}

	if (!reconnect_timer_->isActive())
		reconnect_timer_->start(5000);
}

void RecastKickEvents::onReconnectTimer()
{
	if (channel_slug_.isEmpty())
		return;

	blog(LOG_INFO,
	     "[Recast Events] Attempting Kick events reconnect");

	if (channel_id_ == 0)
		fetchChannelInfo();
	else
		connectPusher();
}

void RecastKickEvents::subscribeToChannels()
{
	/* Subscribe to channel events */
	if (channel_id_ > 0) {
		QJsonObject sub;
		sub["event"] = QStringLiteral("pusher:subscribe");
		QJsonObject sub_data;
		sub_data["auth"] = QString();
		sub_data["channel"] = QString("channel.%1")
			.arg(channel_id_);
		sub["data"] = sub_data;
		ws_->sendTextMessage(
			QString::fromUtf8(
				QJsonDocument(sub).toJson(
					QJsonDocument::Compact)));

		blog(LOG_INFO,
		     "[Recast Events] Subscribed to Kick channel.%d",
		     channel_id_);
	}

	/* Subscribe to chatroom events (for sub/follow events) */
	if (chatroom_id_ > 0) {
		QJsonObject sub;
		sub["event"] = QStringLiteral("pusher:subscribe");
		QJsonObject sub_data;
		sub_data["auth"] = QString();
		sub_data["channel"] = QString("chatrooms.%1.v2")
			.arg(chatroom_id_);
		sub["data"] = sub_data;
		ws_->sendTextMessage(
			QString::fromUtf8(
				QJsonDocument(sub).toJson(
					QJsonDocument::Compact)));

		blog(LOG_INFO,
		     "[Recast Events] Subscribed to Kick chatrooms.%d.v2",
		     chatroom_id_);
	}
}

void RecastKickEvents::onTextMessageReceived(const QString &raw)
{
	QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
	if (!doc.isObject())
		return;

	QJsonObject root = doc.object();
	QString event_name = root.value("event").toString();
	QString data_str = root.value("data").toString();

	/* Pusher internal events */
	if (event_name.startsWith("pusher:"))
		return;

	/* Parse the data string as JSON */
	QJsonDocument data_doc =
		QJsonDocument::fromJson(data_str.toUtf8());
	QJsonObject data;
	if (data_doc.isObject())
		data = data_doc.object();

	RecastPlatformEvent evt = parseKickEvent(event_name, data);
	if (evt.type != EVENT_UNKNOWN)
		emit eventReceived(evt);
}

RecastPlatformEvent RecastKickEvents::parseKickEvent(
	const QString &event_name, const QJsonObject &data)
{
	RecastPlatformEvent evt;
	evt.platform = QStringLiteral("kick");
	evt.timestamp = QDateTime::currentMSecsSinceEpoch();

	if (event_name == "App\\Events\\SubscriptionEvent") {
		evt.type = EVENT_SUBSCRIBE;
		evt.username =
			data.value("username").toString();
		evt.displayName = evt.username;
		evt.amount = data.value("months").toInt(1);

	} else if (event_name ==
		   "App\\Events\\GiftedSubscriptionsEvent") {
		evt.type = EVENT_GIFT_SUB;
		evt.username =
			data.value("gifter_username").toString();
		evt.displayName = evt.username;
		QJsonArray gifted =
			data.value("gifted_usernames").toArray();
		evt.amount = gifted.size();
		if (evt.amount == 0)
			evt.amount = data.value("gifted_count").toInt(1);

	} else if (event_name ==
		   "App\\Events\\LuckyUsersWhoGotGiftSubscriptionsEvent") {
		/* Recipients of gift subs -- skip to avoid duplicates,
		 * the GiftedSubscriptionsEvent covers the gifter */
		evt.type = EVENT_UNKNOWN;

	} else if (event_name == "App\\Events\\FollowersUpdated") {
		evt.type = EVENT_FOLLOW;
		/* FollowersUpdated gives a count, not individual user info */
		evt.username = data.value("username").toString();
		evt.displayName = evt.username;
		if (evt.username.isEmpty()) {
			evt.displayName = QStringLiteral("Someone");
			evt.username = QStringLiteral("unknown");
		}

	} else if (event_name ==
		   "App\\Events\\GiftsLeaderboardUpdated") {
		/* Leaderboard update, not a discrete event */
		evt.type = EVENT_UNKNOWN;

	} else {
		evt.type = EVENT_UNKNOWN;
		blog(LOG_DEBUG,
		     "[Recast Events] Unknown Kick event: %s",
		     event_name.toUtf8().constData());
	}

	return evt;
}

/* ====================================================================
 * RecastEventsDock -- Unified events feed display
 * ==================================================================== */

RecastEventsDock::RecastEventsDock(QWidget *parent)
	: QWidget(parent)
{
	auto *main_layout = new QVBoxLayout(this);
	main_layout->setContentsMargins(0, 0, 0, 0);
	main_layout->setSpacing(0);

	/* Connection indicators bar */
	indicators_widget_ = new QWidget;
	indicators_layout_ = new QHBoxLayout(indicators_widget_);
	indicators_layout_->setContentsMargins(4, 2, 4, 2);
	indicators_layout_->setSpacing(8);
	indicators_layout_->addStretch();
	main_layout->addWidget(indicators_widget_);

	/* Scroll area for events */
	scroll_area_ = new QScrollArea;
	scroll_area_->setWidgetResizable(true);
	scroll_area_->setHorizontalScrollBarPolicy(
		Qt::ScrollBarAlwaysOff);

	events_container_ = new QWidget;
	events_layout_ = new QVBoxLayout(events_container_);
	events_layout_->setAlignment(Qt::AlignTop);
	events_layout_->setContentsMargins(4, 4, 4, 4);
	events_layout_->setSpacing(4);

	/* Empty state label */
	empty_label_ = new QLabel(
		"No events yet. Events will appear here when viewers "
		"follow, subscribe, cheer, and more.");
	empty_label_->setAlignment(Qt::AlignCenter);
	empty_label_->setWordWrap(true);
	empty_label_->setStyleSheet(
		"color: #999; font-style: italic; padding: 24px;");
	events_layout_->addWidget(empty_label_);

	scroll_area_->setWidget(events_container_);
	main_layout->addWidget(scroll_area_);
}

RecastEventsDock::~RecastEventsDock() {}

void RecastEventsDock::addProvider(RecastEventProvider *provider)
{
	if (!provider)
		return;

	providers_.push_back(provider);

	connect(provider, &RecastEventProvider::eventReceived,
		this, &RecastEventsDock::onEventReceived);
	connect(provider, &RecastEventProvider::connectionStateChanged,
		this, &RecastEventsDock::onConnectionStateChanged);

	/* Create indicator label */
	auto *label = new QLabel;
	label->setFixedSize(16, 16);
	QString plat = provider->platform();
	QIcon icon = recast_platform_icon(plat);
	label->setPixmap(icon.pixmap(16, 16));
	label->setToolTip(QString("%1: disconnected").arg(plat));
	label->setStyleSheet("opacity: 0.4;");

	/* Insert before the stretch */
	indicators_layout_->insertWidget(
		indicators_layout_->count() - 1, label);
	indicator_labels_[provider] = label;

	updateIndicator(provider, provider->isConnected());
}

void RecastEventsDock::removeProvider(RecastEventProvider *provider)
{
	if (!provider)
		return;

	QObject::disconnect(provider, nullptr, this, nullptr);

	auto it = std::find(providers_.begin(), providers_.end(), provider);
	if (it != providers_.end())
		providers_.erase(it);

	auto label_it = indicator_labels_.find(provider);
	if (label_it != indicator_labels_.end()) {
		indicators_layout_->removeWidget(label_it.value());
		label_it.value()->deleteLater();
		indicator_labels_.erase(label_it);
	}
}

void RecastEventsDock::onEventReceived(const RecastPlatformEvent &event)
{
	QFrame *card = createEventCard(event);

	/* Hide empty label */
	empty_label_->setVisible(false);

	/* Insert at top (newest first) */
	events_layout_->insertWidget(0, card);
	event_count_++;

	/* Remove oldest if over limit */
	while (event_count_ > MAX_EVENTS) {
		int last_idx = events_layout_->count() - 1;
		/* Skip the empty label if it's the last widget */
		QLayoutItem *item = events_layout_->itemAt(last_idx);
		if (item && item->widget() &&
		    item->widget() != empty_label_) {
			QWidget *w = item->widget();
			events_layout_->removeWidget(w);
			w->deleteLater();
			event_count_--;
		} else if (last_idx > 1) {
			item = events_layout_->itemAt(last_idx - 1);
			if (item && item->widget()) {
				QWidget *w = item->widget();
				events_layout_->removeWidget(w);
				w->deleteLater();
				event_count_--;
			}
		} else {
			break;
		}
	}

	/* Scroll to top to show newest event */
	QScrollBar *sb = scroll_area_->verticalScrollBar();
	if (sb)
		sb->setValue(sb->minimum());
}

void RecastEventsDock::onConnectionStateChanged(bool connected)
{
	auto *provider =
		qobject_cast<RecastEventProvider *>(sender());
	if (provider)
		updateIndicator(provider, connected);
}

void RecastEventsDock::updateIndicator(RecastEventProvider *provider,
				       bool connected)
{
	auto it = indicator_labels_.find(provider);
	if (it == indicator_labels_.end())
		return;

	QLabel *label = it.value();
	QString plat = provider->platform();

	if (connected) {
		label->setToolTip(
			QString("%1: connected").arg(plat));
		label->setStyleSheet("");
	} else {
		label->setToolTip(
			QString("%1: disconnected").arg(plat));
		label->setStyleSheet("opacity: 0.4;");
	}
}

QFrame *RecastEventsDock::createEventCard(const RecastPlatformEvent &event)
{
	auto *card = new QFrame;
	card->setFrameShape(QFrame::StyledPanel);

	QString border_color = eventTypeColor(event.type);
	card->setStyleSheet(
		QString("QFrame { border-left: 3px solid %1; "
			"background: transparent; padding: 4px; }"
			"QFrame:hover { background: rgba(255,255,255,15); }")
			.arg(border_color));

	auto *layout = new QVBoxLayout(card);
	layout->setContentsMargins(8, 4, 8, 4);
	layout->setSpacing(2);

	/* Top row: platform icon + event type label */
	auto *top_row = new QHBoxLayout;
	top_row->setSpacing(6);

	/* Platform icon */
	auto *platform_icon = new QLabel;
	platform_icon->setFixedSize(16, 16);
	QIcon icon = recast_platform_icon(event.platform);
	platform_icon->setPixmap(icon.pixmap(16, 16));
	top_row->addWidget(platform_icon);

	/* Event type label */
	auto *type_label = new QLabel(eventTypeLabel(event.type));
	type_label->setStyleSheet(
		QString("color: %1; font-weight: bold; font-size: 11px;")
			.arg(border_color));
	top_row->addWidget(type_label);

	top_row->addStretch();

	/* Timestamp */
	QDateTime dt = QDateTime::fromMSecsSinceEpoch(event.timestamp);
	auto *time_label = new QLabel(dt.toString("hh:mm:ss"));
	time_label->setStyleSheet("color: #888; font-size: 10px;");
	top_row->addWidget(time_label);

	layout->addLayout(top_row);

	/* Description text */
	auto *desc_label = new QLabel(eventDescription(event));
	desc_label->setWordWrap(true);
	desc_label->setStyleSheet("font-size: 12px;");
	layout->addWidget(desc_label);

	/* Monetary value if applicable */
	if (event.monetaryValue > 0.0) {
		QString value_text;
		if (!event.currency.isEmpty())
			value_text = QString("%1 %2")
				.arg(event.monetaryValue, 0, 'f', 2)
				.arg(event.currency);
		else
			value_text = QString("$%1")
				.arg(event.monetaryValue, 0, 'f', 2);

		auto *value_label = new QLabel(value_text);
		value_label->setStyleSheet(
			"color: #FFD700; font-weight: bold; font-size: 12px;");
		layout->addWidget(value_label);
	}

	return card;
}

QString RecastEventsDock::eventDescription(
	const RecastPlatformEvent &event) const
{
	QString name = escapeHtml(
		event.displayName.isEmpty()
			? event.username : event.displayName);

	switch (event.type) {
	case EVENT_FOLLOW:
		return QString("<b>%1</b> followed!").arg(name);

	case EVENT_SUBSCRIBE: {
		QString tier_str;
		if (event.tier == "1000")
			tier_str = "Tier 1";
		else if (event.tier == "2000")
			tier_str = "Tier 2";
		else if (event.tier == "3000")
			tier_str = "Tier 3";
		else if (!event.tier.isEmpty())
			tier_str = event.tier;
		else
			tier_str = "Tier 1";
		return QString("<b>%1</b> subscribed at %2!")
			.arg(name, tier_str);
	}

	case EVENT_GIFT_SUB: {
		QString tier_str;
		if (event.tier == "1000")
			tier_str = "Tier 1";
		else if (event.tier == "2000")
			tier_str = "Tier 2";
		else if (event.tier == "3000")
			tier_str = "Tier 3";
		else if (!event.tier.isEmpty())
			tier_str = event.tier;
		else
			tier_str = "Tier 1";
		return QString("<b>%1</b> gifted %2 %3 subs!")
			.arg(name).arg(event.amount).arg(tier_str);
	}

	case EVENT_RESUB:
		return QString("<b>%1</b> resubscribed for %2 months!")
			.arg(name).arg(event.amount);

	case EVENT_BITS:
		return QString("<b>%1</b> cheered %2 bits!")
			.arg(name).arg(event.amount);

	case EVENT_SUPER_CHAT: {
		QString currency = event.currency.isEmpty()
			? QStringLiteral("$") : event.currency;
		return QString("<b>%1</b> sent a Super Chat: %2%3!")
			.arg(name, currency)
			.arg(event.monetaryValue, 0, 'f', 2);
	}

	case EVENT_SUPER_STICKER: {
		QString currency = event.currency.isEmpty()
			? QStringLiteral("$") : event.currency;
		return QString("<b>%1</b> sent a Super Sticker: %2%3!")
			.arg(name, currency)
			.arg(event.monetaryValue, 0, 'f', 2);
	}

	case EVENT_MEMBER:
		return QString("<b>%1</b> became a member!")
			.arg(name);

	case EVENT_MEMBER_MILESTONE:
		return QString("<b>%1</b> has been a member for %2 months!")
			.arg(name).arg(event.amount);

	case EVENT_MEMBER_GIFT:
		return QString("<b>%1</b> gifted %2 memberships!")
			.arg(name).arg(event.amount);

	case EVENT_RAID:
		return QString("<b>%1</b> raided with %2 viewers!")
			.arg(name).arg(event.amount);

	case EVENT_CHANNEL_POINTS:
		return QString("<b>%1</b> redeemed %2!")
			.arg(name, escapeHtml(event.message));

	case EVENT_HYPE_TRAIN:
		return QString("Hype Train level %1!")
			.arg(event.amount);

	case EVENT_POLL:
		return QString("Poll: %1")
			.arg(escapeHtml(event.message));

	case EVENT_UNKNOWN:
	default:
		return QString("<b>%1</b> triggered an event")
			.arg(name);
	}
}

QString RecastEventsDock::eventTypeLabel(RecastEventType type) const
{
	switch (type) {
	case EVENT_FOLLOW:           return QStringLiteral("NEW FOLLOW");
	case EVENT_SUBSCRIBE:        return QStringLiteral("SUBSCRIPTION");
	case EVENT_GIFT_SUB:         return QStringLiteral("GIFT SUBS");
	case EVENT_RESUB:            return QStringLiteral("RESUB");
	case EVENT_BITS:             return QStringLiteral("BITS");
	case EVENT_SUPER_CHAT:       return QStringLiteral("SUPER CHAT");
	case EVENT_SUPER_STICKER:    return QStringLiteral("SUPER STICKER");
	case EVENT_MEMBER:           return QStringLiteral("NEW MEMBER");
	case EVENT_MEMBER_MILESTONE: return QStringLiteral("MILESTONE");
	case EVENT_MEMBER_GIFT:      return QStringLiteral("GIFT MEMBERS");
	case EVENT_RAID:             return QStringLiteral("RAID");
	case EVENT_CHANNEL_POINTS:   return QStringLiteral("REDEMPTION");
	case EVENT_HYPE_TRAIN:       return QStringLiteral("HYPE TRAIN");
	case EVENT_POLL:             return QStringLiteral("POLL");
	case EVENT_UNKNOWN:
	default:                     return QStringLiteral("EVENT");
	}
}

QString RecastEventsDock::eventTypeColor(RecastEventType type) const
{
	switch (type) {
	case EVENT_FOLLOW:           return QStringLiteral("#4CAF50"); /* green */
	case EVENT_SUBSCRIBE:        return QStringLiteral("#9C27B0"); /* purple */
	case EVENT_GIFT_SUB:         return QStringLiteral("#AB47BC"); /* light purple */
	case EVENT_RESUB:            return QStringLiteral("#7B1FA2"); /* dark purple */
	case EVENT_BITS:             return QStringLiteral("#FF9800"); /* orange */
	case EVENT_SUPER_CHAT:       return QStringLiteral("#FFEB3B"); /* yellow */
	case EVENT_SUPER_STICKER:    return QStringLiteral("#FDD835"); /* amber */
	case EVENT_MEMBER:           return QStringLiteral("#4CAF50"); /* green */
	case EVENT_MEMBER_MILESTONE: return QStringLiteral("#66BB6A"); /* light green */
	case EVENT_MEMBER_GIFT:      return QStringLiteral("#AB47BC"); /* light purple */
	case EVENT_RAID:             return QStringLiteral("#F44336"); /* red */
	case EVENT_CHANNEL_POINTS:   return QStringLiteral("#2196F3"); /* blue */
	case EVENT_HYPE_TRAIN:       return QStringLiteral("#E91E63"); /* pink */
	case EVENT_POLL:             return QStringLiteral("#00BCD4"); /* cyan */
	case EVENT_UNKNOWN:
	default:                     return QStringLiteral("#999999"); /* gray */
	}
}

QString RecastEventsDock::escapeHtml(const QString &text)
{
	QString out = text;
	out.replace('&', "&amp;");
	out.replace('<', "&lt;");
	out.replace('>', "&gt;");
	out.replace('"', "&quot;");
	return out;
}
