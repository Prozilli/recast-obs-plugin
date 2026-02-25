#pragma once

#include <QDockWidget>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QDialog>
#include <QGroupBox>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>

#include <vector>

extern "C" {
#include "recast-output.h"
#include "recast-config.h"
}

/* ---- Add-Output Dialog ---- */

class RecastAddDialog : public QDialog {
	Q_OBJECT

public:
	explicit RecastAddDialog(QWidget *parent = nullptr);

	QString getName() const;
	QString getUrl() const;
	QString getKey() const;
	QString getScene() const;
	int getWidth() const;
	int getHeight() const;

private:
	QLineEdit *name_edit;
	QLineEdit *url_edit;
	QLineEdit *key_edit;
	QComboBox *scene_combo;
	QSpinBox *width_spin;
	QSpinBox *height_spin;

	void populateScenes();
};

/* ---- Single Output Card ---- */

class RecastOutputCard : public QGroupBox {
	Q_OBJECT

public:
	explicit RecastOutputCard(recast_output_target_t *target,
				  QWidget *parent = nullptr);
	~RecastOutputCard();

	recast_output_target_t *target() const { return target_; }
	void refreshStatus();

signals:
	void deleteRequested(RecastOutputCard *card);

private slots:
	void onToggleStream();

private:
	recast_output_target_t *target_;
	QLabel *status_label;
	QPushButton *toggle_btn;
	QPushButton *delete_btn;
};

/* ---- Main Dock Widget ---- */

class RecastDock : public QDockWidget {
	Q_OBJECT

public:
	explicit RecastDock(QWidget *parent = nullptr);
	~RecastDock();

private slots:
	void onAddOutput();
	void onDeleteOutput(RecastOutputCard *card);
	void onSyncServer();
	void onRefreshTimer();

private:
	QVBoxLayout *cards_layout;
	QTimer *refresh_timer;
	QNetworkAccessManager *net_mgr;
	std::vector<RecastOutputCard *> cards;

	void loadFromConfig();
	void saveToConfig();
	void addCard(recast_output_target_t *target);
	void removeCard(RecastOutputCard *card);
};

#ifdef __cplusplus
extern "C" {
#endif

/* Called from plugin-main.c */
void recast_dock_create(void);
void recast_dock_destroy(void);

#ifdef __cplusplus
}
#endif
