#pragma once

#include <QDockWidget>

class RecastPreviewWidget;

/*
 * RecastVerticalPreviewDock -- Always-present 9:16 preview dock.
 *
 * Wraps RecastPreviewWidget and connects to RecastVertical::activeSceneChanged.
 */

class RecastVerticalPreviewDock : public QDockWidget {
	Q_OBJECT

public:
	explicit RecastVerticalPreviewDock(QWidget *parent = nullptr);
	~RecastVerticalPreviewDock();

	RecastPreviewWidget *previewWidget() const { return preview_; }

public slots:
	void onActiveSceneChanged(int idx);
	void onCanvasSizeChanged(int w, int h);

private:
	RecastPreviewWidget *preview_;
	int canvas_w_ = 1080;
	int canvas_h_ = 1920;

	void refreshScene();
};
