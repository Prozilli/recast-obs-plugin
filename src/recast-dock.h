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
#include <obs-frontend-api.h>
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

/* Handle indices for resize anchors */
enum RecastHandle {
	HANDLE_NONE = -1,
	HANDLE_TL = 0, HANDLE_T, HANDLE_TR,
	HANDLE_R, HANDLE_BR, HANDLE_B,
	HANDLE_BL, HANDLE_L,
	HANDLE_BODY
};

class RecastPreviewWidget : public QWidget {
	Q_OBJECT

public:
	explicit RecastPreviewWidget(QWidget *parent = nullptr);
	~RecastPreviewWidget();

	void SetScene(obs_source_t *scene, int w, int h);
	void ClearScene();

	/* Enable interactive editing for a scene */
	void SetInteractiveScene(obs_scene_t *scene);
	void ClearInteractiveScene();

	obs_sceneitem_t *GetSelectedItem() const { return selected_item; }

public slots:
	void SetSelectedItem(obs_sceneitem_t *item);

signals:
	void itemSelected(obs_sceneitem_t *item);
	void itemTransformed();

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;

private:
	obs_display_t *display = nullptr;
	obs_source_t *scene_source = nullptr;
	int canvas_width = 0;
	int canvas_height = 0;

	/* Interactive editing state */
	obs_scene_t *interactive_scene = nullptr;
	obs_sceneitem_t *selected_item = nullptr;
	bool dragging = false;
	int drag_handle = HANDLE_NONE;
	QPointF drag_start_mouse;
	float item_start_pos_x = 0, item_start_pos_y = 0;
	float item_start_scale_x = 0, item_start_scale_y = 0;
	float item_start_width = 0, item_start_height = 0;

	void CreateDisplay();
	QPointF WidgetToScene(QPoint widget_pos);
	obs_sceneitem_t *HitTestItems(float scene_x, float scene_y);
	int HitTestHandles(float scene_x, float scene_y);
	void GetItemRect(obs_sceneitem_t *item, float &x, float &y,
			 float &w, float &h);

	static void DrawCallback(void *param, uint32_t cx, uint32_t cy);
	static void DrawSelectionOverlay(RecastPreviewWidget *widget);
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
	void autoChanged(RecastOutputCard *card);

protected:
	void mousePressEvent(QMouseEvent *event) override;

private slots:
	void onToggleStream();

private:
	recast_output_target_t *target_;
	bool selected_ = false;
	QLabel *status_label;
	QLabel *platform_icon_label;
	QLabel *protocol_label;
	QPushButton *toggle_btn;
	QPushButton *delete_btn;
	QCheckBox *auto_check;
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

	/* Auto start/stop with main OBS stream */
	void onMainStreamStarted();
	void onMainStreamStopped();
	void onMainSceneChanged();
	static void onFrontendEvent(enum obs_frontend_event event, void *data);

	/* Hotkeys */
	struct OutputHotkeys {
		obs_hotkey_id start_stream;
		obs_hotkey_id stop_stream;
		obs_hotkey_id start_record;
		obs_hotkey_id stop_record;
	};
	std::vector<OutputHotkeys> hotkeys_;

	void registerHotkeys(recast_output_target_t *target);
	void unregisterHotkeys(size_t index);

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
