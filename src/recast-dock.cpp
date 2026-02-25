/*
 * recast-dock.cpp — Qt dock widget UI for Recast multi-output.
 *
 * Provides the "Recast Output" panel in OBS with:
 *   - Output cards (start/stop/delete per target)
 *   - "Add Output" dialog
 *   - "Sync with Recast Server" button
 *   - Periodic status refresh
 */

#include "recast-dock.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QScrollArea>

extern "C" {
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>
}

/* ====================================================================
 * RecastAddDialog
 * ==================================================================== */

RecastAddDialog::RecastAddDialog(QWidget *parent)
	: QDialog(parent)
{
	setWindowTitle(obs_module_text("Recast.AddOutput"));
	setMinimumWidth(400);

	auto *form = new QFormLayout;

	name_edit = new QLineEdit;
	name_edit->setPlaceholderText("e.g. Twitch, YouTube Shorts");
	form->addRow(obs_module_text("Recast.Name"), name_edit);

	url_edit = new QLineEdit;
	url_edit->setPlaceholderText("rtmp://...");
	form->addRow(obs_module_text("Recast.URL"), url_edit);

	key_edit = new QLineEdit;
	key_edit->setEchoMode(QLineEdit::Password);
	key_edit->setPlaceholderText("Stream key");
	form->addRow(obs_module_text("Recast.Key"), key_edit);

	scene_combo = new QComboBox;
	scene_combo->addItem(obs_module_text("Recast.MainScene"), "");
	populateScenes();
	form->addRow(obs_module_text("Recast.Scene"), scene_combo);

	/* Resolution override (0 = use main canvas) */
	auto *res_layout = new QHBoxLayout;
	width_spin = new QSpinBox;
	width_spin->setRange(0, 7680);
	width_spin->setSpecialValueText(obs_module_text("Recast.Auto"));
	height_spin = new QSpinBox;
	height_spin->setRange(0, 7680);
	height_spin->setSpecialValueText(obs_module_text("Recast.Auto"));
	res_layout->addWidget(width_spin);
	res_layout->addWidget(new QLabel("x"));
	res_layout->addWidget(height_spin);
	form->addRow(obs_module_text("Recast.Resolution"), res_layout);

	auto *buttons =
		new QDialogButtonBox(QDialogButtonBox::Ok |
				     QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *root = new QVBoxLayout(this);
	root->addLayout(form);
	root->addWidget(buttons);
}

void RecastAddDialog::populateScenes()
{
	char **scene_names = obs_frontend_get_scene_names();
	if (!scene_names)
		return;
	for (char **s = scene_names; *s; s++) {
		scene_combo->addItem(QString::fromUtf8(*s),
				     QString::fromUtf8(*s));
	}
	bfree(scene_names);
}

QString RecastAddDialog::getName() const { return name_edit->text(); }
QString RecastAddDialog::getUrl() const { return url_edit->text(); }
QString RecastAddDialog::getKey() const { return key_edit->text(); }

QString RecastAddDialog::getScene() const
{
	return scene_combo->currentData().toString();
}

int RecastAddDialog::getWidth() const { return width_spin->value(); }
int RecastAddDialog::getHeight() const { return height_spin->value(); }

/* ====================================================================
 * RecastOutputCard
 * ==================================================================== */

RecastOutputCard::RecastOutputCard(recast_output_target_t *target,
				   QWidget *parent)
	: QGroupBox(parent), target_(target)
{
	setTitle(QString::fromUtf8(target->name));

	auto *vbox = new QVBoxLayout(this);

	/* URL row (masked) */
	QString masked_url = QString::fromUtf8(target->url);
	if (masked_url.length() > 30)
		masked_url = masked_url.left(27) + "...";
	vbox->addWidget(new QLabel(masked_url));

	/* Scene row */
	QString scene_text =
		(target->scene_name && *target->scene_name)
			? QString("Scene: %1").arg(
				  QString::fromUtf8(target->scene_name))
			: QString("Scene: (main)");
	vbox->addWidget(new QLabel(scene_text));

	/* Resolution row (if custom) */
	if (target->width > 0 && target->height > 0) {
		vbox->addWidget(new QLabel(
			QString("Resolution: %1x%2")
				.arg(target->width)
				.arg(target->height)));
	}

	/* Status + buttons row */
	auto *bottom = new QHBoxLayout;

	status_label = new QLabel(obs_module_text("Recast.Status.Stopped"));
	bottom->addWidget(status_label, 1);

	toggle_btn = new QPushButton(obs_module_text("Recast.Start"));
	toggle_btn->setFixedWidth(60);
	connect(toggle_btn, &QPushButton::clicked,
		this, &RecastOutputCard::onToggleStream);
	bottom->addWidget(toggle_btn);

	delete_btn = new QPushButton(obs_module_text("Recast.Delete"));
	delete_btn->setFixedWidth(30);
	delete_btn->setToolTip(obs_module_text("Recast.DeleteTip"));
	connect(delete_btn, &QPushButton::clicked, this, [this]() {
		emit deleteRequested(this);
	});
	bottom->addWidget(delete_btn);

	vbox->addLayout(bottom);
}

RecastOutputCard::~RecastOutputCard()
{
	/* The target is owned by the dock / card lifecycle */
}

void RecastOutputCard::refreshStatus()
{
	if (!target_)
		return;

	if (target_->active) {
		uint64_t sec = recast_output_target_elapsed_sec(target_);
		uint64_t h = sec / 3600;
		uint64_t m = (sec % 3600) / 60;
		uint64_t s = sec % 60;

		QString time_str;
		if (h > 0)
			time_str = QString("%1h %2m").arg(h).arg(m);
		else
			time_str = QString("%1m %2s").arg(m).arg(s);

		status_label->setText(
			QString("%1 (%2)")
				.arg(obs_module_text("Recast.Status.Streaming"))
				.arg(time_str));
		status_label->setStyleSheet("color: #4CAF50; font-weight: bold;");
		toggle_btn->setText(obs_module_text("Recast.Stop"));
	} else {
		status_label->setText(
			obs_module_text("Recast.Status.Stopped"));
		status_label->setStyleSheet("color: #999;");
		toggle_btn->setText(obs_module_text("Recast.Start"));
	}
}

void RecastOutputCard::onToggleStream()
{
	if (!target_)
		return;

	if (target_->active) {
		recast_output_target_stop(target_);
	} else {
		if (!recast_output_target_start(target_)) {
			QMessageBox::warning(
				this, obs_module_text("Recast.Error"),
				obs_module_text("Recast.Error.StartFailed"));
		}
	}
	refreshStatus();
}

/* ====================================================================
 * RecastDock
 * ==================================================================== */

RecastDock::RecastDock(QWidget *parent)
	: QDockWidget(obs_module_text("Recast.DockTitle"), parent)
{
	setObjectName("RecastOutputDock");
	setFeatures(QDockWidget::DockWidgetMovable |
		    QDockWidget::DockWidgetFloatable);

	/* Scroll area for output cards */
	auto *scroll = new QScrollArea;
	scroll->setWidgetResizable(true);

	auto *container = new QWidget;
	auto *root_layout = new QVBoxLayout(container);
	root_layout->setAlignment(Qt::AlignTop);

	/* Add Output button */
	auto *add_btn =
		new QPushButton(obs_module_text("Recast.AddOutput"));
	connect(add_btn, &QPushButton::clicked,
		this, &RecastDock::onAddOutput);
	root_layout->addWidget(add_btn);

	/* Cards container */
	cards_layout = new QVBoxLayout;
	cards_layout->setAlignment(Qt::AlignTop);
	root_layout->addLayout(cards_layout);

	/* Stretch to push sync button to bottom */
	root_layout->addStretch();

	/* Sync with Recast Server button */
	auto *sync_btn =
		new QPushButton(obs_module_text("Recast.SyncServer"));
	connect(sync_btn, &QPushButton::clicked,
		this, &RecastDock::onSyncServer);
	root_layout->addWidget(sync_btn);

	scroll->setWidget(container);
	setWidget(scroll);

	/* Network manager for API calls */
	net_mgr = new QNetworkAccessManager(this);

	/* Refresh timer (every second for elapsed time display) */
	refresh_timer = new QTimer(this);
	connect(refresh_timer, &QTimer::timeout,
		this, &RecastDock::onRefreshTimer);
	refresh_timer->start(1000);

	/* Load saved config */
	loadFromConfig();
}

RecastDock::~RecastDock()
{
	refresh_timer->stop();

	/* Stop all active outputs and destroy targets */
	for (auto *card : cards) {
		recast_output_target_t *t = card->target();
		if (t) {
			recast_output_target_destroy(t);
		}
	}
	cards.clear();
}

void RecastDock::onAddOutput()
{
	RecastAddDialog dlg(this);
	if (dlg.exec() != QDialog::Accepted)
		return;

	QString name = dlg.getName().trimmed();
	QString url = dlg.getUrl().trimmed();
	QString key = dlg.getKey().trimmed();

	if (name.isEmpty() || url.isEmpty()) {
		QMessageBox::warning(
			this, obs_module_text("Recast.Error"),
			obs_module_text("Recast.Error.NameUrlRequired"));
		return;
	}

	recast_output_target_t *target = recast_output_target_create(
		name.toUtf8().constData(),
		url.toUtf8().constData(),
		key.toUtf8().constData(),
		dlg.getScene().toUtf8().constData(),
		dlg.getWidth(), dlg.getHeight());

	addCard(target);
	saveToConfig();
}

void RecastDock::onDeleteOutput(RecastOutputCard *card)
{
	if (!card)
		return;

	auto answer = QMessageBox::question(
		this, obs_module_text("Recast.Confirm"),
		QString("%1 '%2'?")
			.arg(obs_module_text("Recast.ConfirmDelete"))
			.arg(card->target()->name));

	if (answer != QMessageBox::Yes)
		return;

	removeCard(card);
	saveToConfig();
}

void RecastDock::onSyncServer()
{
	char *token = recast_config_get_server_token();
	if (!token || !*token) {
		bfree(token);

		/* Prompt for token */
		QLineEdit *input = new QLineEdit;
		input->setPlaceholderText("Recast API token");

		QDialog dlg(this);
		dlg.setWindowTitle(obs_module_text("Recast.SyncServer"));
		auto *layout = new QVBoxLayout(&dlg);
		layout->addWidget(
			new QLabel(obs_module_text("Recast.EnterToken")));
		layout->addWidget(input);
		auto *btns = new QDialogButtonBox(
			QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
		connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
		layout->addWidget(btns);

		if (dlg.exec() != QDialog::Accepted ||
		    input->text().trimmed().isEmpty())
			return;

		QString new_token = input->text().trimmed();
		recast_config_set_server_token(
			new_token.toUtf8().constData());
		token = bstrdup(new_token.toUtf8().constData());
	}

	/* GET /api/stream/status?token=XXX */
	QString url = QString("https://api.recast.stream/api/stream/status?token=%1")
			      .arg(QString::fromUtf8(token));
	bfree(token);

	QNetworkReply *reply = net_mgr->get(QNetworkRequest(QUrl(url)));
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			QMessageBox::warning(
				this, obs_module_text("Recast.Error"),
				QString("%1: %2")
					.arg(obs_module_text("Recast.Error.Sync"))
					.arg(reply->errorString()));
			return;
		}

		QByteArray data = reply->readAll();
		QJsonDocument doc = QJsonDocument::fromJson(data);
		if (!doc.isObject())
			return;

		QJsonArray platforms =
			doc.object().value("platforms").toArray();

		for (const QJsonValue &val : platforms) {
			QJsonObject p = val.toObject();
			QString name = p.value("name").toString();
			QString rtmp_url = p.value("rtmpUrl").toString();
			QString key = p.value("streamKey").toString();

			if (name.isEmpty() || rtmp_url.isEmpty())
				continue;

			/* Check if we already have this target */
			bool exists = false;
			for (auto *card : cards) {
				if (QString::fromUtf8(card->target()->url) ==
				    rtmp_url) {
					exists = true;
					break;
				}
			}
			if (exists)
				continue;

			recast_output_target_t *target =
				recast_output_target_create(
					name.toUtf8().constData(),
					rtmp_url.toUtf8().constData(),
					key.toUtf8().constData(),
					"", 0, 0);
			addCard(target);
		}

		saveToConfig();

		QMessageBox::information(
			this, obs_module_text("Recast.SyncServer"),
			obs_module_text("Recast.SyncComplete"));
	});
}

void RecastDock::onRefreshTimer()
{
	for (auto *card : cards)
		card->refreshStatus();
}

void RecastDock::loadFromConfig()
{
	obs_data_array_t *arr = recast_config_load();
	if (!arr)
		return;

	size_t count = obs_data_array_count(arr);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);

		const char *name = obs_data_get_string(item, "name");
		const char *url = obs_data_get_string(item, "url");
		const char *key = obs_data_get_string(item, "key");
		const char *scene = obs_data_get_string(item, "scene");
		int w = (int)obs_data_get_int(item, "width");
		int h = (int)obs_data_get_int(item, "height");

		recast_output_target_t *target =
			recast_output_target_create(name, url, key, scene,
						    w, h);
		target->enabled = obs_data_get_bool(item, "enabled");
		target->auto_start = obs_data_get_bool(item, "autoStart");

		addCard(target);

		obs_data_release(item);
	}

	obs_data_array_release(arr);
}

void RecastDock::saveToConfig()
{
	obs_data_array_t *arr = obs_data_array_create();

	for (auto *card : cards) {
		recast_output_target_t *t = card->target();
		obs_data_t *item = obs_data_create();

		obs_data_set_string(item, "name", t->name);
		obs_data_set_string(item, "url", t->url);
		obs_data_set_string(item, "key", t->key);
		obs_data_set_string(item, "scene",
				    t->scene_name ? t->scene_name : "");
		obs_data_set_bool(item, "enabled", t->enabled);
		obs_data_set_bool(item, "autoStart", t->auto_start);
		obs_data_set_int(item, "width", t->width);
		obs_data_set_int(item, "height", t->height);

		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}

	recast_config_save(arr);
	obs_data_array_release(arr);
}

void RecastDock::addCard(recast_output_target_t *target)
{
	auto *card = new RecastOutputCard(target, this);
	connect(card, &RecastOutputCard::deleteRequested,
		this, &RecastDock::onDeleteOutput);
	cards_layout->addWidget(card);
	cards.push_back(card);
	card->refreshStatus();
}

void RecastDock::removeCard(RecastOutputCard *card)
{
	auto it = std::find(cards.begin(), cards.end(), card);
	if (it == cards.end())
		return;

	recast_output_target_t *t = card->target();
	cards.erase(it);
	cards_layout->removeWidget(card);
	card->deleteLater();

	recast_output_target_destroy(t);
}

/* ====================================================================
 * C interface — called from plugin-main.c
 * ==================================================================== */

static RecastDock *dock_instance = nullptr;

void recast_dock_create(void)
{
	if (dock_instance)
		return;

	dock_instance = new RecastDock(
		static_cast<QWidget *>(obs_frontend_get_main_window()));

	obs_frontend_add_dock_by_id("RecastOutputDock",
				    obs_module_text("Recast.DockTitle"),
				    dock_instance);
}

void recast_dock_destroy(void)
{
	/* OBS manages the widget lifecycle, but we clear our reference */
	dock_instance = nullptr;
}
