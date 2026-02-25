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
#include <obs.h>
#include "recast-output.h"
#include "recast-config.h"
}

class RecastDockManager;

/* ---- Add-Output Dialog ---- */

class QCheckBox;

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
	bool getUsePrivateScenes() const;

private:
	QLineEdit *name_edit;
	QLineEdit *url_edit;
	QLineEdit *key_edit;
	QComboBox *scene_combo;
	QSpinBox *width_spin;
	QSpinBox *height_spin;
	QCheckBox *private_scenes_check;

	void populateScenes();
};

/* ---- Live Preview Widget ---- */

class RecastPreviewWidget : public QWidget {
	Q_OBJECT

public:
	explicit RecastPreviewWidget(QWidget *parent = nullptr);
	~RecastPreviewWidget();

	void SetScene(obs_source_t *scene, int w, int h);
	void ClearScene();

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	obs_display_t *display = nullptr;
	obs_source_t *scene_source = nullptr;
	int canvas_width = 0;
	int canvas_height = 0;

	void CreateDisplay();

	static void DrawCallback(void *param, uint32_t cx, uint32_t cy);
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

	bool isSelected() const { return selected_; }
	void setSelected(bool sel);

signals:
	void deleteRequested(RecastOutputCard *card);
	void clicked(RecastOutputCard *card);

protected:
	void mousePressEvent(QMouseEvent *event) override;

private slots:
	void onToggleStream();

private:
	recast_output_target_t *target_;
	bool selected_ = false;
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
	void onCardClicked(RecastOutputCard *card);
	void onConfigChanged();

private:
	QVBoxLayout *cards_layout;
	QLabel *preview_label;
	RecastPreviewWidget *preview;
	RecastOutputCard *selected_card = nullptr;
	QTimer *refresh_timer;
	QNetworkAccessManager *net_mgr;
	std::vector<RecastOutputCard *> cards;
	RecastDockManager *dock_manager_;

	void loadFromConfig();
	void saveToConfig();
	void addCard(recast_output_target_t *target);
	void removeCard(RecastOutputCard *card);
	void selectCard(RecastOutputCard *card);

	friend class RecastDockManager;
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
