/*
 * recast-multistream.cpp -- Single multistream dock + add dialog.
 *
 * Simple RTMP destination list where you add a server URL, pick
 * which canvas (Main or Vertical), and hit Start.
 */

#include "recast-multistream.h"
#include "recast-platform-icons.h"
#include "recast-vertical.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QScrollArea>
#include <QStyle>

extern "C" {
#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include "recast-protocol.h"
#include "recast-config.h"
}

/* ====================================================================
 * recast_destination_t -- Simplified destination management
 * ==================================================================== */

static obs_encoder_t *get_main_video_encoder(void)
{
	obs_output_t *main_out = obs_frontend_get_streaming_output();
	if (!main_out)
		return NULL;
	obs_encoder_t *enc = obs_output_get_video_encoder(main_out);
	obs_output_release(main_out);
	return enc;
}

static obs_encoder_t *get_main_audio_encoder(void)
{
	obs_output_t *main_out = obs_frontend_get_streaming_output();
	if (!main_out)
		return NULL;
	obs_encoder_t *enc = obs_output_get_audio_encoder(main_out, 0);
	obs_output_release(main_out);
	return enc;
}

static char *generate_dest_id(const char *name)
{
	struct dstr id = {0};
	dstr_printf(&id, "recast_dest_%s_%llu", name,
		    (unsigned long long)os_gettime_ns());
	return id.array;
}

recast_destination_t *recast_destination_create(const char *name,
						const char *url,
						const char *key,
						bool canvas_vertical)
{
	recast_destination_t *d = (recast_destination_t *)bzalloc(
		sizeof(recast_destination_t));

	d->id = generate_dest_id(name);
	d->name = bstrdup(name);
	d->url = bstrdup(url);
	d->key = bstrdup(key);
	d->auto_start = false;
	d->auto_stop = false;
	d->canvas_vertical = canvas_vertical;

	d->protocol = recast_protocol_detect(url);

	/* Create service */
	const char *service_id = recast_protocol_service_id(d->protocol);
	obs_data_t *svc_settings = obs_data_create();
	if (d->protocol == RECAST_PROTO_WHIP) {
		obs_data_set_string(svc_settings, "server", url);
		obs_data_set_string(svc_settings, "bearer_token", key);
	} else {
		obs_data_set_string(svc_settings, "server", url);
		obs_data_set_string(svc_settings, "key", key);
	}

	struct dstr svc_name = {0};
	dstr_printf(&svc_name, "recast_svc_%s", name);
	d->service = obs_service_create(
		service_id, svc_name.array, svc_settings, NULL);
	dstr_free(&svc_name);
	obs_data_release(svc_settings);

	/* Create output */
	const char *output_id = recast_protocol_output_id(d->protocol);
	struct dstr out_name = {0};
	dstr_printf(&out_name, "recast_out_%s", name);
	d->output = obs_output_create(
		output_id, out_name.array, NULL, NULL);
	dstr_free(&out_name);

	blog(LOG_INFO,
	     "[Recast] Created destination '%s' (canvas=%s, protocol=%s)",
	     name, canvas_vertical ? "vertical" : "main",
	     recast_protocol_name(d->protocol));

	return d;
}

void recast_destination_destroy(recast_destination_t *d)
{
	if (!d)
		return;

	if (d->active)
		recast_destination_stop(d);

	if (d->output)
		obs_output_release(d->output);
	if (d->service)
		obs_service_release(d->service);

	bfree(d->id);
	bfree(d->name);
	bfree(d->url);
	bfree(d->key);
	bfree(d);
}

bool recast_destination_start(recast_destination_t *d)
{
	if (!d || !d->output || !d->service)
		return false;
	if (d->active)
		return true;

	obs_output_set_service(d->output, d->service);

	obs_encoder_t *venc = NULL;
	obs_encoder_t *aenc = get_main_audio_encoder();

	if (!aenc) {
		blog(LOG_ERROR, "[Recast] No main audio encoder");
		return false;
	}

	if (d->canvas_vertical) {
		/* Use shared vertical encoder */
		venc = recast_vertical_acquire_encoder();
		if (!venc) {
			blog(LOG_ERROR,
			     "[Recast] Failed to acquire vertical encoder "
			     "for '%s'", d->name);
			return false;
		}
	} else {
		/* Share the main encoder (zero overhead) */
		venc = get_main_video_encoder();
		if (!venc) {
			blog(LOG_ERROR,
			     "[Recast] No main video encoder "
			     "(is main stream running?) for '%s'",
			     d->name);
			return false;
		}
	}

	obs_output_set_video_encoder(d->output, venc);
	obs_output_set_audio_encoder(d->output, aenc, 0);

	bool ok = obs_output_start(d->output);
	if (ok) {
		d->active = true;
		d->start_time_ns = os_gettime_ns();
		blog(LOG_INFO, "[Recast] Started destination '%s' -> %s",
		     d->name, d->url);
	} else {
		blog(LOG_ERROR, "[Recast] Failed to start destination '%s'",
		     d->name);
		if (d->canvas_vertical)
			recast_vertical_release_encoder();
	}

	return ok;
}

void recast_destination_stop(recast_destination_t *d)
{
	if (!d || !d->active)
		return;

	obs_output_stop(d->output);
	d->active = false;
	d->start_time_ns = 0;

	if (d->canvas_vertical)
		recast_vertical_release_encoder();

	blog(LOG_INFO, "[Recast] Stopped destination '%s'", d->name);
}

uint64_t recast_destination_elapsed_sec(const recast_destination_t *d)
{
	if (!d || !d->active || d->start_time_ns == 0)
		return 0;
	return (os_gettime_ns() - d->start_time_ns) / 1000000000ULL;
}

/* ====================================================================
 * RecastAddDestinationDialog
 * ==================================================================== */

RecastDestinationDialog::RecastDestinationDialog(
	QWidget *parent, const QString &name, const QString &url,
	const QString &key, bool canvas_vertical)
	: QDialog(parent)
{
	setWindowTitle(obs_module_text("Recast.Multistream.AddDest"));
	setMinimumWidth(400);

	auto *form = new QFormLayout;

	name_edit_ = new QLineEdit;
	name_edit_->setPlaceholderText("e.g. Twitch, YouTube Shorts");
	name_edit_->setText(name);
	form->addRow(obs_module_text("Recast.Name"), name_edit_);

	url_edit_ = new QLineEdit;
	url_edit_->setPlaceholderText("rtmp://...");
	url_edit_->setText(url);
	form->addRow(obs_module_text("Recast.URL"), url_edit_);

	key_edit_ = new QLineEdit;
	key_edit_->setEchoMode(QLineEdit::Password);
	key_edit_->setPlaceholderText("Stream key");
	key_edit_->setText(key);
	form->addRow(obs_module_text("Recast.Key"), key_edit_);

	canvas_combo_ = new QComboBox;
	canvas_combo_->addItem(
		obs_module_text("Recast.Multistream.CanvasMain"), false);
	canvas_combo_->addItem(
		obs_module_text("Recast.Multistream.CanvasVertical"), true);
	if (canvas_vertical)
		canvas_combo_->setCurrentIndex(1);
	form->addRow(obs_module_text("Recast.Multistream.Canvas"),
		     canvas_combo_);

	auto *buttons =
		new QDialogButtonBox(QDialogButtonBox::Ok |
				     QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *root = new QVBoxLayout(this);
	root->addLayout(form);
	root->addWidget(buttons);
}

QString RecastDestinationDialog::getName() const
{
	return name_edit_->text();
}

QString RecastDestinationDialog::getUrl() const
{
	return url_edit_->text();
}

QString RecastDestinationDialog::getKey() const
{
	return key_edit_->text();
}

bool RecastDestinationDialog::getCanvasVertical() const
{
	return canvas_combo_->currentData().toBool();
}

/* ====================================================================
 * RecastDestinationRow
 * ==================================================================== */

RecastDestinationRow::RecastDestinationRow(recast_destination_t *dest,
					   QWidget *parent)
	: QFrame(parent), dest_(dest), last_bytes_(0)
{
	setFrameShape(QFrame::StyledPanel);
	setFrameShadow(QFrame::Raised);

	/* Main card layout: two rows stacked vertically */
	auto *card = new QVBoxLayout(this);
	card->setContentsMargins(8, 6, 8, 6);
	card->setSpacing(4);

	/* ---- Top row: icon, name, canvas badge, auto, edit, delete ---- */
	auto *top = new QHBoxLayout;
	top->setSpacing(8);

	/* Platform icon */
	platform_icon_label_ = new QLabel;
	platform_icon_label_->setFixedSize(20, 20);
	QString platform_id = recast_detect_platform(
		QString::fromUtf8(dest->url));
	if (!platform_id.isEmpty()) {
		QIcon icon = recast_platform_icon(platform_id);
		platform_icon_label_->setPixmap(icon.pixmap(20, 20));
	}
	top->addWidget(platform_icon_label_);

	/* Name (bold, stretch) */
	name_label_ = new QLabel(QString::fromUtf8(dest->name));
	name_label_->setStyleSheet("font-weight: bold; font-size: 13px;");
	top->addWidget(name_label_, 1);

	/* Canvas indicator badge */
	canvas_label_ = new QLabel(
		dest->canvas_vertical
			? obs_module_text("Recast.Multistream.CanvasVertical")
			: obs_module_text("Recast.Multistream.CanvasMain"));
	canvas_label_->setStyleSheet(
		"background: #444; color: #fff; padding: 2px 6px; "
		"border-radius: 3px; font-size: 11px;");
	top->addWidget(canvas_label_);

	/* Auto checkbox */
	auto_check_ = new QCheckBox(obs_module_text("Recast.Auto"));
	auto_check_->setChecked(dest->auto_start);
	auto_check_->setToolTip(
		obs_module_text("Recast.Multistream.AutoTip"));
	connect(auto_check_, &QCheckBox::toggled, this,
		[this](bool checked) {
			dest_->auto_start = checked;
			dest_->auto_stop = checked;
			emit autoChanged(this);
		});
	top->addWidget(auto_check_);

	/* Edit button (gear icon) */
	edit_btn_ = new QPushButton;
	edit_btn_->setIcon(edit_btn_->style()->standardIcon(
		QStyle::SP_FileDialogDetailedView));
	edit_btn_->setFlat(true);
	edit_btn_->setFixedSize(24, 24);
	edit_btn_->setToolTip(obs_module_text("Recast.Multistream.Edit"));
	connect(edit_btn_, &QPushButton::clicked, this, [this]() {
		emit editRequested(this);
	});
	top->addWidget(edit_btn_);

	/* Delete button (trash icon) */
	delete_btn_ = new QPushButton;
	delete_btn_->setIcon(delete_btn_->style()->standardIcon(
		QStyle::SP_TrashIcon));
	delete_btn_->setFlat(true);
	delete_btn_->setFixedSize(24, 24);
	delete_btn_->setToolTip(obs_module_text("Recast.DeleteTip"));
	connect(delete_btn_, &QPushButton::clicked, this, [this]() {
		emit deleteRequested(this);
	});
	top->addWidget(delete_btn_);

	/* ---- Bottom row: status, bitrate, dropped, spacer, toggle ---- */
	auto *bottom = new QHBoxLayout;
	bottom->setSpacing(12);

	/* Status dot + elapsed time */
	status_label_ = new QLabel;
	status_label_->setTextFormat(Qt::RichText);
	status_label_->setMinimumWidth(80);
	bottom->addWidget(status_label_);

	/* Bitrate */
	bitrate_label_ = new QLabel;
	bitrate_label_->setStyleSheet("color: #999; font-size: 12px;");
	bitrate_label_->setVisible(false);
	bottom->addWidget(bitrate_label_);

	/* Dropped frames */
	dropped_label_ = new QLabel;
	dropped_label_->setStyleSheet("color: #999; font-size: 12px;");
	dropped_label_->setVisible(false);
	bottom->addWidget(dropped_label_);

	bottom->addStretch();

	/* Start/Stop button */
	toggle_btn_ = new QPushButton(obs_module_text("Recast.Start"));
	toggle_btn_->setFixedWidth(70);
	toggle_btn_->setStyleSheet(
		"QPushButton { background: #2e7d32; color: white; "
		"border-radius: 3px; padding: 4px 8px; font-weight: bold; }"
		"QPushButton:hover { background: #388e3c; }");
	connect(toggle_btn_, &QPushButton::clicked,
		this, &RecastDestinationRow::onToggleStream);
	bottom->addWidget(toggle_btn_);

	card->addLayout(top);
	card->addLayout(bottom);

	refreshStatus();
}

RecastDestinationRow::~RecastDestinationRow() {}

void RecastDestinationRow::refreshStatus()
{
	if (!dest_)
		return;

	if (dest_->active) {
		uint64_t sec = recast_destination_elapsed_sec(dest_);
		uint64_t h = sec / 3600;
		uint64_t m = (sec % 3600) / 60;
		uint64_t s = sec % 60;

		QString time_str;
		if (h > 0)
			time_str = QString("%1h %2m").arg(h).arg(m);
		else
			time_str = QString("%1m %2s").arg(m).arg(s);

		status_label_->setText(
			QString("<span style='color:#4CAF50;'>"
				"\xe2\x97\x8f</span> %1").arg(time_str));
		status_label_->setStyleSheet(
			"color: #4CAF50; font-weight: bold;");
		toggle_btn_->setText(obs_module_text("Recast.Stop"));
		toggle_btn_->setStyleSheet(
			"QPushButton { background: #c62828; color: white; "
			"border-radius: 3px; padding: 4px 8px; "
			"font-weight: bold; }"
			"QPushButton:hover { background: #e53935; }");

		/* Bitrate: compute from byte delta over 1s interval */
		if (dest_->output) {
			uint64_t total =
				obs_output_get_total_bytes(dest_->output);
			uint64_t delta = total - last_bytes_;
			last_bytes_ = total;
			int kbps = (int)(delta * 8 / 1000);
			bitrate_label_->setText(
				QString("%1 kbps").arg(kbps));
			bitrate_label_->setVisible(true);

			/* Dropped frames */
			int dropped = obs_output_get_frames_dropped(
				dest_->output);
			dropped_label_->setText(
				QString("%1 dropped").arg(dropped));
			dropped_label_->setStyleSheet(
				dropped > 0
					? "color: #ef5350; font-size: 12px;"
					: "color: #999; font-size: 12px;");
			dropped_label_->setVisible(true);
		}
	} else {
		status_label_->setText(QString("<span style='color:#999;'>"
			"\xe2\x97\x8f</span> %1")
			.arg(obs_module_text("Recast.Status.Stopped")));
		status_label_->setStyleSheet("color: #999;");
		toggle_btn_->setText(obs_module_text("Recast.Start"));
		toggle_btn_->setStyleSheet(
			"QPushButton { background: #2e7d32; color: white; "
			"border-radius: 3px; padding: 4px 8px; "
			"font-weight: bold; }"
			"QPushButton:hover { background: #388e3c; }");

		/* Hide health stats when stopped */
		bitrate_label_->setVisible(false);
		dropped_label_->setVisible(false);
		last_bytes_ = 0;
	}
}

void RecastDestinationRow::onToggleStream()
{
	if (!dest_)
		return;

	if (dest_->active) {
		recast_destination_stop(dest_);
	} else {
		if (!recast_destination_start(dest_)) {
			QMessageBox::warning(
				this, obs_module_text("Recast.Error"),
				obs_module_text(
					"Recast.Multistream.StartFailed"));
		}
	}
	refreshStatus();
}

/* ====================================================================
 * RecastMultistreamDock
 * ==================================================================== */

RecastMultistreamDock::RecastMultistreamDock(QWidget *parent)
	: QWidget(parent)
{
	auto *main_layout = new QVBoxLayout(this);
	main_layout->setContentsMargins(0, 0, 0, 0);

	auto *scroll = new QScrollArea;
	scroll->setWidgetResizable(true);

	auto *container = new QWidget;
	auto *root_layout = new QVBoxLayout(container);
	root_layout->setAlignment(Qt::AlignTop);

	/* Empty state label */
	empty_label_ = new QLabel(
		"No destinations configured.\n"
		"Click \"Add Destination\" to get started.");
	empty_label_->setAlignment(Qt::AlignCenter);
	empty_label_->setStyleSheet(
		"color: #999; font-style: italic; padding: 24px;");
	root_layout->addWidget(empty_label_);

	/* Destination rows container */
	rows_layout_ = new QVBoxLayout;
	rows_layout_->setAlignment(Qt::AlignTop);
	rows_layout_->setSpacing(2);
	root_layout->addLayout(rows_layout_);

	/* Start All / Stop All row */
	auto *all_row = new QHBoxLayout;

	start_all_btn_ = new QPushButton("Start All");
	start_all_btn_->setStyleSheet(
		"QPushButton { background: #2e7d32; color: white; "
		"border-radius: 3px; padding: 4px 12px; font-weight: bold; }"
		"QPushButton:hover { background: #388e3c; }"
		"QPushButton:disabled { background: #555; color: #888; }");
	connect(start_all_btn_, &QPushButton::clicked,
		this, &RecastMultistreamDock::onStartAll);
	all_row->addWidget(start_all_btn_);

	stop_all_btn_ = new QPushButton("Stop All");
	stop_all_btn_->setStyleSheet(
		"QPushButton { background: #c62828; color: white; "
		"border-radius: 3px; padding: 4px 12px; font-weight: bold; }"
		"QPushButton:hover { background: #e53935; }"
		"QPushButton:disabled { background: #555; color: #888; }");
	connect(stop_all_btn_, &QPushButton::clicked,
		this, &RecastMultistreamDock::onStopAll);
	all_row->addWidget(stop_all_btn_);

	all_row->addStretch();
	root_layout->addLayout(all_row);

	/* Buttons row */
	auto *buttons_row = new QHBoxLayout;

	auto *add_btn = new QPushButton(
		obs_module_text("Recast.Multistream.AddDest"));
	connect(add_btn, &QPushButton::clicked,
		this, &RecastMultistreamDock::onAddDestination);
	buttons_row->addWidget(add_btn);

	auto *sync_btn = new QPushButton(
		obs_module_text("Recast.SyncServer"));
	connect(sync_btn, &QPushButton::clicked,
		this, &RecastMultistreamDock::onSyncServer);
	buttons_row->addWidget(sync_btn);

	root_layout->addLayout(buttons_row);

	root_layout->addStretch();

	scroll->setWidget(container);
	main_layout->addWidget(scroll);

	/* Initial empty/button state */
	updateButtonStates();

	/* Network manager */
	net_mgr_ = new QNetworkAccessManager(this);

	/* Refresh timer */
	refresh_timer_ = new QTimer(this);
	connect(refresh_timer_, &QTimer::timeout,
		this, &RecastMultistreamDock::onRefreshTimer);
	refresh_timer_->start(1000);

	/* Register for main stream events */
	obs_frontend_add_event_callback(onFrontendEvent, this);
}

RecastMultistreamDock::~RecastMultistreamDock()
{
	refresh_timer_->stop();
	obs_frontend_remove_event_callback(onFrontendEvent, this);

	/* Stop and destroy all destinations */
	for (auto *row : rows_) {
		recast_destination_t *d = row->destination();
		if (d)
			recast_destination_destroy(d);
	}
	rows_.clear();
}

void RecastMultistreamDock::onAddDestination()
{
	RecastDestinationDialog dlg(this);
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

	/* Validate URL format */
	if (!url.startsWith("rtmp://") && !url.startsWith("rtmps://")
	    && !url.startsWith("srt://")) {
		QMessageBox::warning(
			this, obs_module_text("Recast.Error"),
			"URL must start with rtmp://, rtmps://, or srt://");
		return;
	}

	recast_destination_t *dest = recast_destination_create(
		name.toUtf8().constData(),
		url.toUtf8().constData(),
		key.toUtf8().constData(),
		dlg.getCanvasVertical());

	addRow(dest);
	emit configChanged();
}

void RecastMultistreamDock::onEditDestination(RecastDestinationRow *row)
{
	if (!row)
		return;

	recast_destination_t *d = row->destination();

	RecastDestinationDialog dlg(
		this,
		QString::fromUtf8(d->name),
		QString::fromUtf8(d->url),
		QString::fromUtf8(d->key),
		d->canvas_vertical);

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

	/* Stop if active before changing settings */
	bool was_active = d->active;
	if (was_active)
		recast_destination_stop(d);

	/* Update fields */
	bfree(d->name);
	d->name = bstrdup(name.toUtf8().constData());
	bfree(d->url);
	d->url = bstrdup(url.toUtf8().constData());
	bfree(d->key);
	d->key = bstrdup(key.toUtf8().constData());
	d->canvas_vertical = dlg.getCanvasVertical();
	d->protocol = recast_protocol_detect(d->url);

	/* Recreate service with new settings */
	if (d->service) {
		obs_service_release(d->service);
		d->service = nullptr;
	}
	const char *service_id = recast_protocol_service_id(d->protocol);
	obs_data_t *svc_settings = obs_data_create();
	if (d->protocol == RECAST_PROTO_WHIP) {
		obs_data_set_string(svc_settings, "server", d->url);
		obs_data_set_string(svc_settings, "bearer_token", d->key);
	} else {
		obs_data_set_string(svc_settings, "server", d->url);
		obs_data_set_string(svc_settings, "key", d->key);
	}
	struct dstr svc_name = {0};
	dstr_printf(&svc_name, "recast_svc_%s", d->name);
	d->service = obs_service_create(
		service_id, svc_name.array, svc_settings, NULL);
	dstr_free(&svc_name);
	obs_data_release(svc_settings);

	/* Recreate output with new protocol */
	if (d->output) {
		obs_output_release(d->output);
		d->output = nullptr;
	}
	const char *output_id = recast_protocol_output_id(d->protocol);
	struct dstr out_name = {0};
	dstr_printf(&out_name, "recast_out_%s", d->name);
	d->output = obs_output_create(
		output_id, out_name.array, NULL, NULL);
	dstr_free(&out_name);

	/* Rebuild the row UI by removing and re-adding */
	auto it = std::find(rows_.begin(), rows_.end(), row);
	int pos = (it != rows_.end()) ? (int)(it - rows_.begin()) : -1;

	if (pos >= 0) {
		rows_.erase(it);
		rows_layout_->removeWidget(row);
		row->deleteLater();

		auto *new_row = new RecastDestinationRow(d, this);
		connect(new_row, &RecastDestinationRow::deleteRequested,
			this, &RecastMultistreamDock::onDeleteDestination);
		connect(new_row, &RecastDestinationRow::editRequested,
			this, &RecastMultistreamDock::onEditDestination);
		connect(new_row, &RecastDestinationRow::autoChanged,
			this, [this](RecastDestinationRow *) {
				emit configChanged();
			});
		rows_layout_->insertWidget(pos, new_row);
		rows_.insert(rows_.begin() + pos, new_row);
	}

	emit configChanged();
}

void RecastMultistreamDock::onDeleteDestination(RecastDestinationRow *row)
{
	if (!row)
		return;

	auto answer = QMessageBox::question(
		this, obs_module_text("Recast.Confirm"),
		QString("%1 '%2'?")
			.arg(obs_module_text("Recast.ConfirmDelete"))
			.arg(row->destination()->name));

	if (answer != QMessageBox::Yes)
		return;

	removeRow(row);
	emit configChanged();
}

void RecastMultistreamDock::onSyncServer()
{
	char *token = recast_config_get_server_token();

	/* Always show dialog so user can set/change server URL and token */
	QDialog dlg(this);
	dlg.setWindowTitle(obs_module_text("Recast.SyncServer"));
	dlg.setMinimumWidth(450);

	auto *form = new QFormLayout;

	auto *server_input = new QLineEdit;
	server_input->setPlaceholderText(
		"https://your-server.com/api/stream/status");
	/* Load saved server URL from config if available */
	char *config_path = recast_config_get_path();
	if (config_path) {
		obs_data_t *root =
			obs_data_create_from_json_file(config_path);
		if (root) {
			const char *saved_url =
				obs_data_get_string(root, "server_url");
			if (saved_url && *saved_url)
				server_input->setText(
					QString::fromUtf8(saved_url));
			obs_data_release(root);
		}
		bfree(config_path);
	}
	form->addRow("Server URL", server_input);

	auto *token_input = new QLineEdit;
	token_input->setPlaceholderText("API token");
	token_input->setEchoMode(QLineEdit::Password);
	if (token && *token)
		token_input->setText(QString::fromUtf8(token));
	bfree(token);
	form->addRow(obs_module_text("Recast.Key"), token_input);

	auto *btns = new QDialogButtonBox(
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	auto *layout = new QVBoxLayout(&dlg);
	layout->addLayout(form);
	layout->addWidget(btns);

	if (dlg.exec() != QDialog::Accepted)
		return;

	QString server_url = server_input->text().trimmed();
	QString token_str = token_input->text().trimmed();

	if (server_url.isEmpty()) {
		QMessageBox::warning(this,
			obs_module_text("Recast.Error"),
			"Server URL is required.");
		return;
	}

	/* Save token and server URL */
	recast_config_set_server_token(token_str.toUtf8().constData());

	/* Save server URL to config */
	config_path = recast_config_get_path();
	if (config_path) {
		obs_data_t *root =
			obs_data_create_from_json_file(config_path);
		if (!root)
			root = obs_data_create();
		obs_data_set_string(root, "server_url",
			server_url.toUtf8().constData());
		obs_data_save_json(root, config_path);
		obs_data_release(root);
		bfree(config_path);
	}

	/* Build request with token in Authorization header */
	QNetworkRequest req{QUrl(server_url)};
	req.setTransferTimeout(15000); /* 15 second timeout */
	if (!token_str.isEmpty())
		req.setRawHeader("Authorization",
				 ("Bearer " + token_str).toUtf8());
	QNetworkReply *reply = net_mgr_->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			QMessageBox::warning(
				this, obs_module_text("Recast.Error"),
				QString("%1: %2")
					.arg(obs_module_text(
						"Recast.Error.Sync"))
					.arg(reply->errorString()));
			return;
		}

		int http_status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (http_status < 200 || http_status >= 300) {
			QMessageBox::warning(
				this, obs_module_text("Recast.Error"),
				QString("Server returned HTTP %1").arg(http_status));
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

			/* Check if we already have this destination */
			bool exists = false;
			for (auto *row : rows_) {
				if (QString::fromUtf8(
					    row->destination()->url) ==
				    rtmp_url) {
					exists = true;
					break;
				}
			}
			if (exists)
				continue;

			recast_destination_t *dest =
				recast_destination_create(
					name.toUtf8().constData(),
					rtmp_url.toUtf8().constData(),
					key.toUtf8().constData(),
					false);
			addRow(dest);
		}

		emit configChanged();

		QMessageBox::information(
			this, obs_module_text("Recast.SyncServer"),
			obs_module_text("Recast.SyncComplete"));
	});
}

void RecastMultistreamDock::onRefreshTimer()
{
	for (auto *row : rows_)
		row->refreshStatus();
	updateButtonStates();
}

void RecastMultistreamDock::addRow(recast_destination_t *dest)
{
	auto *row = new RecastDestinationRow(dest, this);
	connect(row, &RecastDestinationRow::deleteRequested,
		this, &RecastMultistreamDock::onDeleteDestination);
	connect(row, &RecastDestinationRow::editRequested,
		this, &RecastMultistreamDock::onEditDestination);
	connect(row, &RecastDestinationRow::autoChanged,
		this, [this](RecastDestinationRow *) { emit configChanged(); });
	rows_layout_->addWidget(row);
	rows_.push_back(row);

	empty_label_->setVisible(false);
	updateButtonStates();
}

void RecastMultistreamDock::removeRow(RecastDestinationRow *row)
{
	auto it = std::find(rows_.begin(), rows_.end(), row);
	if (it == rows_.end())
		return;

	recast_destination_t *d = row->destination();
	rows_.erase(it);
	rows_layout_->removeWidget(row);
	row->deleteLater();
	recast_destination_destroy(d);

	empty_label_->setVisible(rows_.empty());
	updateButtonStates();
}

void RecastMultistreamDock::onStartAll()
{
	for (auto *row : rows_) {
		recast_destination_t *d = row->destination();
		if (d && !d->active)
			recast_destination_start(d);
		row->refreshStatus();
	}
	updateButtonStates();
}

void RecastMultistreamDock::onStopAll()
{
	for (auto *row : rows_) {
		recast_destination_t *d = row->destination();
		if (d && d->active)
			recast_destination_stop(d);
		row->refreshStatus();
	}
	updateButtonStates();
}

void RecastMultistreamDock::updateButtonStates()
{
	bool any_active = false;
	bool any_stopped = false;

	for (auto *row : rows_) {
		recast_destination_t *d = row->destination();
		if (d && d->active)
			any_active = true;
		else
			any_stopped = true;
	}

	bool has_rows = !rows_.empty();
	start_all_btn_->setEnabled(has_rows && any_stopped);
	stop_all_btn_->setEnabled(has_rows && any_active);
	start_all_btn_->setVisible(has_rows);
	stop_all_btn_->setVisible(has_rows);
}

void RecastMultistreamDock::onFrontendEvent(enum obs_frontend_event event,
					    void *data)
{
	auto *dock = static_cast<RecastMultistreamDock *>(data);
	if (!dock)
		return;

	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED)
		dock->onMainStreamStarted();
	else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED)
		dock->onMainStreamStopped();
}

void RecastMultistreamDock::onMainStreamStarted()
{
	for (auto *row : rows_) {
		recast_destination_t *d = row->destination();
		if (d && d->auto_start && !d->active) {
			if (recast_destination_start(d)) {
				blog(LOG_INFO,
				     "[Recast] Auto-started destination '%s'",
				     d->name);
			}
			row->refreshStatus();
		}
	}
}

void RecastMultistreamDock::onMainStreamStopped()
{
	for (auto *row : rows_) {
		recast_destination_t *d = row->destination();
		if (d && d->auto_stop && d->active) {
			recast_destination_stop(d);
			blog(LOG_INFO,
			     "[Recast] Auto-stopped destination '%s'",
			     d->name);
			row->refreshStatus();
		}
	}
}

/* ---- Config persistence ---- */

void RecastMultistreamDock::loadDestinations(obs_data_array_t *arr)
{
	if (!arr)
		return;

	size_t count = obs_data_array_count(arr);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);

		const char *name = obs_data_get_string(item, "name");
		const char *url = obs_data_get_string(item, "url");
		const char *key = obs_data_get_string(item, "key");
		const char *canvas_str =
			obs_data_get_string(item, "canvas");
		bool canvas_vertical = canvas_str && strcmp(canvas_str,
							    "vertical") == 0;

		recast_destination_t *dest = recast_destination_create(
			name, url, key, canvas_vertical);
		dest->auto_start = obs_data_get_bool(item, "autoStart");
		dest->auto_stop = obs_data_get_bool(item, "autoStop");

		/* Restore saved ID if present */
		const char *saved_id = obs_data_get_string(item, "id");
		if (saved_id && *saved_id) {
			bfree(dest->id);
			dest->id = bstrdup(saved_id);
		}

		addRow(dest);
		obs_data_release(item);
	}
}

obs_data_array_t *RecastMultistreamDock::saveDestinations() const
{
	obs_data_array_t *arr = obs_data_array_create();

	for (auto *row : rows_) {
		recast_destination_t *d = row->destination();
		obs_data_t *item = obs_data_create();

		obs_data_set_string(item, "id", d->id);
		obs_data_set_string(item, "name", d->name);
		obs_data_set_string(item, "url", d->url);
		obs_data_set_string(item, "key", d->key);
		obs_data_set_string(item, "canvas",
				    d->canvas_vertical ? "vertical" : "main");
		obs_data_set_bool(item, "autoStart", d->auto_start);
		obs_data_set_bool(item, "autoStop", d->auto_stop);

		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}

	return arr;
}
