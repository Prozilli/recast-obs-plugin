/*
 * recast-auth.cpp -- OAuth token management for Twitch and YouTube.
 *
 * Twitch uses Device Code Grant, YouTube uses Authorization Code + PKCE
 * with a loopback HTTP server. Tokens are stored in the plugin config
 * JSON under an "auth" key and auto-refreshed before expiry.
 */

#include "recast-auth.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

extern "C" {
#include <obs-module.h>
}

/* ====================================================================
 * RecastAuthManager -- Singleton
 * ==================================================================== */

RecastAuthManager *RecastAuthManager::instance_ = nullptr;
QMutex RecastAuthManager::instance_mutex_;

RecastAuthManager::RecastAuthManager(QObject *parent)
	: QObject(parent)
{
	net_ = new QNetworkAccessManager(this);

	refresh_timer_ = new QTimer(this);
	refresh_timer_->setInterval(60 * 1000); /* check every 60s */
	connect(refresh_timer_, &QTimer::timeout,
		this, &RecastAuthManager::onRefreshTimer);
	refresh_timer_->start();
}

RecastAuthManager::~RecastAuthManager()
{
	refresh_timer_->stop();

	if (loopback_server_) {
		loopback_server_->close();
		loopback_server_->deleteLater();
		loopback_server_ = nullptr;
	}
}

RecastAuthManager *RecastAuthManager::instance()
{
	QMutexLocker lock(&instance_mutex_);
	if (!instance_)
		instance_ = new RecastAuthManager();
	return instance_;
}

void RecastAuthManager::destroyInstance()
{
	delete instance_;
	instance_ = nullptr;
}

/* ---- Public accessors ---- */

bool RecastAuthManager::isAuthenticated(const QString &platform) const
{
	if (!tokens_.contains(platform))
		return false;
	const RecastAuthToken &t = tokens_[platform];
	return !t.access_token.isEmpty();
}

QString RecastAuthManager::accessToken(const QString &platform) const
{
	if (!tokens_.contains(platform))
		return QString();
	return tokens_[platform].access_token;
}

QString RecastAuthManager::clientId(const QString &platform) const
{
	if (!tokens_.contains(platform))
		return QString();
	return tokens_[platform].client_id;
}

QString RecastAuthManager::userId(const QString &platform) const
{
	if (!tokens_.contains(platform))
		return QString();
	return tokens_[platform].user_id;
}

QString RecastAuthManager::userName(const QString &platform) const
{
	if (!tokens_.contains(platform))
		return QString();
	return tokens_[platform].user_name;
}

/* ---- Auth entry point ---- */

void RecastAuthManager::startAuth(const QString &platform)
{
	if (platform == "twitch")
		startTwitchAuth();
	else if (platform == "youtube")
		startYouTubeAuth();
	else
		emit authError(platform, "Unknown platform: " + platform);
}

void RecastAuthManager::logout(const QString &platform)
{
	if (tokens_.contains(platform)) {
		tokens_.remove(platform);
		blog(LOG_INFO, "[Recast] Logged out of %s",
		     platform.toUtf8().constData());
		emit authStateChanged(platform, false);
	}
}

/* ---- Config persistence ---- */

void RecastAuthManager::loadFromConfig(obs_data_t *auth_data)
{
	if (!auth_data)
		return;

	QStringList platforms = {"twitch", "youtube"};
	for (const QString &p : platforms) {
		obs_data_t *pd = obs_data_get_obj(auth_data,
						  p.toUtf8().constData());
		if (!pd)
			continue;

		RecastAuthToken t;
		t.client_id = QString::fromUtf8(
			obs_data_get_string(pd, "client_id"));
		t.client_secret = QString::fromUtf8(
			obs_data_get_string(pd, "client_secret"));
		t.access_token = QString::fromUtf8(
			obs_data_get_string(pd, "access_token"));
		t.refresh_token = QString::fromUtf8(
			obs_data_get_string(pd, "refresh_token"));
		t.user_id = QString::fromUtf8(
			obs_data_get_string(pd, "user_id"));
		t.user_name = QString::fromUtf8(
			obs_data_get_string(pd, "user_name"));
		t.expires_at = (qint64)obs_data_get_int(pd, "expires_at");

		tokens_[p] = t;
		obs_data_release(pd);

		if (!t.access_token.isEmpty()) {
			blog(LOG_INFO,
			     "[Recast] Loaded auth for %s (user: %s)",
			     p.toUtf8().constData(),
			     t.user_name.toUtf8().constData());
		}
	}
}

obs_data_t *RecastAuthManager::saveToConfig() const
{
	obs_data_t *auth_data = obs_data_create();

	for (auto it = tokens_.constBegin(); it != tokens_.constEnd(); ++it) {
		const QString &platform = it.key();
		const RecastAuthToken &t = it.value();

		obs_data_t *pd = obs_data_create();
		obs_data_set_string(pd, "client_id",
				    t.client_id.toUtf8().constData());
		obs_data_set_string(pd, "client_secret",
				    t.client_secret.toUtf8().constData());
		obs_data_set_string(pd, "access_token",
				    t.access_token.toUtf8().constData());
		obs_data_set_string(pd, "refresh_token",
				    t.refresh_token.toUtf8().constData());
		obs_data_set_string(pd, "user_id",
				    t.user_id.toUtf8().constData());
		obs_data_set_string(pd, "user_name",
				    t.user_name.toUtf8().constData());
		obs_data_set_int(pd, "expires_at", (long long)t.expires_at);

		obs_data_set_obj(auth_data,
				 platform.toUtf8().constData(), pd);
		obs_data_release(pd);
	}

	return auth_data;
}

/* ---- Token refresh ---- */

void RecastAuthManager::refreshTokenIfNeeded(const QString &platform)
{
	if (!tokens_.contains(platform))
		return;

	const RecastAuthToken &t = tokens_[platform];
	if (t.refresh_token.isEmpty())
		return;

	qint64 now = QDateTime::currentSecsSinceEpoch();
	qint64 margin = 5 * 60; /* refresh 5 minutes before expiry */

	if (t.expires_at > 0 && now >= (t.expires_at - margin)) {
		blog(LOG_INFO, "[Recast] Refreshing %s token (expires_at=%lld, now=%lld)",
		     platform.toUtf8().constData(),
		     (long long)t.expires_at, (long long)now);

		if (platform == "twitch")
			refreshTwitchToken();
		else if (platform == "youtube")
			refreshYouTubeToken();
	}
}

void RecastAuthManager::onRefreshTimer()
{
	QStringList platforms = {"twitch", "youtube"};
	for (const QString &p : platforms) {
		if (isAuthenticated(p))
			refreshTokenIfNeeded(p);
	}
}

/* ====================================================================
 * Twitch Device Code Grant
 * ==================================================================== */

void RecastAuthManager::startTwitchAuth()
{
	QString client_id;
	if (tokens_.contains("twitch") &&
	    !tokens_["twitch"].client_id.isEmpty()) {
		client_id = tokens_["twitch"].client_id;
	}

	if (client_id.isEmpty()) {
		emit authError("twitch",
			"No Twitch Client ID configured. "
			"Please enter a Client ID first.");
		return;
	}

	/* POST to device authorization endpoint */
	QUrl url("https://id.twitch.tv/oauth2/device");
	QUrlQuery params;
	params.addQueryItem("client_id", client_id);
	params.addQueryItem("scopes",
		"chat:read chat:edit moderator:read:followers "
		"channel:read:subscriptions bits:read "
		"channel:read:redemptions channel:read:hype_train");

	QNetworkRequest req(url);
	req.setTransferTimeout(15000);
	req.setHeader(QNetworkRequest::ContentTypeHeader,
		      "application/x-www-form-urlencoded");

	QNetworkReply *reply = net_->post(req, params.toString(
		QUrl::FullyEncoded).toUtf8());

	connect(reply, &QNetworkReply::finished, this, [this, reply, client_id]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			emit authError("twitch",
				"Device code request failed: " +
				reply->errorString());
			return;
		}

		QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
		QJsonObject obj = doc.object();

		QString device_code = obj.value("device_code").toString();
		QString user_code = obj.value("user_code").toString();
		QString verification_uri = obj.value("verification_uri").toString();
		int interval = obj.value("interval").toInt(5);

		if (device_code.isEmpty() || user_code.isEmpty()) {
			emit authError("twitch",
				"Invalid device code response from Twitch.");
			return;
		}

		blog(LOG_INFO,
		     "[Recast] Twitch device code obtained, user_code=%s",
		     user_code.toUtf8().constData());

		/* Show dialog with user code and verification URI */
		QDialog *waitDlg = new QDialog(
			qobject_cast<QWidget *>(parent()));
		waitDlg->setWindowTitle("Twitch Authorization");
		waitDlg->setAttribute(Qt::WA_DeleteOnClose);
		waitDlg->setMinimumWidth(400);

		auto *layout = new QVBoxLayout(waitDlg);

		auto *info = new QLabel(
			QString("Go to <a href=\"%1\">%1</a> and enter "
				"this code:")
				.arg(verification_uri));
		info->setOpenExternalLinks(true);
		info->setWordWrap(true);
		layout->addWidget(info);

		auto *codeLabel = new QLabel(user_code);
		codeLabel->setAlignment(Qt::AlignCenter);
		codeLabel->setStyleSheet(
			"font-size: 28px; font-weight: bold; "
			"padding: 16px; background: #333; "
			"border-radius: 8px; color: #fff;");
		codeLabel->setTextInteractionFlags(
			Qt::TextSelectableByMouse);
		layout->addWidget(codeLabel);

		auto *waiting = new QLabel("Waiting for authorization...");
		waiting->setAlignment(Qt::AlignCenter);
		waiting->setStyleSheet("color: #999; font-style: italic;");
		layout->addWidget(waiting);

		auto *cancelBtn = new QPushButton("Cancel");
		layout->addWidget(cancelBtn);

		connect(cancelBtn, &QPushButton::clicked,
			waitDlg, &QDialog::reject);

		/* Open browser automatically */
		QDesktopServices::openUrl(QUrl(verification_uri));

		waitDlg->show();

		/* Start polling */
		auto *pollTimer = new QTimer(waitDlg);
		pollTimer->setInterval(interval * 1000);

		connect(pollTimer, &QTimer::timeout, this,
			[this, device_code, client_id, waitDlg, pollTimer]() {
			QUrl tokenUrl("https://id.twitch.tv/oauth2/token");
			QUrlQuery tokenParams;
			tokenParams.addQueryItem("client_id", client_id);
			tokenParams.addQueryItem("device_code", device_code);
			tokenParams.addQueryItem("grant_type",
				"urn:ietf:params:oauth:grant-type:device_code");

			QNetworkRequest tokenReq(tokenUrl);
			tokenReq.setHeader(
				QNetworkRequest::ContentTypeHeader,
				"application/x-www-form-urlencoded");

			QNetworkReply *tokenReply = net_->post(
				tokenReq,
				tokenParams.toString(
					QUrl::FullyEncoded).toUtf8());

			connect(tokenReply, &QNetworkReply::finished, this,
				[this, tokenReply, client_id, waitDlg,
				 pollTimer]() {
				tokenReply->deleteLater();

				QJsonDocument tdoc = QJsonDocument::fromJson(
					tokenReply->readAll());
				QJsonObject tobj = tdoc.object();

				/* Check for pending status */
				if (tobj.contains("message")) {
					/* Still waiting -- keep polling */
					return;
				}

				if (tobj.contains("access_token")) {
					pollTimer->stop();

					QString access =
						tobj.value("access_token")
							.toString();
					QString refresh =
						tobj.value("refresh_token")
							.toString();
					int expires_in =
						tobj.value("expires_in")
							.toInt(0);

					RecastAuthToken &t = tokens_["twitch"];
					t.client_id = client_id;
					t.access_token = access;
					t.refresh_token = refresh;
					t.expires_at =
						QDateTime::currentSecsSinceEpoch()
						+ expires_in;

					blog(LOG_INFO,
					     "[Recast] Twitch auth "
					     "successful, fetching "
					     "user info");

					waitDlg->accept();
					fetchTwitchUserInfo(client_id, access);
				}

				/* Check for explicit error */
				if (tobj.contains("error")) {
					QString err =
						tobj.value("error").toString();
					if (err == "authorization_pending" ||
					    err == "slow_down") {
						/* Keep polling */
						return;
					}
					pollTimer->stop();
					waitDlg->reject();
					emit authError("twitch",
						"Twitch auth error: " + err);
				}
			});
		});

		connect(waitDlg, &QDialog::rejected, this, [pollTimer]() {
			pollTimer->stop();
		});

		pollTimer->start();
	});
}

void RecastAuthManager::fetchTwitchUserInfo(const QString &client_id,
					    const QString &access_token)
{
	QUrl url("https://api.twitch.tv/helix/users");
	QNetworkRequest req(url);
	req.setTransferTimeout(15000);
	req.setRawHeader("Authorization",
			 ("Bearer " + access_token).toUtf8());
	req.setRawHeader("Client-Id", client_id.toUtf8());

	QNetworkReply *reply = net_->get(req);
	connect(reply, &QNetworkReply::finished, this,
		[this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			emit authError("twitch",
				"Failed to fetch Twitch user info: " +
				reply->errorString());
			return;
		}

		QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
		QJsonArray data = doc.object().value("data").toArray();
		if (data.isEmpty()) {
			emit authError("twitch",
				"No user data returned from Twitch.");
			return;
		}

		QJsonObject user = data[0].toObject();
		RecastAuthToken &t = tokens_["twitch"];
		t.user_id = user.value("id").toString();
		t.user_name = user.value("display_name").toString();

		blog(LOG_INFO,
		     "[Recast] Twitch authenticated as %s (ID: %s)",
		     t.user_name.toUtf8().constData(),
		     t.user_id.toUtf8().constData());

		emit authStateChanged("twitch", true);
	});
}

void RecastAuthManager::validateTwitchToken(const QString &access_token)
{
	QUrl url("https://id.twitch.tv/oauth2/validate");
	QNetworkRequest req(url);
	req.setTransferTimeout(15000);
	req.setRawHeader("Authorization",
			 ("OAuth " + access_token).toUtf8());

	QNetworkReply *reply = net_->get(req);
	connect(reply, &QNetworkReply::finished, this,
		[this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			blog(LOG_INFO,
			     "[Recast] Twitch token validation failed, "
			     "attempting refresh");
			refreshTwitchToken();
			return;
		}

		QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
		QJsonObject obj = doc.object();
		int expires_in = obj.value("expires_in").toInt(0);

		if (expires_in > 0) {
			RecastAuthToken &t = tokens_["twitch"];
			t.expires_at = QDateTime::currentSecsSinceEpoch()
				       + expires_in;
		}
	});
}

void RecastAuthManager::refreshTwitchToken()
{
	if (!tokens_.contains("twitch"))
		return;

	const RecastAuthToken &t = tokens_["twitch"];
	if (t.refresh_token.isEmpty() || t.client_id.isEmpty())
		return;

	QUrl url("https://id.twitch.tv/oauth2/token");
	QUrlQuery params;
	params.addQueryItem("grant_type", "refresh_token");
	params.addQueryItem("refresh_token", t.refresh_token);
	params.addQueryItem("client_id", t.client_id);
	params.addQueryItem("client_secret", "");

	QNetworkRequest req(url);
	req.setTransferTimeout(15000);
	req.setHeader(QNetworkRequest::ContentTypeHeader,
		      "application/x-www-form-urlencoded");

	QNetworkReply *reply = net_->post(req, params.toString(
		QUrl::FullyEncoded).toUtf8());

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			blog(LOG_ERROR,
			     "[Recast] Twitch token refresh failed: %s",
			     reply->errorString().toUtf8().constData());
			emit authError("twitch",
				"Token refresh failed: " +
				reply->errorString());
			return;
		}

		QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
		QJsonObject obj = doc.object();

		if (!obj.contains("access_token")) {
			QString err = obj.value("message").toString(
				"Unknown error");
			blog(LOG_ERROR,
			     "[Recast] Twitch token refresh error: %s",
			     err.toUtf8().constData());
			emit authError("twitch",
				"Token refresh error: " + err);
			return;
		}

		RecastAuthToken &t = tokens_["twitch"];
		t.access_token = obj.value("access_token").toString();
		t.refresh_token = obj.value("refresh_token").toString();
		int expires_in = obj.value("expires_in").toInt(0);
		t.expires_at = QDateTime::currentSecsSinceEpoch()
			       + expires_in;

		blog(LOG_INFO, "[Recast] Twitch token refreshed "
		     "(expires in %ds)", expires_in);

		emit authStateChanged("twitch", true);
	});
}

/* ====================================================================
 * YouTube Authorization Code + PKCE with loopback
 * ==================================================================== */

static QString generateCodeVerifier()
{
	/* 43-128 unreserved characters */
	const int len = 64;
	const char charset[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
		"0123456789-._~";
	const int charset_len = (int)(sizeof(charset) - 1);

	QString verifier;
	verifier.reserve(len);
	QRandomGenerator *rng = QRandomGenerator::global();
	for (int i = 0; i < len; i++)
		verifier.append(QChar(charset[rng->bounded(charset_len)]));
	return verifier;
}

static QString generateCodeChallenge(const QString &verifier)
{
	QByteArray hash = QCryptographicHash::hash(
		verifier.toUtf8(), QCryptographicHash::Sha256);
	return QString::fromLatin1(
		hash.toBase64(QByteArray::Base64UrlEncoding |
			      QByteArray::OmitTrailingEquals));
}

void RecastAuthManager::startYouTubeAuth()
{
	QString client_id;
	QString client_secret;
	if (tokens_.contains("youtube")) {
		client_id = tokens_["youtube"].client_id;
		client_secret = tokens_["youtube"].client_secret;
	}

	if (client_id.isEmpty()) {
		emit authError("youtube",
			"No YouTube Client ID configured. "
			"Please enter a Client ID first.");
		return;
	}

	/* Start loopback server */
	if (loopback_server_) {
		loopback_server_->close();
		loopback_server_->deleteLater();
	}

	loopback_server_ = new QTcpServer(this);
	if (!loopback_server_->listen(QHostAddress::LocalHost, 0)) {
		emit authError("youtube",
			"Failed to start loopback server: " +
			loopback_server_->errorString());
		loopback_server_->deleteLater();
		loopback_server_ = nullptr;
		return;
	}

	quint16 port = loopback_server_->serverPort();
	QString redirect_uri = QString("http://127.0.0.1:%1").arg(port);

	blog(LOG_INFO, "[Recast] YouTube loopback server on port %d",
	     (int)port);

	/* Generate PKCE */
	QString code_verifier = generateCodeVerifier();
	QString code_challenge = generateCodeChallenge(code_verifier);

	/* Build authorization URL */
	QUrl authUrl("https://accounts.google.com/o/oauth2/v2/auth");
	QUrlQuery authParams;
	authParams.addQueryItem("client_id", client_id);
	authParams.addQueryItem("redirect_uri", redirect_uri);
	authParams.addQueryItem("response_type", "code");
	authParams.addQueryItem("scope",
		"https://www.googleapis.com/auth/youtube.readonly "
		"https://www.googleapis.com/auth/youtube.force-ssl");
	authParams.addQueryItem("code_challenge", code_challenge);
	authParams.addQueryItem("code_challenge_method", "S256");
	authParams.addQueryItem("access_type", "offline");
	authParams.addQueryItem("prompt", "consent");
	authUrl.setQuery(authParams);

	/* Open browser */
	QDesktopServices::openUrl(authUrl);

	/* Wait for the redirect */
	connect(loopback_server_, &QTcpServer::newConnection, this,
		[this, code_verifier, redirect_uri, client_secret]() {
		QTcpSocket *socket = loopback_server_->nextPendingConnection();
		if (!socket)
			return;

		connect(socket, &QTcpSocket::readyRead, this,
			[this, socket, code_verifier, redirect_uri,
			 client_secret]() {
			QByteArray data = socket->readAll();
			QString request = QString::fromUtf8(data);

			/* Parse the GET request line for the code */
			int get_start = request.indexOf("GET ");
			int http_end = request.indexOf(" HTTP/");
			if (get_start < 0 || http_end < 0) {
				socket->close();
				socket->deleteLater();
				return;
			}

			QString path = request.mid(get_start + 4,
						   http_end - get_start - 4);
			QUrl requestUrl("http://localhost" + path);
			QUrlQuery query(requestUrl);
			QString code = query.queryItemValue("code");
			QString error = query.queryItemValue("error");

			/* Send response to browser */
			QString html;
			if (!code.isEmpty()) {
				html = "<html><body><h2>Authorization "
				       "successful!</h2>"
				       "<p>You can close this window and "
				       "return to OBS.</p></body></html>";
			} else {
				html = "<html><body><h2>Authorization "
				       "failed</h2><p>" +
				       error.toHtmlEscaped() +
				       "</p></body></html>";
			}

			QByteArray response =
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: text/html\r\n"
				"Connection: close\r\n\r\n" +
				html.toUtf8();
			socket->write(response);
			socket->flush();
			socket->close();
			socket->deleteLater();

			/* Close the server */
			loopback_server_->close();
			loopback_server_->deleteLater();
			loopback_server_ = nullptr;

			if (!code.isEmpty()) {
				exchangeYouTubeCode(code, code_verifier,
						    redirect_uri);
			} else {
				emit authError("youtube",
					"YouTube auth denied: " + error);
			}
		});
	});
}

void RecastAuthManager::exchangeYouTubeCode(const QString &code,
					    const QString &code_verifier,
					    const QString &redirect_uri)
{
	if (!tokens_.contains("youtube"))
		return;

	const RecastAuthToken &t = tokens_["youtube"];

	QUrl url("https://oauth2.googleapis.com/token");
	QUrlQuery params;
	params.addQueryItem("code", code);
	params.addQueryItem("client_id", t.client_id);
	params.addQueryItem("client_secret", t.client_secret);
	params.addQueryItem("redirect_uri", redirect_uri);
	params.addQueryItem("grant_type", "authorization_code");
	params.addQueryItem("code_verifier", code_verifier);

	QNetworkRequest req(url);
	req.setTransferTimeout(15000);
	req.setHeader(QNetworkRequest::ContentTypeHeader,
		      "application/x-www-form-urlencoded");

	QNetworkReply *reply = net_->post(req, params.toString(
		QUrl::FullyEncoded).toUtf8());

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			emit authError("youtube",
				"YouTube token exchange failed: " +
				reply->errorString());
			return;
		}

		QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
		QJsonObject obj = doc.object();

		if (!obj.contains("access_token")) {
			QString err = obj.value("error_description").toString(
				obj.value("error").toString("Unknown error"));
			emit authError("youtube",
				"YouTube token exchange error: " + err);
			return;
		}

		RecastAuthToken &t = tokens_["youtube"];
		t.access_token = obj.value("access_token").toString();
		if (obj.contains("refresh_token"))
			t.refresh_token =
				obj.value("refresh_token").toString();
		int expires_in = obj.value("expires_in").toInt(0);
		t.expires_at = QDateTime::currentSecsSinceEpoch()
			       + expires_in;

		blog(LOG_INFO,
		     "[Recast] YouTube token obtained, "
		     "fetching channel info");

		fetchYouTubeChannelInfo(t.access_token);
	});
}

void RecastAuthManager::fetchYouTubeChannelInfo(const QString &access_token)
{
	QUrl url("https://www.googleapis.com/youtube/v3/channels"
		 "?part=snippet&mine=true");
	QNetworkRequest req(url);
	req.setTransferTimeout(15000);
	req.setRawHeader("Authorization",
			 ("Bearer " + access_token).toUtf8());

	QNetworkReply *reply = net_->get(req);
	connect(reply, &QNetworkReply::finished, this,
		[this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			emit authError("youtube",
				"Failed to fetch YouTube channel info: " +
				reply->errorString());
			return;
		}

		QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
		QJsonArray items = doc.object().value("items").toArray();
		if (items.isEmpty()) {
			emit authError("youtube",
				"No YouTube channel found for this account.");
			return;
		}

		QJsonObject channel = items[0].toObject();
		RecastAuthToken &t = tokens_["youtube"];
		t.user_id = channel.value("id").toString();

		QJsonObject snippet =
			channel.value("snippet").toObject();
		t.user_name = snippet.value("title").toString();

		blog(LOG_INFO,
		     "[Recast] YouTube authenticated as %s (ID: %s)",
		     t.user_name.toUtf8().constData(),
		     t.user_id.toUtf8().constData());

		emit authStateChanged("youtube", true);
	});
}

void RecastAuthManager::refreshYouTubeToken()
{
	if (!tokens_.contains("youtube"))
		return;

	const RecastAuthToken &t = tokens_["youtube"];
	if (t.refresh_token.isEmpty() || t.client_id.isEmpty())
		return;

	QUrl url("https://oauth2.googleapis.com/token");
	QUrlQuery params;
	params.addQueryItem("grant_type", "refresh_token");
	params.addQueryItem("refresh_token", t.refresh_token);
	params.addQueryItem("client_id", t.client_id);
	params.addQueryItem("client_secret", t.client_secret);

	QNetworkRequest req(url);
	req.setTransferTimeout(15000);
	req.setHeader(QNetworkRequest::ContentTypeHeader,
		      "application/x-www-form-urlencoded");

	QNetworkReply *reply = net_->post(req, params.toString(
		QUrl::FullyEncoded).toUtf8());

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			blog(LOG_ERROR,
			     "[Recast] YouTube token refresh failed: %s",
			     reply->errorString().toUtf8().constData());
			emit authError("youtube",
				"Token refresh failed: " +
				reply->errorString());
			return;
		}

		QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
		QJsonObject obj = doc.object();

		if (!obj.contains("access_token")) {
			QString err = obj.value("error_description").toString(
				obj.value("error").toString("Unknown error"));
			blog(LOG_ERROR,
			     "[Recast] YouTube token refresh error: %s",
			     err.toUtf8().constData());
			emit authError("youtube",
				"Token refresh error: " + err);
			return;
		}

		RecastAuthToken &t = tokens_["youtube"];
		t.access_token = obj.value("access_token").toString();
		if (obj.contains("refresh_token"))
			t.refresh_token =
				obj.value("refresh_token").toString();
		int expires_in = obj.value("expires_in").toInt(0);
		t.expires_at = QDateTime::currentSecsSinceEpoch()
			       + expires_in;

		blog(LOG_INFO, "[Recast] YouTube token refreshed "
		     "(expires in %ds)", expires_in);

		emit authStateChanged("youtube", true);
	});
}

/* ====================================================================
 * RecastAuthDialog
 * ==================================================================== */

RecastAuthDialog::RecastAuthDialog(QWidget *parent)
	: QDialog(parent)
{
	setWindowTitle("Platform Accounts");
	setMinimumWidth(500);

	auto *root = new QVBoxLayout(this);

	RecastAuthManager *auth = RecastAuthManager::instance();

	/* ---- Twitch group ---- */
	auto *twitch_group = new QGroupBox("Twitch");
	auto *twitch_layout = new QVBoxLayout(twitch_group);

	/* Status row */
	auto *twitch_status_row = new QHBoxLayout;

	twitch_status_ = new QLabel;
	twitch_status_->setMinimumWidth(250);
	twitch_status_row->addWidget(twitch_status_, 1);

	twitch_btn_ = new QPushButton;
	twitch_btn_->setFixedWidth(100);
	twitch_status_row->addWidget(twitch_btn_);

	twitch_layout->addLayout(twitch_status_row);

	/* Client ID field */
	auto *twitch_id_row = new QFormLayout;
	twitch_client_id_ = new QLineEdit;
	twitch_client_id_->setPlaceholderText(
		"Enter your Twitch application Client ID");
	if (auth->isAuthenticated("twitch"))
		twitch_client_id_->setText(auth->clientId("twitch"));
	else if (auth->clientId("twitch").isEmpty() == false)
		twitch_client_id_->setText(auth->clientId("twitch"));
	twitch_id_row->addRow("Client ID:", twitch_client_id_);
	twitch_layout->addLayout(twitch_id_row);

	root->addWidget(twitch_group);

	/* ---- YouTube group ---- */
	auto *youtube_group = new QGroupBox("YouTube");
	auto *youtube_layout = new QVBoxLayout(youtube_group);

	/* Status row */
	auto *youtube_status_row = new QHBoxLayout;

	youtube_status_ = new QLabel;
	youtube_status_->setMinimumWidth(250);
	youtube_status_row->addWidget(youtube_status_, 1);

	youtube_btn_ = new QPushButton;
	youtube_btn_->setFixedWidth(100);
	youtube_status_row->addWidget(youtube_btn_);

	youtube_layout->addLayout(youtube_status_row);

	/* Client ID + Secret fields */
	auto *youtube_id_row = new QFormLayout;
	youtube_client_id_ = new QLineEdit;
	youtube_client_id_->setPlaceholderText(
		"Enter your Google OAuth Client ID");
	if (!auth->clientId("youtube").isEmpty())
		youtube_client_id_->setText(auth->clientId("youtube"));
	youtube_id_row->addRow("Client ID:", youtube_client_id_);

	youtube_client_secret_ = new QLineEdit;
	youtube_client_secret_->setPlaceholderText(
		"Enter your Google OAuth Client Secret");
	youtube_client_secret_->setEchoMode(QLineEdit::Password);
	/* Load existing client secret from token store */
	if (auth->isAuthenticated("youtube")) {
		/* Access secret via saveToConfig round-trip */
		obs_data_t *ad = auth->saveToConfig();
		if (ad) {
			obs_data_t *yt = obs_data_get_obj(ad, "youtube");
			if (yt) {
				const char *sec = obs_data_get_string(
					yt, "client_secret");
				if (sec && *sec)
					youtube_client_secret_->setText(
						QString::fromUtf8(sec));
				obs_data_release(yt);
			}
			obs_data_release(ad);
		}
	}
	youtube_id_row->addRow("Client Secret:", youtube_client_secret_);
	youtube_layout->addLayout(youtube_id_row);

	root->addWidget(youtube_group);

	/* ---- Close button ---- */
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
	connect(buttons, &QDialogButtonBox::rejected,
		this, &QDialog::accept);
	root->addWidget(buttons);

	/* ---- Initialize UI states ---- */
	updatePlatformUI("twitch");
	updatePlatformUI("youtube");

	/* ---- Wire signals ---- */
	connect(auth, &RecastAuthManager::authStateChanged,
		this, &RecastAuthDialog::onAuthStateChanged);
	connect(auth, &RecastAuthManager::authError,
		this, &RecastAuthDialog::onAuthError);

	/* Twitch connect/disconnect */
	connect(twitch_btn_, &QPushButton::clicked, this, [this, auth]() {
		if (auth->isAuthenticated("twitch")) {
			auth->logout("twitch");
		} else {
			/* Save client ID before starting auth */
			QString cid = twitch_client_id_->text().trimmed();
			if (cid.isEmpty()) {
				QMessageBox::warning(this,
					"Twitch",
					"Please enter a Twitch Client ID "
					"before connecting.");
				return;
			}
			/* Store the client ID in the token entry */
			obs_data_t *ad = obs_data_create();
			obs_data_t *td = obs_data_create();
			obs_data_set_string(td, "client_id",
				cid.toUtf8().constData());
			/* Preserve existing tokens if any */
			obs_data_t *existing = auth->saveToConfig();
			obs_data_t *et = existing
				? obs_data_get_obj(existing, "twitch")
				: nullptr;
			if (et) {
				obs_data_set_string(td, "access_token",
					obs_data_get_string(et,
						"access_token"));
				obs_data_set_string(td, "refresh_token",
					obs_data_get_string(et,
						"refresh_token"));
				obs_data_set_string(td, "user_id",
					obs_data_get_string(et, "user_id"));
				obs_data_set_string(td, "user_name",
					obs_data_get_string(et, "user_name"));
				obs_data_set_int(td, "expires_at",
					obs_data_get_int(et, "expires_at"));
				obs_data_release(et);
			}
			if (existing) {
				/* Copy youtube entry if present */
				obs_data_t *yt = obs_data_get_obj(
					existing, "youtube");
				if (yt) {
					obs_data_set_obj(ad, "youtube", yt);
					obs_data_release(yt);
				}
				obs_data_release(existing);
			}
			obs_data_set_obj(ad, "twitch", td);
			obs_data_release(td);
			auth->loadFromConfig(ad);
			obs_data_release(ad);

			auth->startAuth("twitch");
		}
	});

	/* YouTube connect/disconnect */
	connect(youtube_btn_, &QPushButton::clicked, this, [this, auth]() {
		if (auth->isAuthenticated("youtube")) {
			auth->logout("youtube");
		} else {
			QString cid = youtube_client_id_->text().trimmed();
			QString csec =
				youtube_client_secret_->text().trimmed();
			if (cid.isEmpty()) {
				QMessageBox::warning(this,
					"YouTube",
					"Please enter a YouTube Client ID "
					"before connecting.");
				return;
			}
			/* Store client ID and secret */
			obs_data_t *ad = obs_data_create();
			obs_data_t *yd = obs_data_create();
			obs_data_set_string(yd, "client_id",
				cid.toUtf8().constData());
			obs_data_set_string(yd, "client_secret",
				csec.toUtf8().constData());
			/* Preserve existing tokens if any */
			obs_data_t *existing = auth->saveToConfig();
			if (existing) {
				obs_data_t *tw = obs_data_get_obj(
					existing, "twitch");
				if (tw) {
					obs_data_set_obj(ad, "twitch", tw);
					obs_data_release(tw);
				}
				obs_data_release(existing);
			}
			obs_data_set_obj(ad, "youtube", yd);
			obs_data_release(yd);
			auth->loadFromConfig(ad);
			obs_data_release(ad);

			auth->startAuth("youtube");
		}
	});
}

void RecastAuthDialog::onAuthStateChanged(const QString &platform,
					  bool authenticated)
{
	Q_UNUSED(authenticated);
	updatePlatformUI(platform);
}

void RecastAuthDialog::onAuthError(const QString &platform,
				   const QString &error)
{
	QMessageBox::warning(this,
		platform.left(1).toUpper() + platform.mid(1) + " Error",
		error);
}

void RecastAuthDialog::updatePlatformUI(const QString &platform)
{
	RecastAuthManager *auth = RecastAuthManager::instance();
	bool connected = auth->isAuthenticated(platform);

	if (platform == "twitch") {
		if (connected) {
			twitch_status_->setText(
				QString("<span style='color:#4CAF50;'>"
					"\xe2\x9c\x93</span> Connected as "
					"<b>%1</b>")
					.arg(auth->userName("twitch")
						.toHtmlEscaped()));
			twitch_btn_->setText("Disconnect");
			twitch_btn_->setStyleSheet(
				"QPushButton { background: #c62828; "
				"color: white; border-radius: 3px; "
				"padding: 4px 8px; }"
				"QPushButton:hover { background: #e53935; }");
		} else {
			twitch_status_->setText(
				"<span style='color:#999;'>"
				"\xe2\x9c\x97</span> Not Connected");
			twitch_btn_->setText("Connect");
			twitch_btn_->setStyleSheet(
				"QPushButton { background: #9146FF; "
				"color: white; border-radius: 3px; "
				"padding: 4px 8px; }"
				"QPushButton:hover { background: #7B2FFF; }");
		}
	} else if (platform == "youtube") {
		if (connected) {
			youtube_status_->setText(
				QString("<span style='color:#4CAF50;'>"
					"\xe2\x9c\x93</span> Connected as "
					"<b>%1</b>")
					.arg(auth->userName("youtube")
						.toHtmlEscaped()));
			youtube_btn_->setText("Disconnect");
			youtube_btn_->setStyleSheet(
				"QPushButton { background: #c62828; "
				"color: white; border-radius: 3px; "
				"padding: 4px 8px; }"
				"QPushButton:hover { background: #e53935; }");
		} else {
			youtube_status_->setText(
				"<span style='color:#999;'>"
				"\xe2\x9c\x97</span> Not Connected");
			youtube_btn_->setText("Connect");
			youtube_btn_->setStyleSheet(
				"QPushButton { background: #FF0000; "
				"color: white; border-radius: 3px; "
				"padding: 4px 8px; }"
				"QPushButton:hover { background: #CC0000; }");
		}
	}
}
