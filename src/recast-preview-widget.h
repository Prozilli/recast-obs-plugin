#pragma once

#include <QWidget>

extern "C" {
#include <obs.h>
}

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
	void mouseDoubleClickEvent(QMouseEvent *event) override;
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
