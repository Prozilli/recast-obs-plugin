/*
 * recast-preview-widget.cpp -- Extracted live preview widget.
 *
 * Interactive drag/resize, selection overlay, draw callback with safe refs.
 * Standalone widget used by the vertical preview dock.
 */

#include "recast-preview-widget.h"

#include <QAction>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QWindow>

#include <cmath>

extern "C" {
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <obs-frontend-api.h>
}

/* ---- Helpers ---- */

static inline void GetScaleAndCenterPos(int baseCX, int baseCY, int windowCX,
					int windowCY, int &x, int &y,
					float &scale)
{
	double windowAspect = double(windowCX) / double(windowCY);
	double baseAspect = double(baseCX) / double(baseCY);
	int newCX, newCY;

	if (windowAspect > baseAspect) {
		scale = float(windowCY) / float(baseCY);
		newCX = int(double(windowCY) * baseAspect);
		newCY = windowCY;
	} else {
		scale = float(windowCX) / float(baseCX);
		newCX = windowCX;
		newCY = int(float(windowCX) / baseAspect);
	}

	x = windowCX / 2 - newCX / 2;
	y = windowCY / 2 - newCY / 2;
}

/* ---- Source dimensions after crop ---- */

static void GetCroppedSize(obs_sceneitem_t *item, float &cx, float &cy)
{
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src) { cx = cy = 0; return; }

	cx = (float)obs_source_get_width(src);
	cy = (float)obs_source_get_height(src);

	struct obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);

	cx -= (float)(crop.left + crop.right);
	cy -= (float)(crop.top + crop.bottom);
	if (cx < 1.0f) cx = 1.0f;
	if (cy < 1.0f) cy = 1.0f;
}

/* ---- Transform helpers ---- */

void RecastPreviewWidget::FitToCanvas(obs_sceneitem_t *item, int cw, int ch)
{
	if (!item) return;

	float src_cx, src_cy;
	GetCroppedSize(item, src_cx, src_cy);
	if (src_cx <= 0 || src_cy <= 0) return;

	float scale = std::min((float)cw / src_cx, (float)ch / src_cy);

	struct vec2 sc = {scale, scale};
	obs_sceneitem_set_scale(item, &sc);

	float disp_w = src_cx * scale;
	float disp_h = src_cy * scale;
	struct vec2 pos = {((float)cw - disp_w) / 2.0f,
			   ((float)ch - disp_h) / 2.0f};
	obs_sceneitem_set_pos(item, &pos);
}

void RecastPreviewWidget::StretchToCanvas(obs_sceneitem_t *item, int cw, int ch)
{
	if (!item) return;

	float src_cx, src_cy;
	GetCroppedSize(item, src_cx, src_cy);
	if (src_cx <= 0 || src_cy <= 0) return;

	struct vec2 sc = {(float)cw / src_cx, (float)ch / src_cy};
	obs_sceneitem_set_scale(item, &sc);

	struct vec2 pos = {0.0f, 0.0f};
	obs_sceneitem_set_pos(item, &pos);
}

void RecastPreviewWidget::CenterOnCanvas(obs_sceneitem_t *item, int cw, int ch)
{
	if (!item) return;

	float src_cx, src_cy;
	GetCroppedSize(item, src_cx, src_cy);

	struct vec2 sc;
	obs_sceneitem_get_scale(item, &sc);

	float disp_w = src_cx * std::fabs(sc.x);
	float disp_h = src_cy * std::fabs(sc.y);

	struct vec2 pos;
	pos.x = ((float)cw - disp_w) / 2.0f;
	pos.y = ((float)ch - disp_h) / 2.0f;

	/* For flipped items, adjust position so visible area centers */
	if (sc.x < 0.0f) pos.x += disp_w;
	if (sc.y < 0.0f) pos.y += disp_h;

	obs_sceneitem_set_pos(item, &pos);
}

void RecastPreviewWidget::FlipHorizontal(obs_sceneitem_t *item, int cw)
{
	if (!item) return;

	float src_cx, src_cy;
	GetCroppedSize(item, src_cx, src_cy);

	struct vec2 sc;
	obs_sceneitem_get_scale(item, &sc);
	struct vec2 pos;
	obs_sceneitem_get_pos(item, &pos);

	float disp_w = src_cx * sc.x; /* signed width */
	sc.x = -sc.x;
	pos.x = pos.x + disp_w;

	obs_sceneitem_set_scale(item, &sc);
	obs_sceneitem_set_pos(item, &pos);
}

void RecastPreviewWidget::FlipVertical(obs_sceneitem_t *item, int ch)
{
	if (!item) return;

	float src_cx, src_cy;
	GetCroppedSize(item, src_cx, src_cy);

	struct vec2 sc;
	obs_sceneitem_get_scale(item, &sc);
	struct vec2 pos;
	obs_sceneitem_get_pos(item, &pos);

	float disp_h = src_cy * sc.y; /* signed height */
	sc.y = -sc.y;
	pos.y = pos.y + disp_h;

	obs_sceneitem_set_scale(item, &sc);
	obs_sceneitem_set_pos(item, &pos);
}

void RecastPreviewWidget::CenterHorizontal(obs_sceneitem_t *item, int cw)
{
	if (!item) return;

	float src_cx, src_cy;
	GetCroppedSize(item, src_cx, src_cy);

	struct vec2 sc;
	obs_sceneitem_get_scale(item, &sc);

	float disp_w = src_cx * std::fabs(sc.x);

	struct vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	pos.x = ((float)cw - disp_w) / 2.0f;
	if (sc.x < 0.0f) pos.x += disp_w;

	obs_sceneitem_set_pos(item, &pos);
}

void RecastPreviewWidget::CenterVertical(obs_sceneitem_t *item, int ch)
{
	if (!item) return;

	float src_cx, src_cy;
	GetCroppedSize(item, src_cx, src_cy);

	struct vec2 sc;
	obs_sceneitem_get_scale(item, &sc);

	float disp_h = src_cy * std::fabs(sc.y);

	struct vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	pos.y = ((float)ch - disp_h) / 2.0f;
	if (sc.y < 0.0f) pos.y += disp_h;

	obs_sceneitem_set_pos(item, &pos);
}

void RecastPreviewWidget::Rotate90CW(obs_sceneitem_t *item)
{
	if (!item) return;
	float rot = obs_sceneitem_get_rot(item);
	rot = std::fmod(rot + 90.0f, 360.0f);
	if (rot < 0.0f) rot += 360.0f;
	obs_sceneitem_set_rot(item, rot);
}

void RecastPreviewWidget::Rotate90CCW(obs_sceneitem_t *item)
{
	if (!item) return;
	float rot = obs_sceneitem_get_rot(item);
	rot = std::fmod(rot - 90.0f, 360.0f);
	if (rot < 0.0f) rot += 360.0f;
	obs_sceneitem_set_rot(item, rot);
}

void RecastPreviewWidget::ResetTransform(obs_sceneitem_t *item)
{
	if (!item) return;

	struct vec2 pos = {0.0f, 0.0f};
	struct vec2 sc = {1.0f, 1.0f};
	struct obs_sceneitem_crop crop = {0, 0, 0, 0};

	obs_sceneitem_set_pos(item, &pos);
	obs_sceneitem_set_scale(item, &sc);
	obs_sceneitem_set_rot(item, 0.0f);
	obs_sceneitem_set_crop(item, &crop);
}

/* ---- Constructor / Destructor ---- */

RecastPreviewWidget::RecastPreviewWidget(QWidget *parent)
	: QWidget(parent)
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_NativeWindow);

	setMouseTracking(true);
	setFocusPolicy(Qt::ClickFocus);

	setMinimumHeight(100);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

RecastPreviewWidget::~RecastPreviewWidget()
{
	if (display) {
		obs_display_remove_draw_callback(display, DrawCallback, this);
		obs_display_destroy(display);
		display = nullptr;
	}
	if (selected_item) {
		obs_sceneitem_release(selected_item);
		selected_item = nullptr;
	}
	if (scene_source) {
		obs_source_release(scene_source);
		scene_source = nullptr;
	}
}

void RecastPreviewWidget::CreateDisplay()
{
	if (display)
		return;

	if (!windowHandle() || !windowHandle()->isExposed())
		return;

	QSize size = this->size() * this->devicePixelRatioF();

	gs_init_data info = {};
	info.cx = size.width();
	info.cy = size.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;

#ifdef _WIN32
	info.window.hwnd = (HWND)windowHandle()->winId();
#elif __APPLE__
	info.window.view = (id)windowHandle()->winId();
#else
	info.window.id = windowHandle()->winId();
	info.window.display = obs_get_nix_platform_display();
#endif

	/* Dark gray surround matching OBS main preview — ABGR format */
	display = obs_display_create(&info, 0xFF2D2D2D);
	obs_display_add_draw_callback(display, DrawCallback, this);
}

void RecastPreviewWidget::SetScene(obs_source_t *scene, int w, int h)
{
	if (scene_source) {
		obs_source_release(scene_source);
		scene_source = nullptr;
	}
	if (scene) {
		scene_source = obs_source_get_ref(scene);
	}
	canvas_width = w;
	canvas_height = h;
}

void RecastPreviewWidget::ClearScene()
{
	if (scene_source) {
		obs_source_release(scene_source);
		scene_source = nullptr;
	}
	canvas_width = 0;
	canvas_height = 0;
	if (selected_item) {
		obs_sceneitem_release(selected_item);
		selected_item = nullptr;
	}
	interactive_scene = nullptr;
}

void RecastPreviewWidget::SetInteractiveScene(obs_scene_t *scene)
{
	interactive_scene = scene;
	if (selected_item) {
		obs_sceneitem_release(selected_item);
		selected_item = nullptr;
	}
	dragging = false;
}

void RecastPreviewWidget::ClearInteractiveScene()
{
	interactive_scene = nullptr;
	if (selected_item) {
		obs_sceneitem_release(selected_item);
		selected_item = nullptr;
	}
	dragging = false;
}

void RecastPreviewWidget::SetSelectedItem(obs_sceneitem_t *item)
{
	if (selected_item)
		obs_sceneitem_release(selected_item);
	selected_item = item;
	if (selected_item)
		obs_sceneitem_addref(selected_item);
}

/* ---- Coordinate conversion ---- */

QPointF RecastPreviewWidget::WidgetToScene(QPoint pos)
{
	if (canvas_width <= 0 || canvas_height <= 0)
		return QPointF(0, 0);

	float dpr = (float)devicePixelRatioF();
	int widgetCX = (int)(width() * dpr);
	int widgetCY = (int)(height() * dpr);

	int vx, vy;
	float scale;
	GetScaleAndCenterPos(canvas_width, canvas_height,
			     widgetCX, widgetCY, vx, vy, scale);

	if (scale <= 0.0f)
		return QPointF(0, 0);

	float scene_x = (pos.x() * dpr - vx) / scale;
	float scene_y = (pos.y() * dpr - vy) / scale;
	return QPointF(scene_x, scene_y);
}

/* ---- Item bounding rect (no rotation) ---- */

void RecastPreviewWidget::GetItemRect(obs_sceneitem_t *item,
				      float &rx, float &ry,
				      float &rw, float &rh)
{
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src) {
		rx = ry = rw = rh = 0;
		return;
	}
	float cx = (float)obs_source_get_width(src);
	float cy = (float)obs_source_get_height(src);

	struct vec2 pos, sc;
	obs_sceneitem_get_pos(item, &pos);
	obs_sceneitem_get_scale(item, &sc);

	struct obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);

	float crop_cx = cx - (float)(crop.left + crop.right);
	float crop_cy = cy - (float)(crop.top + crop.bottom);
	if (crop_cx < 1.0f) crop_cx = 1.0f;
	if (crop_cy < 1.0f) crop_cy = 1.0f;

	rx = pos.x;
	ry = pos.y;
	rw = crop_cx * sc.x;
	rh = crop_cy * sc.y;
}

/* ---- Hit testing ---- */

struct hit_test_ctx {
	float mx, my;
	obs_sceneitem_t *result;
};

static bool hit_test_cb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<struct hit_test_ctx *>(param);

	if (!obs_sceneitem_visible(item))
		return true;

	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return true;
	float cx = (float)obs_source_get_width(src);
	float cy = (float)obs_source_get_height(src);
	if (cx <= 0 || cy <= 0)
		return true;

	/* Use box transform + inverse for accurate hit test */
	struct matrix4 box_transform;
	obs_sceneitem_get_box_transform(item, &box_transform);

	struct matrix4 inv;
	matrix4_inv(&inv, &box_transform);

	struct vec3 scene_pt;
	vec3_set(&scene_pt, ctx->mx, ctx->my, 0.0f);

	struct vec3 local_pt;
	vec3_transform(&local_pt, &scene_pt, &inv);

	if (local_pt.x >= 0.0f && local_pt.x <= cx &&
	    local_pt.y >= 0.0f && local_pt.y <= cy) {
		ctx->result = item;
	}

	return true; /* continue to find topmost */
}

obs_sceneitem_t *RecastPreviewWidget::HitTestItems(float scene_x,
						    float scene_y)
{
	if (!interactive_scene)
		return nullptr;

	struct hit_test_ctx ctx;
	ctx.mx = scene_x;
	ctx.my = scene_y;
	ctx.result = nullptr;

	obs_scene_enum_items(interactive_scene, hit_test_cb, &ctx);
	return ctx.result;
}

int RecastPreviewWidget::HitTestHandles(float scene_x, float scene_y)
{
	if (!selected_item)
		return HANDLE_NONE;

	float rx, ry, rw, rh;
	GetItemRect(selected_item, rx, ry, rw, rh);

	/* Handle size in scene coords -- scale-independent 8px */
	float dpr = (float)devicePixelRatioF();
	int widgetCX = (int)(width() * dpr);
	int widgetCY = (int)(height() * dpr);
	int vx_unused, vy_unused;
	float view_scale;
	GetScaleAndCenterPos(canvas_width, canvas_height,
			     widgetCX, widgetCY, vx_unused, vy_unused,
			     view_scale);
	float hr = (view_scale > 0.0f) ? (8.0f * dpr / view_scale) : 8.0f;

	/* 8 handle positions */
	struct { float x, y; } handles[8] = {
		{rx,          ry},           /* TL */
		{rx + rw / 2, ry},           /* T  */
		{rx + rw,     ry},           /* TR */
		{rx + rw,     ry + rh / 2},  /* R  */
		{rx + rw,     ry + rh},      /* BR */
		{rx + rw / 2, ry + rh},      /* B  */
		{rx,          ry + rh},      /* BL */
		{rx,          ry + rh / 2},  /* L  */
	};

	for (int i = 0; i < 8; i++) {
		float dx = scene_x - handles[i].x;
		float dy = scene_y - handles[i].y;
		if (std::fabs(dx) <= hr && std::fabs(dy) <= hr)
			return i;
	}

	/* Check body */
	if (scene_x >= rx && scene_x <= rx + rw &&
	    scene_y >= ry && scene_y <= ry + rh)
		return HANDLE_BODY;

	return HANDLE_NONE;
}

/* ---- Mouse events ---- */

void RecastPreviewWidget::mousePressEvent(QMouseEvent *event)
{
	if (!interactive_scene) {
		QWidget::mousePressEvent(event);
		return;
	}

	/* Right-click: context menu */
	if (event->button() == Qt::RightButton) {
		/* Hit-test and select item under cursor */
		QPointF sp = WidgetToScene(event->pos());
		obs_sceneitem_t *hit = HitTestItems((float)sp.x(),
						    (float)sp.y());
		if (hit) {
			if (selected_item)
				obs_sceneitem_release(selected_item);
			selected_item = hit;
			obs_sceneitem_addref(selected_item);
			emit itemSelected(selected_item);
		} else {
			if (selected_item) {
				obs_sceneitem_release(selected_item);
				selected_item = nullptr;
				emit itemSelected(nullptr);
			}
		}
		ShowContextMenu(event->pos());
		return;
	}

	if (event->button() != Qt::LeftButton) {
		QWidget::mousePressEvent(event);
		return;
	}

	QPointF sp = WidgetToScene(event->pos());
	float sx = (float)sp.x();
	float sy = (float)sp.y();

	/* Hit test items first — if nothing under cursor, deselect */
	obs_sceneitem_t *hit = HitTestItems(sx, sy);

	if (!hit) {
		if (selected_item)
			obs_sceneitem_release(selected_item);
		selected_item = nullptr;
		emit itemSelected(nullptr);
		return;
	}

	/* If clicking on the already-selected item, check handles */
	if (selected_item && hit == selected_item &&
	    !obs_sceneitem_locked(selected_item)) {
		int handle = HitTestHandles(sx, sy);
		if (handle != HANDLE_NONE) {
			dragging = true;
			drag_handle = handle;
			drag_start_mouse = sp;

			struct vec2 pos, sc;
			obs_sceneitem_get_pos(selected_item, &pos);
			obs_sceneitem_get_scale(selected_item, &sc);
			item_start_pos_x = pos.x;
			item_start_pos_y = pos.y;
			item_start_scale_x = sc.x;
			item_start_scale_y = sc.y;

			obs_source_t *src =
				obs_sceneitem_get_source(selected_item);
			if (!src) return;
			struct obs_sceneitem_crop crop;
			obs_sceneitem_get_crop(selected_item, &crop);
			item_start_width =
				((float)obs_source_get_width(src) -
				 (float)(crop.left + crop.right));
			item_start_height =
				((float)obs_source_get_height(src) -
				 (float)(crop.top + crop.bottom));
			if (item_start_width < 1.0f) item_start_width = 1.0f;
			if (item_start_height < 1.0f)
				item_start_height = 1.0f;
			return;
		}
	}

	/* Select the hit item */
	if (selected_item)
		obs_sceneitem_release(selected_item);
	selected_item = hit;
	obs_sceneitem_addref(selected_item);
	emit itemSelected(selected_item);

	if (!obs_sceneitem_locked(hit)) {
		dragging = true;
		drag_handle = HANDLE_BODY;
		drag_start_mouse = sp;

		struct vec2 pos, sc;
		obs_sceneitem_get_pos(hit, &pos);
		obs_sceneitem_get_scale(hit, &sc);
		item_start_pos_x = pos.x;
		item_start_pos_y = pos.y;
		item_start_scale_x = sc.x;
		item_start_scale_y = sc.y;

		obs_source_t *src = obs_sceneitem_get_source(hit);
		if (!src) return;
		struct obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(hit, &crop);
		item_start_width = ((float)obs_source_get_width(src) -
				    (float)(crop.left + crop.right));
		item_start_height = ((float)obs_source_get_height(src) -
				     (float)(crop.top + crop.bottom));
		if (item_start_width < 1.0f) item_start_width = 1.0f;
		if (item_start_height < 1.0f) item_start_height = 1.0f;
	}
}

void RecastPreviewWidget::mouseMoveEvent(QMouseEvent *event)
{
	if (!interactive_scene) {
		QWidget::mouseMoveEvent(event);
		return;
	}

	QPointF sp = WidgetToScene(event->pos());
	float sx = (float)sp.x();
	float sy = (float)sp.y();

	/* Update cursor based on handle hover */
	if (!dragging && selected_item) {
		int handle = HitTestHandles(sx, sy);
		switch (handle) {
		case HANDLE_TL: case HANDLE_BR:
			setCursor(Qt::SizeFDiagCursor); break;
		case HANDLE_TR: case HANDLE_BL:
			setCursor(Qt::SizeBDiagCursor); break;
		case HANDLE_T: case HANDLE_B:
			setCursor(Qt::SizeVerCursor); break;
		case HANDLE_L: case HANDLE_R:
			setCursor(Qt::SizeHorCursor); break;
		case HANDLE_BODY:
			setCursor(Qt::SizeAllCursor); break;
		default:
			setCursor(Qt::ArrowCursor); break;
		}
	}

	if (!dragging || !selected_item)
		return;

	float dx = sx - (float)drag_start_mouse.x();
	float dy = sy - (float)drag_start_mouse.y();

	/* Snap threshold in scene coordinates */
	float snap_dist = 10.0f;
	float cw = (float)canvas_width;
	float ch = (float)canvas_height;

	if (drag_handle == HANDLE_BODY) {
		/* Move */
		float new_x = item_start_pos_x + dx;
		float new_y = item_start_pos_y + dy;

		/* Calculate item displayed size for edge snapping */
		float disp_w = item_start_width * item_start_scale_x;
		float disp_h = item_start_height * item_start_scale_y;

		/* Snap edges and center to canvas */
		/* Left edge → 0 */
		if (std::fabs(new_x) < snap_dist)
			new_x = 0.0f;
		/* Right edge → canvas width */
		if (std::fabs(new_x + disp_w - cw) < snap_dist)
			new_x = cw - disp_w;
		/* Top edge → 0 */
		if (std::fabs(new_y) < snap_dist)
			new_y = 0.0f;
		/* Bottom edge → canvas height */
		if (std::fabs(new_y + disp_h - ch) < snap_dist)
			new_y = ch - disp_h;
		/* Horizontal center → canvas center */
		if (std::fabs(new_x + disp_w / 2.0f - cw / 2.0f) < snap_dist)
			new_x = cw / 2.0f - disp_w / 2.0f;
		/* Vertical center → canvas center */
		if (std::fabs(new_y + disp_h / 2.0f - ch / 2.0f) < snap_dist)
			new_y = ch / 2.0f - disp_h / 2.0f;

		struct vec2 new_pos;
		new_pos.x = new_x;
		new_pos.y = new_y;
		obs_sceneitem_set_pos(selected_item, &new_pos);

	} else {
		/* Resize via handles — aspect-ratio locked by default.
		 * Hold Shift to resize freely without aspect lock.
		 *
		 * Aspect ratio preserves the DISPLAYED shape by using
		 * scale_y/scale_x ratio, NOT source width/height. */
		float new_pos_x = item_start_pos_x;
		float new_pos_y = item_start_pos_y;
		float new_sx = item_start_scale_x;
		float new_sy = item_start_scale_y;

		float orig_w = item_start_width * item_start_scale_x;
		float orig_h = item_start_height * item_start_scale_y;

		/* Ratio that preserves displayed aspect */
		float sy_over_sx = (item_start_scale_x != 0.0f)
			? (item_start_scale_y / item_start_scale_x)
			: 1.0f;
		float sx_over_sy = (item_start_scale_y != 0.0f)
			? (item_start_scale_x / item_start_scale_y)
			: 1.0f;

		bool shift = (event->modifiers() & Qt::ShiftModifier);

		switch (drag_handle) {
		case HANDLE_BR:
			new_sx = (orig_w + dx) / item_start_width;
			if (shift) {
				new_sy = (orig_h + dy) / item_start_height;
			} else {
				new_sy = new_sx * sy_over_sx;
			}
			break;
		case HANDLE_R:
			new_sx = (orig_w + dx) / item_start_width;
			if (!shift)
				new_sy = new_sx * sy_over_sx;
			break;
		case HANDLE_B:
			new_sy = (orig_h + dy) / item_start_height;
			if (!shift)
				new_sx = new_sy * sx_over_sy;
			break;
		case HANDLE_TL: {
			new_sx = (orig_w - dx) / item_start_width;
			if (shift) {
				new_sy = (orig_h - dy) / item_start_height;
			} else {
				new_sy = new_sx * sy_over_sx;
			}
			float dw = item_start_width * (new_sx - item_start_scale_x);
			float dh = item_start_height * (new_sy - item_start_scale_y);
			new_pos_x = item_start_pos_x - dw;
			new_pos_y = item_start_pos_y - dh;
			break;
		}
		case HANDLE_T:
			new_sy = (orig_h - dy) / item_start_height;
			if (!shift)
				new_sx = new_sy * sx_over_sy;
			new_pos_y = item_start_pos_y - item_start_height * (new_sy - item_start_scale_y);
			if (!shift)
				new_pos_x = item_start_pos_x - item_start_width * (new_sx - item_start_scale_x) * 0.5f;
			break;
		case HANDLE_L:
			new_sx = (orig_w - dx) / item_start_width;
			if (!shift)
				new_sy = new_sx * sy_over_sx;
			new_pos_x = item_start_pos_x - item_start_width * (new_sx - item_start_scale_x);
			if (!shift)
				new_pos_y = item_start_pos_y - item_start_height * (new_sy - item_start_scale_y) * 0.5f;
			break;
		case HANDLE_TR: {
			new_sx = (orig_w + dx) / item_start_width;
			if (shift) {
				new_sy = (orig_h - dy) / item_start_height;
			} else {
				new_sy = new_sx * sy_over_sx;
			}
			new_pos_y = item_start_pos_y - item_start_height * (new_sy - item_start_scale_y);
			break;
		}
		case HANDLE_BL: {
			new_sx = (orig_w - dx) / item_start_width;
			if (shift) {
				new_sy = (orig_h + dy) / item_start_height;
			} else {
				new_sy = new_sx * sy_over_sx;
			}
			new_pos_x = item_start_pos_x - item_start_width * (new_sx - item_start_scale_x);
			break;
		}
		}

		/* Clamp to minimum */
		if (new_sx < 0.01f) new_sx = 0.01f;
		if (new_sy < 0.01f) new_sy = 0.01f;

		/* Snap resized edges to canvas edges/center */
		float disp_w = item_start_width * new_sx;
		float disp_h = item_start_height * new_sy;
		float right = new_pos_x + disp_w;
		float bottom = new_pos_y + disp_h;

		/* Right edge → canvas width */
		if (std::fabs(right - cw) < snap_dist) {
			disp_w = cw - new_pos_x;
			new_sx = disp_w / item_start_width;
			if (!shift) new_sy = new_sx * sy_over_sx;
		}
		/* Bottom edge → canvas height */
		if (std::fabs(bottom - ch) < snap_dist) {
			disp_h = ch - new_pos_y;
			new_sy = disp_h / item_start_height;
			if (!shift) new_sx = new_sy * sx_over_sy;
		}

		struct vec2 pos_v = {new_pos_x, new_pos_y};
		struct vec2 sc_v = {new_sx, new_sy};
		obs_sceneitem_set_pos(selected_item, &pos_v);
		obs_sceneitem_set_scale(selected_item, &sc_v);
	}

	emit itemTransformed();
}

void RecastPreviewWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (!interactive_scene || event->button() != Qt::LeftButton) {
		QWidget::mouseDoubleClickEvent(event);
		return;
	}

	QPointF sp = WidgetToScene(event->pos());
	obs_sceneitem_t *hit = HitTestItems((float)sp.x(), (float)sp.y());
	if (hit) {
		obs_source_t *src = obs_sceneitem_get_source(hit);
		if (src)
			obs_frontend_open_source_properties(src);
	}
}

void RecastPreviewWidget::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton && dragging) {
		dragging = false;
		drag_handle = HANDLE_NONE;
		if (selected_item)
			emit itemTransformed();
	}
	QWidget::mouseReleaseEvent(event);
}

/* ---- Right-click context menu ---- */

void RecastPreviewWidget::ShowContextMenu(QPoint widget_pos)
{
	QMenu menu(this);

	if (selected_item) {
		obs_source_t *src = obs_sceneitem_get_source(selected_item);
		bool locked = obs_sceneitem_locked(selected_item);
		bool visible = obs_sceneitem_visible(selected_item);

		/* Transform submenu */
		QMenu *transform_menu = menu.addMenu("Transform");

		QAction *fit_act = transform_menu->addAction(
			"Fit to Canvas\tCtrl+F");
		connect(fit_act, &QAction::triggered, this, [this]() {
			FitToCanvas(selected_item, canvas_width, canvas_height);
			emit itemTransformed();
		});

		QAction *stretch_act = transform_menu->addAction(
			"Stretch to Canvas\tCtrl+R");
		connect(stretch_act, &QAction::triggered, this, [this]() {
			StretchToCanvas(selected_item, canvas_width,
					canvas_height);
			emit itemTransformed();
		});

		QAction *center_act = transform_menu->addAction(
			"Center on Canvas\tCtrl+E");
		connect(center_act, &QAction::triggered, this, [this]() {
			CenterOnCanvas(selected_item, canvas_width,
				       canvas_height);
			emit itemTransformed();
		});

		QAction *center_h_act =
			transform_menu->addAction("Center Horizontally");
		connect(center_h_act, &QAction::triggered, this, [this]() {
			CenterHorizontal(selected_item, canvas_width);
			emit itemTransformed();
		});

		QAction *center_v_act =
			transform_menu->addAction("Center Vertically");
		connect(center_v_act, &QAction::triggered, this, [this]() {
			CenterVertical(selected_item, canvas_height);
			emit itemTransformed();
		});

		transform_menu->addSeparator();

		QAction *flip_h_act =
			transform_menu->addAction("Flip Horizontal");
		connect(flip_h_act, &QAction::triggered, this, [this]() {
			FlipHorizontal(selected_item, canvas_width);
			emit itemTransformed();
		});

		QAction *flip_v_act =
			transform_menu->addAction("Flip Vertical");
		connect(flip_v_act, &QAction::triggered, this, [this]() {
			FlipVertical(selected_item, canvas_height);
			emit itemTransformed();
		});

		transform_menu->addSeparator();

		QAction *rot_cw_act =
			transform_menu->addAction("Rotate 90 CW");
		connect(rot_cw_act, &QAction::triggered, this, [this]() {
			Rotate90CW(selected_item);
			emit itemTransformed();
		});

		QAction *rot_ccw_act =
			transform_menu->addAction("Rotate 90 CCW");
		connect(rot_ccw_act, &QAction::triggered, this, [this]() {
			Rotate90CCW(selected_item);
			emit itemTransformed();
		});

		transform_menu->addSeparator();

		QAction *reset_act = transform_menu->addAction(
			"Reset Transform\tCtrl+0");
		connect(reset_act, &QAction::triggered, this, [this]() {
			ResetTransform(selected_item);
			emit itemTransformed();
		});

		menu.addSeparator();

		/* Properties */
		QAction *props_act = menu.addAction("Properties");
		connect(props_act, &QAction::triggered, this, [src]() {
			if (src)
				obs_frontend_open_source_properties(src);
		});

		/* Filters */
		QAction *filters_act = menu.addAction("Filters");
		connect(filters_act, &QAction::triggered, this, [src]() {
			if (src)
				obs_frontend_open_source_filters(src);
		});

		menu.addSeparator();

		/* Lock / Unlock */
		QAction *lock_act = menu.addAction(locked ? "Unlock" : "Lock");
		connect(lock_act, &QAction::triggered, this,
			[this, locked]() {
				obs_sceneitem_set_locked(selected_item,
							 !locked);
				emit itemTransformed();
			});

		/* Hide / Show */
		QAction *vis_act =
			menu.addAction(visible ? "Hide" : "Show");
		connect(vis_act, &QAction::triggered, this,
			[this, visible]() {
				obs_sceneitem_set_visible(selected_item,
							  !visible);
				emit itemTransformed();
			});

		menu.addSeparator();

		/* Remove */
		QAction *remove_act = menu.addAction("Remove");
		connect(remove_act, &QAction::triggered, this, [this]() {
			QMessageBox::StandardButton reply =
				QMessageBox::question(
					this, "Remove Source",
					"Remove this source from the scene?",
					QMessageBox::Yes | QMessageBox::No);
			if (reply == QMessageBox::Yes) {
				obs_sceneitem_t *item = selected_item;
				selected_item = nullptr;
				emit itemSelected(nullptr);
				obs_sceneitem_remove(item);
				obs_sceneitem_release(item);
				emit itemTransformed();
			}
		});
	} else {
		QAction *no_sel = menu.addAction("No source selected");
		no_sel->setEnabled(false);
	}

	menu.exec(mapToGlobal(widget_pos));
}

/* ---- Keyboard shortcuts ---- */

void RecastPreviewWidget::keyPressEvent(QKeyEvent *event)
{
	if (!interactive_scene || !selected_item) {
		QWidget::keyPressEvent(event);
		return;
	}

	bool handled = true;
	int key = event->key();
	Qt::KeyboardModifiers mods = event->modifiers();

	/* Arrow key nudge */
	if (key == Qt::Key_Left || key == Qt::Key_Right ||
	    key == Qt::Key_Up || key == Qt::Key_Down) {
		if (obs_sceneitem_locked(selected_item))
			return;

		float step = (mods & Qt::ShiftModifier) ? 10.0f : 1.0f;
		struct vec2 pos;
		obs_sceneitem_get_pos(selected_item, &pos);

		switch (key) {
		case Qt::Key_Left:  pos.x -= step; break;
		case Qt::Key_Right: pos.x += step; break;
		case Qt::Key_Up:    pos.y -= step; break;
		case Qt::Key_Down:  pos.y += step; break;
		}

		obs_sceneitem_set_pos(selected_item, &pos);
		emit itemTransformed();

	/* Delete: remove with confirmation */
	} else if (key == Qt::Key_Delete) {
		QMessageBox::StandardButton reply = QMessageBox::question(
			this, "Remove Source",
			"Remove this source from the scene?",
			QMessageBox::Yes | QMessageBox::No);
		if (reply == QMessageBox::Yes) {
			obs_sceneitem_t *item = selected_item;
			selected_item = nullptr;
			emit itemSelected(nullptr);
			obs_sceneitem_remove(item);
			obs_sceneitem_release(item);
			emit itemTransformed();
		}

	/* Ctrl+D: duplicate selected source */
	} else if (key == Qt::Key_D && (mods & Qt::ControlModifier)) {
		obs_source_t *src = obs_sceneitem_get_source(selected_item);
		if (src && interactive_scene) {
			obs_sceneitem_t *dup =
				obs_scene_add(interactive_scene, src);
			if (dup) {
				/* Copy transform from original */
				struct vec2 pos, sc;
				obs_sceneitem_get_pos(selected_item, &pos);
				obs_sceneitem_get_scale(selected_item, &sc);
				float rot = obs_sceneitem_get_rot(selected_item);
				struct obs_sceneitem_crop crop;
				obs_sceneitem_get_crop(selected_item, &crop);

				/* Offset slightly so duplicate is visible */
				pos.x += 20.0f;
				pos.y += 20.0f;

				obs_sceneitem_set_pos(dup, &pos);
				obs_sceneitem_set_scale(dup, &sc);
				obs_sceneitem_set_rot(dup, rot);
				obs_sceneitem_set_crop(dup, &crop);

				/* Select the duplicate */
				obs_sceneitem_release(selected_item);
				selected_item = dup;
				obs_sceneitem_addref(selected_item);
				emit itemSelected(selected_item);
				emit itemTransformed();
			}
		}

	/* Ctrl+F: fit to canvas */
	} else if (key == Qt::Key_F && (mods & Qt::ControlModifier)) {
		FitToCanvas(selected_item, canvas_width, canvas_height);
		emit itemTransformed();

	/* Ctrl+R: stretch to canvas */
	} else if (key == Qt::Key_R && (mods & Qt::ControlModifier)) {
		StretchToCanvas(selected_item, canvas_width, canvas_height);
		emit itemTransformed();

	/* Ctrl+E: center on canvas */
	} else if (key == Qt::Key_E && (mods & Qt::ControlModifier)) {
		CenterOnCanvas(selected_item, canvas_width, canvas_height);
		emit itemTransformed();

	/* Ctrl+0: reset transform */
	} else if (key == Qt::Key_0 && (mods & Qt::ControlModifier)) {
		ResetTransform(selected_item);
		emit itemTransformed();

	} else {
		handled = false;
	}

	if (!handled)
		QWidget::keyPressEvent(event);
}

/* ---- Selection overlay drawing ---- */

void RecastPreviewWidget::DrawSelectionOverlay(RecastPreviewWidget *widget)
{
	obs_sceneitem_t *item = widget->selected_item;
	if (!item)
		return;

	/* Validate the sceneitem's source is still alive */
	obs_source_t *item_src = obs_sceneitem_get_source(item);
	if (!item_src)
		return;

	float rx, ry, rw, rh;
	widget->GetItemRect(item, rx, ry, rw, rh);
	if (rw <= 0 || rh <= 0)
		return;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color_param =
		gs_effect_get_param_by_name(solid, "color");

	/* Selection rectangle: green */
	struct vec4 green;
	vec4_set(&green, 0.0f, 1.0f, 0.0f, 1.0f);
	gs_effect_set_vec4(color_param, &green);

	while (gs_effect_loop(solid, "Solid")) {
		/* Draw bounding box outline */
		gs_render_start(true);
		gs_vertex2f(rx, ry);
		gs_vertex2f(rx + rw, ry);
		gs_vertex2f(rx + rw, ry + rh);
		gs_vertex2f(rx, ry + rh);
		gs_vertex2f(rx, ry);
		gs_render_stop(GS_LINESTRIP);
	}

	/* Handle squares: white filled */
	struct vec4 white;
	vec4_set(&white, 1.0f, 1.0f, 1.0f, 1.0f);
	gs_effect_set_vec4(color_param, &white);

	float hr = 4.0f; /* handle half-size in scene coords */
	/* Scale handle size based on canvas so they look reasonable */
	if (widget->canvas_width > 0)
		hr = (float)widget->canvas_width / 240.0f;
	if (hr < 3.0f) hr = 3.0f;
	if (hr > 12.0f) hr = 12.0f;

	struct { float x, y; } handles[8] = {
		{rx,          ry},
		{rx + rw / 2, ry},
		{rx + rw,     ry},
		{rx + rw,     ry + rh / 2},
		{rx + rw,     ry + rh},
		{rx + rw / 2, ry + rh},
		{rx,          ry + rh},
		{rx,          ry + rh / 2},
	};

	while (gs_effect_loop(solid, "Solid")) {
		for (int i = 0; i < 8; i++) {
			float hx = handles[i].x;
			float hy = handles[i].y;
			gs_render_start(true);
			gs_vertex2f(hx - hr, hy - hr);
			gs_vertex2f(hx + hr, hy - hr);
			gs_vertex2f(hx - hr, hy + hr);
			gs_vertex2f(hx + hr, hy + hr);
			gs_render_stop(GS_TRISTRIP);
		}
	}

	/* Handle outlines: dark border */
	struct vec4 dark;
	vec4_set(&dark, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_effect_set_vec4(color_param, &dark);

	while (gs_effect_loop(solid, "Solid")) {
		for (int i = 0; i < 8; i++) {
			float hx = handles[i].x;
			float hy = handles[i].y;
			gs_render_start(true);
			gs_vertex2f(hx - hr, hy - hr);
			gs_vertex2f(hx + hr, hy - hr);
			gs_vertex2f(hx + hr, hy + hr);
			gs_vertex2f(hx - hr, hy + hr);
			gs_vertex2f(hx - hr, hy - hr);
			gs_render_stop(GS_LINESTRIP);
		}
	}
}

void RecastPreviewWidget::paintEvent(QPaintEvent *)
{
	if (!display)
		CreateDisplay();
}

void RecastPreviewWidget::resizeEvent(QResizeEvent *)
{
	if (display) {
		QSize size = this->size() * this->devicePixelRatioF();
		obs_display_resize(display, size.width(), size.height());
	}
}

void RecastPreviewWidget::DrawCallback(void *param, uint32_t cx, uint32_t cy)
{
	auto *widget = static_cast<RecastPreviewWidget *>(param);
	if (!widget->scene_source || widget->canvas_width <= 0 ||
	    widget->canvas_height <= 0)
		return;

	/* Take a safe reference to the source for rendering.
	 * This prevents a crash if the source is freed on the UI thread
	 * while we are rendering on the graphics thread. */
	obs_source_t *src = obs_source_get_ref(widget->scene_source);
	if (!src)
		return;

	int x, y;
	float scale;
	GetScaleAndCenterPos(widget->canvas_width, widget->canvas_height,
			     (int)cx, (int)cy, x, y, scale);

	int newCX = int(scale * float(widget->canvas_width));
	int newCY = int(scale * float(widget->canvas_height));

	gs_viewport_push();
	gs_projection_push();
	gs_set_viewport(x, y, newCX, newCY);
	gs_ortho(0.0f, float(widget->canvas_width), 0.0f,
		 float(widget->canvas_height), -100.0f, 100.0f);

	/* Draw black canvas background so empty scenes show black
	 * against the dark gray surround (matching OBS main preview) */
	gs_effect_t *bg_solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *bg_color =
		gs_effect_get_param_by_name(bg_solid, "color");
	struct vec4 black;
	vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_effect_set_vec4(bg_color, &black);
	while (gs_effect_loop(bg_solid, "Solid")) {
		gs_render_start(true);
		gs_vertex2f(0.0f, 0.0f);
		gs_vertex2f((float)widget->canvas_width, 0.0f);
		gs_vertex2f(0.0f, (float)widget->canvas_height);
		gs_vertex2f((float)widget->canvas_width,
			    (float)widget->canvas_height);
		gs_render_stop(GS_TRISTRIP);
	}

	obs_source_video_render(src);

	/* Draw selection overlay if interactive */
	if (widget->interactive_scene && widget->selected_item)
		DrawSelectionOverlay(widget);

	gs_projection_pop();
	gs_viewport_pop();

	obs_source_release(src);
}
