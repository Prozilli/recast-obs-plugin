#pragma once

#include <QVBoxLayout>
#include <QScrollArea>
#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QDialog>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include <vector>

extern "C" {
#include <obs.h>
#include <obs-frontend-api.h>
#include "recast-output.h"
}

/* ---- Simplified destination target ---- */

typedef struct recast_destination {
	char *id;
	char *name;
	char *url;
	char *key;
	bool auto_start;
	bool auto_stop;
	bool canvas_vertical;  /* false = main, true = vertical */

	recast_protocol_t protocol;
	obs_output_t *output;
	obs_service_t *service;

	bool active;
	uint64_t start_time_ns;
} recast_destination_t;

recast_destination_t *recast_destination_create(const char *name,
						const char *url,
						const char *key,
						bool canvas_vertical);
void recast_destination_destroy(recast_destination_t *dest);
bool recast_destination_start(recast_destination_t *dest);
void recast_destination_stop(recast_destination_t *dest);
uint64_t recast_destination_elapsed_sec(const recast_destination_t *dest);

/* ---- Add Destination Dialog ---- */

class RecastDestinationDialog : public QDialog {
	Q_OBJECT

public:
	explicit RecastDestinationDialog(QWidget *parent = nullptr,
		const QString &name = QString(),
		const QString &url = QString(),
		const QString &key = QString(),
		bool canvas_vertical = false);

	QString getName() const;
	QString getUrl() const;
	QString getKey() const;
	bool getCanvasVertical() const;

private:
	QLineEdit *name_edit_;
	QLineEdit *url_edit_;
	QLineEdit *key_edit_;
	QComboBox *canvas_combo_;
};

/* ---- Destination Row Widget ---- */

class RecastDestinationRow : public QFrame {
	Q_OBJECT

public:
	explicit RecastDestinationRow(recast_destination_t *dest,
				      QWidget *parent = nullptr);
	~RecastDestinationRow();

	recast_destination_t *destination() const { return dest_; }
	void refreshStatus();

signals:
	void deleteRequested(RecastDestinationRow *row);
	void editRequested(RecastDestinationRow *row);
	void autoChanged(RecastDestinationRow *row);

private slots:
	void onToggleStream();

private:
	recast_destination_t *dest_;
	QLabel *name_label_;
	QLabel *status_label_;
	QLabel *platform_icon_label_;
	QLabel *canvas_label_;
	QPushButton *toggle_btn_;
	QPushButton *edit_btn_;
	QPushButton *delete_btn_;
	QCheckBox *auto_check_;
};

/* ---- Multistream Dock ---- */

class RecastMultistreamDock : public QWidget {
	Q_OBJECT

public:
	explicit RecastMultistreamDock(QWidget *parent = nullptr);
	~RecastMultistreamDock();

	/* Config save/load (called from plugin-main) */
	void loadDestinations(obs_data_array_t *arr);
	obs_data_array_t *saveDestinations() const;

signals:
	void configChanged();

private slots:
	void onAddDestination();
	void onEditDestination(RecastDestinationRow *row);
	void onDeleteDestination(RecastDestinationRow *row);
	void onSyncServer();
	void onRefreshTimer();

private:
	QVBoxLayout *rows_layout_;
	QTimer *refresh_timer_;
	QNetworkAccessManager *net_mgr_;
	std::vector<RecastDestinationRow *> rows_;

	void addRow(recast_destination_t *dest);
	void removeRow(RecastDestinationRow *row);

	/* Auto start/stop with main OBS stream */
	void onMainStreamStarted();
	void onMainStreamStopped();
	static void onFrontendEvent(enum obs_frontend_event event, void *data);
};
