#pragma once

#include <QObject>
#include <QWidget>
#include <QTextBrowser>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QWebSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QColor>
#include <QString>
#include <QScrollBar>

#include <vector>

extern "C" {
#include <obs.h>
}

/* ---- Chat message shared by all providers ---- */

struct RecastChatMessage {
	QString platform;    /* twitch, youtube, kick */
	QString username;
	QString displayName;
	QString message;
	QColor nameColor;
	bool isMod = false;
	bool isSub = false;
	bool isOwner = false;
	qint64 timestamp = 0;
};

/* ---- Abstract chat provider ---- */

class RecastChatProvider : public QObject {
	Q_OBJECT

public:
	explicit RecastChatProvider(QObject *parent = nullptr)
		: QObject(parent) {}
	virtual ~RecastChatProvider() = default;

	virtual void connectToChat(const QString &channel) = 0;
	virtual void disconnect() = 0;
	virtual void sendMessage(const QString &msg) = 0;
	virtual bool isConnected() const = 0;
	virtual QString platform() const = 0;

signals:
	void messageReceived(const RecastChatMessage &msg);
	void connectionStateChanged(bool connected);
};

/* ---- Twitch IRC via WebSocket ---- */

class RecastTwitchChat : public RecastChatProvider {
	Q_OBJECT

public:
	explicit RecastTwitchChat(QObject *parent = nullptr);
	~RecastTwitchChat() override;

	void connectToChat(const QString &channel) override;
	void disconnect() override;
	void sendMessage(const QString &msg) override;
	bool isConnected() const override;
	QString platform() const override { return QStringLiteral("twitch"); }

private slots:
	void onConnected();
	void onDisconnected();
	void onTextMessageReceived(const QString &raw);
	void onReconnectTimer();

private:
	QWebSocket *ws_ = nullptr;
	QTimer *reconnect_timer_ = nullptr;
	QString channel_;
	bool connected_ = false;

	void parseLine(const QString &line);
	RecastChatMessage parsePrivmsg(const QString &tags,
				       const QString &prefix,
				       const QString &trailing);
};

/* ---- YouTube Live Chat via polling ---- */

class RecastYouTubeChat : public RecastChatProvider {
	Q_OBJECT

public:
	explicit RecastYouTubeChat(QObject *parent = nullptr);
	~RecastYouTubeChat() override;

	void connectToChat(const QString &channel) override;
	void disconnect() override;
	void sendMessage(const QString &msg) override;
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
};

/* ---- Kick chat via Pusher WebSocket ---- */

class RecastKickChat : public RecastChatProvider {
	Q_OBJECT

public:
	explicit RecastKickChat(QObject *parent = nullptr);
	~RecastKickChat() override;

	void connectToChat(const QString &channel) override;
	void disconnect() override;
	void sendMessage(const QString &msg) override;
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
	int chatroom_id_ = 0;
	bool connected_ = false;

	void fetchChannelInfo();
	void connectPusher();
	void subscribeToChatroom();
	void parseChatMessageEvent(const QString &data_str);
};

/* ---- Unified chat dock ---- */

class RecastChatDock : public QWidget {
	Q_OBJECT

public:
	explicit RecastChatDock(QWidget *parent = nullptr);
	~RecastChatDock() override;

	void addProvider(RecastChatProvider *provider);
	void removeProvider(RecastChatProvider *provider);

private slots:
	void onMessageReceived(const RecastChatMessage &msg);
	void onConnectionStateChanged(bool connected);
	void onSendClicked();

private:
	QTextBrowser *chat_display_ = nullptr;
	QLineEdit *input_ = nullptr;
	QPushButton *send_btn_ = nullptr;
	QHBoxLayout *indicators_layout_ = nullptr;
	QWidget *indicators_widget_ = nullptr;

	std::vector<RecastChatProvider *> providers_;
	QMap<RecastChatProvider *, QLabel *> indicator_labels_;
	int message_count_ = 0;

	static const int MAX_MESSAGES = 500;

	void appendMessage(const RecastChatMessage &msg);
	void updateIndicator(RecastChatProvider *provider, bool connected);
	bool isScrolledToBottom() const;
	QString platformColor(const QString &platform) const;
	QString platformLetter(const QString &platform) const;
	static QString escapeHtml(const QString &text);
};
