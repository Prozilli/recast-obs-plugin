#pragma once

#include <QObject>
#include <QDialog>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QMap>
#include <QMutex>
#include <QString>
#include <QTcpServer>

extern "C" {
#include <obs.h>
}

/*
 * Per-platform token bundle stored in config JSON under "auth".
 */
struct RecastAuthToken {
	QString client_id;
	QString client_secret;
	QString access_token;
	QString refresh_token;
	QString user_id;
	QString user_name;
	qint64 expires_at = 0; /* seconds since epoch */
};

/*
 * RecastAuthManager -- Singleton managing OAuth tokens for Twitch and YouTube.
 *
 * Handles device-code (Twitch) and authorization-code+PKCE (YouTube) flows,
 * stores tokens in the plugin config JSON, and auto-refreshes before expiry.
 */
class RecastAuthManager : public QObject {
	Q_OBJECT

public:
	static RecastAuthManager *instance();
	static void destroyInstance();

	bool isAuthenticated(const QString &platform) const;
	QString accessToken(const QString &platform) const;
	QString clientId(const QString &platform) const;
	QString userId(const QString &platform) const;
	QString userName(const QString &platform) const;

	void startAuth(const QString &platform);
	void logout(const QString &platform);

	void loadFromConfig(obs_data_t *auth_data);
	obs_data_t *saveToConfig() const;

	void refreshTokenIfNeeded(const QString &platform);

signals:
	void authStateChanged(const QString &platform, bool authenticated);
	void authError(const QString &platform, const QString &error);

private:
	explicit RecastAuthManager(QObject *parent = nullptr);
	~RecastAuthManager();

	static RecastAuthManager *instance_;
	static QMutex instance_mutex_;

	QNetworkAccessManager *net_;
	QMap<QString, RecastAuthToken> tokens_;
	QTimer *refresh_timer_;

	/* Twitch device-code flow */
	void startTwitchAuth();
	void pollTwitchDeviceCode(const QString &device_code,
				  const QString &client_id, int interval);
	void fetchTwitchUserInfo(const QString &client_id,
				 const QString &access_token);
	void validateTwitchToken(const QString &access_token);
	void refreshTwitchToken();

	/* YouTube auth-code + PKCE flow */
	void startYouTubeAuth();
	void exchangeYouTubeCode(const QString &code,
				 const QString &code_verifier,
				 const QString &redirect_uri);
	void fetchYouTubeChannelInfo(const QString &access_token);
	void refreshYouTubeToken();

	/* Loopback server for YouTube */
	QTcpServer *loopback_server_ = nullptr;

	/* Refresh timer handler */
	void onRefreshTimer();
};

/*
 * RecastAuthDialog -- Settings dialog showing connected accounts.
 *
 * For each platform (Twitch, YouTube) shows the connected username
 * or "Not Connected", plus Connect/Disconnect and Client ID fields.
 */
class RecastAuthDialog : public QDialog {
	Q_OBJECT

public:
	explicit RecastAuthDialog(QWidget *parent = nullptr);

private slots:
	void onAuthStateChanged(const QString &platform, bool authenticated);
	void onAuthError(const QString &platform, const QString &error);

private:
	/* Twitch widgets */
	QLabel *twitch_status_;
	QPushButton *twitch_btn_;
	QLineEdit *twitch_client_id_;

	/* YouTube widgets */
	QLabel *youtube_status_;
	QPushButton *youtube_btn_;
	QLineEdit *youtube_client_id_;
	QLineEdit *youtube_client_secret_;

	void updatePlatformUI(const QString &platform);
};
