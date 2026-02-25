#pragma once

#include <QObject>
#include <QWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QWebSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QString>
#include <QJsonObject>

#include <vector>

extern "C" {
#include <obs.h>
}

/* ---- Event types shared by all platforms ---- */

enum RecastEventType {
	EVENT_FOLLOW,
	EVENT_SUBSCRIBE,
	EVENT_GIFT_SUB,
	EVENT_RESUB,
	EVENT_BITS,
	EVENT_SUPER_CHAT,
	EVENT_SUPER_STICKER,
	EVENT_MEMBER,
	EVENT_MEMBER_MILESTONE,
	EVENT_MEMBER_GIFT,
	EVENT_RAID,
	EVENT_CHANNEL_POINTS,
	EVENT_HYPE_TRAIN,
	EVENT_POLL,
	EVENT_UNKNOWN,
};

/* ---- Platform event data ---- */

struct RecastPlatformEvent {
	RecastEventType type = EVENT_UNKNOWN;
	QString platform;       /* twitch, youtube, kick */
	QString username;       /* who triggered it */
	QString displayName;
	QString message;        /* optional text */
	int amount = 0;         /* bits, months, viewers, gift count, etc. */
	QString tier;           /* sub tier: "1000"/"2000"/"3000" for Twitch; level name for YouTube */
	double monetaryValue = 0.0; /* USD value for super chats, bits value, etc. */
	QString currency;       /* ISO currency code */
	bool isAnonymous = false;
	qint64 timestamp = 0;
};

/* ---- Abstract event provider ---- */

class RecastEventProvider : public QObject {
	Q_OBJECT

public:
	explicit RecastEventProvider(QObject *parent = nullptr)
		: QObject(parent) {}
	virtual ~RecastEventProvider() = default;

	virtual void connectToEvents() = 0;
	virtual void disconnect() = 0;
	virtual bool isConnected() const = 0;
	virtual QString platform() const = 0;

signals:
	void eventReceived(const RecastPlatformEvent &event);
	void connectionStateChanged(bool connected);
};

/* ---- Twitch EventSub via WebSocket ---- */

class RecastTwitchEvents : public RecastEventProvider {
	Q_OBJECT

public:
	explicit RecastTwitchEvents(QObject *parent = nullptr);
	~RecastTwitchEvents() override;

	void connectToEvents() override;
	void disconnect() override;
	bool isConnected() const override;
	QString platform() const override { return QStringLiteral("twitch"); }

private slots:
	void onConnected();
	void onDisconnected();
	void onTextMessageReceived(const QString &raw);
	void onReconnectTimer();
	void onKeepaliveTimeout();

private:
	QWebSocket *ws_ = nullptr;
	QWebSocket *reconnect_ws_ = nullptr;
	QNetworkAccessManager *net_mgr_ = nullptr;
	QTimer *reconnect_timer_ = nullptr;
	QTimer *keepalive_timer_ = nullptr;
	QString session_id_;
	bool connected_ = false;

	void createSubscription(const QString &type, const QString &version,
				const QJsonObject &condition);
	void createAllSubscriptions();
	RecastPlatformEvent parseNotification(const QString &sub_type,
					      const QJsonObject &event_data);
};

/* ---- YouTube Live Events via polling ---- */

class RecastYouTubeEvents : public RecastEventProvider {
	Q_OBJECT

public:
	explicit RecastYouTubeEvents(QObject *parent = nullptr);
	~RecastYouTubeEvents() override;

	void connectToEvents() override;
	void disconnect() override;
	bool isConnected() const override;
	QString platform() const override { return QStringLiteral("youtube"); }

private slots:
	void onPollTimer();
	void onReconnectTimer();

private:
	QNetworkAccessManager *net_ = nullptr;
	QTimer *poll_timer_ = nullptr;
	QTimer *reconnect_timer_ = nullptr;
	QString live_chat_id_;
	QString next_page_token_;
	int poll_interval_ms_ = 5000;
	bool connected_ = false;

	void fetchLiveChatId();
	void pollMessages();
	RecastPlatformEvent parseEventItem(const QJsonObject &item);
};

/* ---- Kick Events via Pusher WebSocket ---- */

class RecastKickEvents : public RecastEventProvider {
	Q_OBJECT

public:
	explicit RecastKickEvents(QObject *parent = nullptr);
	~RecastKickEvents() override;

	void connectToEvents() override;
	void connectToEvents(const QString &channelSlug);
	void disconnect() override;
	bool isConnected() const override;
	QString platform() const override { return QStringLiteral("kick"); }

private slots:
	void onConnected();
	void onDisconnected();
	void onTextMessageReceived(const QString &raw);
	void onReconnectTimer();

private:
	QWebSocket *ws_ = nullptr;
	QNetworkAccessManager *net_ = nullptr;
	QTimer *reconnect_timer_ = nullptr;
	QString channel_slug_;
	int channel_id_ = 0;
	int chatroom_id_ = 0;
	bool connected_ = false;

	void fetchChannelInfo();
	void connectPusher();
	void subscribeToChannels();
	RecastPlatformEvent parseKickEvent(const QString &event_name,
					   const QJsonObject &data);
};

/* ---- Unified events feed dock ---- */

class RecastEventsDock : public QWidget {
	Q_OBJECT

public:
	explicit RecastEventsDock(QWidget *parent = nullptr);
	~RecastEventsDock() override;

	void addProvider(RecastEventProvider *provider);
	void removeProvider(RecastEventProvider *provider);

private slots:
	void onEventReceived(const RecastPlatformEvent &event);
	void onConnectionStateChanged(bool connected);

private:
	QScrollArea *scroll_area_ = nullptr;
	QVBoxLayout *events_layout_ = nullptr;
	QWidget *events_container_ = nullptr;
	QLabel *empty_label_ = nullptr;
	QHBoxLayout *indicators_layout_ = nullptr;
	QWidget *indicators_widget_ = nullptr;

	std::vector<RecastEventProvider *> providers_;
	QMap<RecastEventProvider *, QLabel *> indicator_labels_;
	int event_count_ = 0;

	static const int MAX_EVENTS = 200;

	QFrame *createEventCard(const RecastPlatformEvent &event);
	QString eventDescription(const RecastPlatformEvent &event) const;
	QString eventTypeLabel(RecastEventType type) const;
	QString eventTypeColor(RecastEventType type) const;
	void updateIndicator(RecastEventProvider *provider, bool connected);
	static QString escapeHtml(const QString &text);
};
